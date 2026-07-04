/* static_analysis.c - ELF parsing for Debag
 *
 * Scans a binary to determine which syscalls it's likely to use, which
 * libraries it links, and whether it does networking, file writes,
 * exec, threads, or dlopen. This drives the seccomp filter generation:
 * syscalls the binary is expected to use get ALLOW; dangerous ones get
 * TRACE (fall through to ptrace); everything else gets KILL.
 *
 * We read the ELF dynamic symbol table (.dynsym) and match symbol names
 * against a table of libc functions → syscall numbers. This isn't a
 * full static analysis (that's undecidable), but it catches the common
 * case: a binary that calls connect() will need the connect syscall.
 *
 * For statically-linked binaries, we fall back to scanning the .text
 * section for syscall instruction patterns - crude but better than nothing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "debag.h"

/* ── Symbol → syscall mapping ──────────────────────────────────────
 * Maps libc function names to syscall numbers (x86_64). This is the
 * heuristic that drives seccomp filter generation. */
struct sym_to_syscall {
    const char *sym;        /* libc function name */
    int syscall;            /* __NR_* value */
    int category;           /* what kind of syscall */
};

#define CAT_SAFE     0   /* read, write, close - always allow */
#define CAT_NET      1   /* socket, connect, bind - network */
#define CAT_WRITE    2   /* open(O_WRONLY), unlink, rename - file write */
#define CAT_EXEC     3   /* execve - dangerous */
#define CAT_THREAD   4   /* clone, futex - threading */
#define CAT_MOUNT    5   /* mount, umount - filesystem */
#define CAT_TRACE    6   /* other syscalls that need ptrace inspection */

#include <sys/syscall.h>  /* __NR_* definitions */

static const struct sym_to_syscall sym_table[] = {
    /* Safe I/O */
    {"read",       __NR_read,       CAT_SAFE},
    {"write",      __NR_write,      CAT_SAFE},
    {"close",      __NR_close,      CAT_SAFE},
    {"fstat",      __NR_fstat,      CAT_SAFE},
    {"stat",       __NR_stat,       CAT_SAFE},
    {"lstat",      __NR_lstat,      CAT_SAFE},
    {"lseek",      __NR_lseek,      CAT_SAFE},
    {"mmap",       __NR_mmap,       CAT_SAFE},
    {"munmap",     __NR_munmap,     CAT_SAFE},
    {"mprotect",   __NR_mprotect,   CAT_SAFE},
    {"brk",        __NR_brk,        CAT_SAFE},
    {"exit",       __NR_exit,       CAT_SAFE},
    {"exit_group", __NR_exit_group, CAT_SAFE},
    {"rt_sigaction", __NR_rt_sigaction, CAT_SAFE},
    {"rt_sigprocmask", __NR_rt_sigprocmask, CAT_SAFE},
    {"getpid",     __NR_getpid,     CAT_SAFE},
    {"getuid",     __NR_getuid,     CAT_SAFE},
    {"getgid",     __NR_getgid,     CAT_SAFE},
    {"geteuid",    __NR_geteuid,    CAT_SAFE},
    {"getegid",    __NR_getegid,    CAT_SAFE},
    {"arch_prctl", __NR_arch_prctl, CAT_SAFE},
    {"set_tid_address", __NR_set_tid_address, CAT_SAFE},
    {"set_robust_list", __NR_set_robust_list, CAT_SAFE},
    {"rseq",       __NR_rseq,       CAT_SAFE},
    {"prlimit64",  __NR_prlimit64,  CAT_SAFE},
    {"getrandom",  __NR_getrandom,  CAT_SAFE},
    {"clock_gettime", __NR_clock_gettime, CAT_SAFE},
    {"clock_getres", __NR_clock_getres, CAT_SAFE},
    {"ioctl",      __NR_ioctl,      CAT_SAFE},
    {"pread64",    __NR_pread64,    CAT_SAFE},
    {"pwrite64",   __NR_pwrite64,   CAT_SAFE},
    {"fcntl",      __NR_fcntl,      CAT_SAFE},
    {"dup",        __NR_dup,        CAT_SAFE},
    {"dup2",       __NR_dup2,       CAT_SAFE},
    {"pipe",       __NR_pipe,       CAT_SAFE},
    {"poll",       __NR_poll,       CAT_SAFE},
    {"ppoll",      __NR_ppoll,      CAT_SAFE},
    {"epoll_create1", __NR_epoll_create1, CAT_SAFE},
    {"epoll_ctl",  __NR_epoll_ctl,  CAT_SAFE},
    {"epoll_wait", __NR_epoll_wait, CAT_SAFE},
    {"eventfd2",   __NR_eventfd2,   CAT_SAFE},
    {"timerfd_create", __NR_timerfd_create, CAT_SAFE},
    {"timerfd_settime", __NR_timerfd_settime, CAT_SAFE},
    {"futex",      __NR_futex,      CAT_SAFE},  /* futex is common, allow by default */
    {"sched_yield", __NR_sched_yield, CAT_SAFE},
    {"sched_getaffinity", __NR_sched_getaffinity, CAT_SAFE},

    /* Network */
    {"socket",     __NR_socket,     CAT_NET},
    {"connect",    __NR_connect,    CAT_NET},
    {"bind",       __NR_bind,       CAT_NET},
    {"listen",     __NR_listen,     CAT_NET},
    {"accept",     __NR_accept,     CAT_NET},
    {"accept4",    __NR_accept4,    CAT_NET},
    {"sendto",     __NR_sendto,     CAT_NET},
    {"recvfrom",   __NR_recvfrom,   CAT_NET},
    {"sendmsg",    __NR_sendmsg,    CAT_NET},
    {"recvmsg",    __NR_recvmsg,    CAT_NET},
    {"shutdown",   __NR_shutdown,   CAT_NET},
    {"getsockname", __NR_getsockname, CAT_NET},
    {"getpeername", __NR_getpeername, CAT_NET},
    {"setsockopt", __NR_setsockopt, CAT_NET},
    {"getsockopt", __NR_getsockopt, CAT_NET},

    /* File write */
    {"open",       __NR_open,       CAT_WRITE},
    {"openat",     __NR_openat,     CAT_WRITE},
    {"creat",      __NR_creat,      CAT_WRITE},
    {"unlink",     __NR_unlink,     CAT_WRITE},
    {"unlinkat",   __NR_unlinkat,   CAT_WRITE},
    {"rename",     __NR_rename,     CAT_WRITE},
    {"renameat",   __NR_renameat,   CAT_WRITE},
    {"mkdir",      __NR_mkdir,      CAT_WRITE},
    {"rmdir",      __NR_rmdir,      CAT_WRITE},
    {"truncate",   __NR_truncate,   CAT_WRITE},
    {"ftruncate",  __NR_ftruncate,  CAT_WRITE},
    {"chmod",      __NR_chmod,      CAT_WRITE},
    {"fchmod",     __NR_fchmod,     CAT_WRITE},
    {"chown",      __NR_chown,      CAT_WRITE},
    {"fchown",     __NR_fchown,     CAT_WRITE},
    {"symlink",    __NR_symlink,    CAT_WRITE},
    {"link",       __NR_link,       CAT_WRITE},
    {"writev",     __NR_writev,     CAT_WRITE},

    /* Exec */
    {"execve",     __NR_execve,     CAT_EXEC},
    {"execveat",   __NR_execveat,   CAT_EXEC},

    /* Threading */
    {"clone",      __NR_clone,      CAT_THREAD},
    {"clone3",     __NR_clone3,     CAT_THREAD},
    {"vfork",      __NR_vfork,      CAT_THREAD},

    /* Mount */
    {"mount",      __NR_mount,      CAT_MOUNT},
    {"umount",     __NR_umount2,    CAT_MOUNT},
    {"umount2",    __NR_umount2,    CAT_MOUNT},

    {NULL, 0, 0}
};

/* ── Read ELF dynamic symbols ────────────────────────────────────── */

static int find_syscall_for_sym(const char *sym, int *syscall_out, int *cat_out)
{
    for (const struct sym_to_syscall *e = sym_table; e->sym; e++) {
        if (strcmp(sym, e->sym) == 0) {
            *syscall_out = e->syscall;
            *cat_out = e->category;
            return 1;
        }
    }
    return 0;
}

/* ── ELF pretty-printing helpers (shared by static_analysis + REPLs) ────
 * Declared in debag.h. Implemented here so the static-db REPL can reuse
 * them without re-rolling its own tables. */

const char *debag_elf_section_type_name(uint32_t sh_type)
{
    switch (sh_type) {
    case SHT_NULL:          return "NULL";
    case SHT_PROGBITS:      return "PROGBITS";
    case SHT_SYMTAB:        return "SYMTAB";
    case SHT_STRTAB:        return "STRTAB";
    case SHT_RELA:          return "RELA";
    case SHT_HASH:          return "HASH";
    case SHT_DYNAMIC:       return "DYNAMIC";
    case SHT_NOTE:          return "NOTE";
    case SHT_NOBITS:        return "NOBITS";
    case SHT_REL:           return "REL";
    case SHT_DYNSYM:        return "DYNSYM";
    case SHT_INIT_ARRAY:    return "INIT_ARRAY";
    case SHT_FINI_ARRAY:    return "FINI_ARRAY";
#ifdef SHT_GNU_HASH
    case SHT_GNU_HASH:      return "GNU_HASH";
#endif
#ifdef SHT_GNU_verdef
    case SHT_GNU_verdef:    return "GNU_VERDEF";
#endif
#ifdef SHT_GNU_verneed
    case SHT_GNU_verneed:   return "GNU_VERNEED";
#endif
#ifdef SHT_GNU_versym
    case SHT_GNU_versym:    return "GNU_VERSYM";
#endif
    default:                return "OTHER";
    }
}

/* Build a one-letter section flag string in the style of readelf.
 * Caller provides a buffer of at least 10 bytes. */
void debag_elf_section_flags_str(uint64_t flags, char *buf, size_t bufsz)
{
    size_t i = 0;
    if (i < bufsz - 1 && (flags & SHF_WRITE))          buf[i++] = 'W';
    if (i < bufsz - 1 && (flags & SHF_ALLOC))          buf[i++] = 'A';
    if (i < bufsz - 1 && (flags & SHF_EXECINSTR))      buf[i++] = 'X';
    if (i < bufsz - 1 && (flags & SHF_MERGE))          buf[i++] = 'M';
    if (i < bufsz - 1 && (flags & SHF_STRINGS))        buf[i++] = 'S';
    if (i < bufsz - 1 && (flags & SHF_INFO_LINK))      buf[i++] = 'I';
    if (i < bufsz - 1 && (flags & SHF_LINK_ORDER))     buf[i++] = 'L';
    if (i < bufsz - 1 && (flags & SHF_GROUP))          buf[i++] = 'G';
    if (i < bufsz - 1 && (flags & SHF_TLS))            buf[i++] = 'T';
    buf[i] = '\0';
}

const char *debag_elf_symbol_type_name(int st_type)
{
    switch (st_type) {
    case STT_NOTYPE:  return "NOTYPE";
    case STT_OBJECT:  return "OBJECT";
    case STT_FUNC:    return "FUNC";
    case STT_SECTION: return "SECTION";
    case STT_FILE:    return "FILE";
    case STT_COMMON:  return "COMMON";
    case STT_TLS:     return "TLS";
#ifdef STT_GNU_IFUNC
    case STT_GNU_IFUNC: return "GNU_IFUNC";
#endif
    default:          return "?";
    }
}

const char *debag_elf_symbol_bind_name(int st_bind)
{
    switch (st_bind) {
    case STB_LOCAL:  return "LOCAL";
    case STB_GLOBAL: return "GLOBAL";
    case STB_WEAK:   return "WEAK";
#ifdef STB_GNU_UNIQUE
    case STB_GNU_UNIQUE: return "GNU_UNIQUE";
#endif
    default:         return "?";
    }
}

const char *debag_elf_segment_type_name(uint32_t p_type)
{
    switch (p_type) {
    case PT_NULL:    return "NULL";
    case PT_LOAD:    return "LOAD";
    case PT_DYNAMIC: return "DYNAMIC";
    case PT_INTERP:  return "INTERP";
    case PT_NOTE:    return "NOTE";
    case PT_SHLIB:   return "SHLIB";
    case PT_PHDR:    return "PHDR";
    case PT_TLS:     return "TLS";
#ifdef PT_GNU_EH_FRAME
    case PT_GNU_EH_FRAME:  return "GNU_EH_FRAME";
#endif
#ifdef PT_GNU_STACK
    case PT_GNU_STACK:     return "GNU_STACK";
#endif
#ifdef PT_GNU_RELRO
    case PT_GNU_RELRO:     return "GNU_RELRO";
#endif
#ifdef PT_GNU_PROPERTY
    case PT_GNU_PROPERTY:  return "GNU_PROPERTY";
#endif
    default:        return "OTHER";
    }
}

/* Pretty-printer for ELF relocation type codes. Only the common
 * x86-64 / x86 / aarch64 types are named; anything else falls through
 * to "OTHER". Used by the static-db REPL's `ii` to label each GOT
 * slot (e.g. R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT). */
const char *debag_elf_reloc_type_name(uint32_t r_type)
{
#if defined(R_X86_64_64)
    switch (r_type) {
    case R_X86_64_NONE:        return "R_X86_64_NONE";
    case R_X86_64_64:          return "R_X86_64_64";
    case R_X86_64_PC32:        return "R_X86_64_PC32";
    case R_X86_64_GOT32:       return "R_X86_64_GOT32";
    case R_X86_64_PLT32:       return "R_X86_64_PLT32";
    case R_X86_64_COPY:        return "R_X86_64_COPY";
    case R_X86_64_GLOB_DAT:    return "R_X86_64_GLOB_DAT";
    case R_X86_64_JUMP_SLOT:   return "R_X86_64_JUMP_SLOT";
    case R_X86_64_RELATIVE:    return "R_X86_64_RELATIVE";
    case R_X86_64_GOTPCREL:    return "R_X86_64_GOTPCREL";
    case R_X86_64_32:          return "R_X86_64_32";
    case R_X86_64_32S:         return "R_X86_64_32S";
    case R_X86_64_16:          return "R_X86_64_16";
    case R_X86_64_PC16:        return "R_X86_64_PC16";
    case R_X86_64_8:           return "R_X86_64_8";
    case R_X86_64_PC8:         return "R_X86_64_PC8";
    case R_X86_64_PC64:        return "R_X86_64_PC64";
    default:                   break;
    }
#endif
#if defined(R_AARCH64_NONE) && !defined(R_X86_64_64)
    switch (r_type) {
    case R_AARCH64_NONE:           return "R_AARCH64_NONE";
    case R_AARCH64_ABS64:          return "R_AARCH64_ABS64";
    case R_AARCH64_GLOB_DAT:       return "R_AARCH64_GLOB_DAT";
    case R_AARCH64_JUMP_SLOT:      return "R_AARCH64_JUMP_SLOT";
    case R_AARCH64_RELATIVE:       return "R_AARCH64_RELATIVE";
    default:                       break;
    }
#endif
    return "OTHER";
}

/* Resolve e_machine to a short arch name. Used by debag_analyze() to
 * populate analysis->arch_name, and by the static-db REPL to pick a
 * Capstone cs_arch. */
static const char *elf_machine_name(uint16_t e_machine)
{
    switch (e_machine) {
    case EM_X86_64: return "x86-64";
    case EM_386:    return "x86";
    case EM_AARCH64: return "aarch64";
    case EM_ARM:    return "arm";
    case EM_RISCV:  return "riscv";
    case EM_PPC64:  return "ppc64";
    case EM_S390:   return "s390x";
    default:        return "unknown";
    }
}

static const char *elf_type_name(uint16_t e_type)
{
    switch (e_type) {
    case ET_EXEC: return "EXEC";
    case ET_DYN:  return "DYN";
    case ET_CORE: return "CORE";
    case ET_REL:  return "REL";
    default:      return "?";
    }
}

/* Add a symbol to the analysis's symbols array. Dedups on (name, vaddr)
 * so .symtab + .dynsym merges don't double-count. */
static void analysis_add_symbol(debag_analysis_t *a, const char *name,
                                uint64_t vaddr, uint64_t size,
                                int type, int bind, int is_import)
{
    if (!name || !name[0]) return;

    /* Dedup: .dynsym often overlaps with .symtab. */
    for (size_t i = 0; i < a->symbol_count; i++) {
        if (a->symbols[i].vaddr == vaddr &&
            a->symbols[i].name &&
            strcmp(a->symbols[i].name, name) == 0)
            return;
    }

    /* Grow the array in 16-entry slabs when we run out of room. */
    size_t cap = (a->symbols == NULL) ? 0 :
                 ((a->symbol_count + 15) & ~(size_t)15);
    if (a->symbol_count + 1 > cap) {
        cap = a->symbol_count + 16;
        debag_elf_symbol_t *ns = realloc(a->symbols, cap * sizeof(*ns));
        if (!ns) return;
        a->symbols = ns;
    }
    debag_elf_symbol_t *s = &a->symbols[a->symbol_count++];
    s->name = strdup(name);
    s->vaddr = vaddr;
    s->size = size;
    s->type = type;
    s->bind = bind;
    s->is_import = is_import;
}

debag_analysis_t *debag_analyze(const char *binary_path)
{
    if (!binary_path) return NULL;

    int fd = open(binary_path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        close(fd);
        return NULL;
    }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return NULL;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)map;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        munmap(map, st.st_size);
        return NULL;  /* not an ELF */
    }

    debag_analysis_t *a = calloc(1, sizeof(*a));
    a->binary_path = strdup(binary_path);

    /* ELF metadata for the interactive REPLs */
    a->entry_point = ehdr->e_entry;
    a->bits = (ehdr->e_ident[EI_CLASS] == ELFCLASS64) ? 64 : 32;
    a->is_big_endian = (ehdr->e_ident[EI_DATA] == ELFDATA2MSB) ? 1 : 0;
    a->arch_name = strdup(elf_machine_name(ehdr->e_machine));
    a->binary_type = strdup(elf_type_name(ehdr->e_type));

    /* Build the section table descriptor array. Section names live in
     * the .shstrtab section, indexed by ehdr->e_shstrndx. */
    Elf64_Shdr *shdrs = (Elf64_Shdr *)((char *)map + ehdr->e_shoff);
    const char *shstrtab = NULL;
    if (ehdr->e_shstrndx != SHN_UNDEF &&
        ehdr->e_shstrndx < (unsigned)ehdr->e_shnum) {
        Elf64_Shdr *shstr_sh = &shdrs[ehdr->e_shstrndx];
        shstrtab = (const char *)map + shstr_sh->sh_offset;
    }

    if (ehdr->e_shnum > 0) {
        a->sections = calloc(ehdr->e_shnum, sizeof(*a->sections));
        if (a->sections) {
            for (int i = 0; i < ehdr->e_shnum; i++) {
                debag_elf_section_t *s = &a->sections[a->section_count++];
                s->name  = strdup(shstrtab ? shstrtab + shdrs[i].sh_name : "");
                s->vaddr = shdrs[i].sh_addr;
                s->offset= shdrs[i].sh_offset;
                s->size  = shdrs[i].sh_size;
                s->type  = shdrs[i].sh_type;
                s->flags = shdrs[i].sh_flags;
            }
        }
    }

    /* Build the program-header / segment descriptor array. */
    if (ehdr->e_phnum > 0) {
        Elf64_Phdr *phdrs = (Elf64_Phdr *)((char *)map + ehdr->e_phoff);
        a->segments = calloc(ehdr->e_phnum, sizeof(*a->segments));
        if (a->segments) {
            for (int i = 0; i < ehdr->e_phnum; i++) {
                debag_elf_segment_t *s = &a->segments[a->segment_count++];
                s->type   = phdrs[i].p_type;
                s->flags  = phdrs[i].p_flags;
                s->vaddr  = phdrs[i].p_vaddr;
                s->offset = phdrs[i].p_offset;
                s->filesz = phdrs[i].p_filesz;
                s->memsz  = phdrs[i].p_memsz;
            }
        }
    }

    /* Walk section headers to find .dynsym and .dynstr */
    Elf64_Shdr *dynsym_sh = NULL, *dynstr_sh = NULL;
    Elf64_Shdr *dynamic_sh = NULL;
    Elf64_Shdr *symtab_sh = NULL, *strtab_sh = NULL;

    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM)
            dynsym_sh = &shdrs[i];
        else if (shdrs[i].sh_type == SHT_SYMTAB)
            symtab_sh = &shdrs[i];
        else if (shdrs[i].sh_type == SHT_DYNAMIC)
            dynamic_sh = &shdrs[i];
    }

    /* Find dynstr via dynsym's sh_link */
    if (dynsym_sh && dynsym_sh->sh_link < (unsigned)ehdr->e_shnum)
        dynstr_sh = &shdrs[dynsym_sh->sh_link];

    /* Find strtab via symtab's sh_link (for the .symtab case) */
    if (symtab_sh && symtab_sh->sh_link < (unsigned)ehdr->e_shnum)
        strtab_sh = &shdrs[symtab_sh->sh_link];

    /* Collect defined + imported symbols from .dynsym */
    if (dynsym_sh && dynstr_sh) {
        Elf64_Sym *syms = (Elf64_Sym *)((char *)map + dynsym_sh->sh_offset);
        size_t sym_count = dynsym_sh->sh_size / sizeof(Elf64_Sym);
        const char *strtab = (const char *)map + dynstr_sh->sh_offset;

        for (size_t i = 0; i < sym_count; i++) {
            const char *name = strtab + syms[i].st_name;
            if (!name[0]) continue;
            int is_import = (syms[i].st_shndx == SHN_UNDEF);
            analysis_add_symbol(a, name,
                                syms[i].st_value,
                                syms[i].st_size,
                                ELF64_ST_TYPE(syms[i].st_info),
                                ELF64_ST_BIND(syms[i].st_info),
                                is_import);
        }
    }

    /* Also collect symbols from .symtab (often stripped from shipped
     * binaries, but useful when present - e.g. local binaries). */
    if (symtab_sh && strtab_sh) {
        Elf64_Sym *syms = (Elf64_Sym *)((char *)map + symtab_sh->sh_offset);
        size_t sym_count = symtab_sh->sh_size / sizeof(Elf64_Sym);
        const char *strtab = (const char *)map + strtab_sh->sh_offset;

        for (size_t i = 0; i < sym_count; i++) {
            const char *name = strtab + syms[i].st_name;
            if (!name[0]) continue;
            int is_import = (syms[i].st_shndx == SHN_UNDEF);
            analysis_add_symbol(a, name,
                                syms[i].st_value,
                                syms[i].st_size,
                                ELF64_ST_TYPE(syms[i].st_info),
                                ELF64_ST_BIND(syms[i].st_info),
                                is_import);
        }
    }

    /* Walk dynamic symbols */
    int *allowed = NULL, *traced = NULL;
    size_t allowed_cap = 64, allowed_cnt = 0;
    size_t traced_cap = 32, traced_cnt = 0;
    allowed = malloc(allowed_cap * sizeof(int));
    traced = malloc(traced_cap * sizeof(int));

    if (dynsym_sh && dynstr_sh) {
        Elf64_Sym *syms = (Elf64_Sym *)((char *)map + dynsym_sh->sh_offset);
        size_t sym_count = dynsym_sh->sh_size / sizeof(Elf64_Sym);
        const char *strtab = (const char *)map + dynstr_sh->sh_offset;

        for (size_t i = 0; i < sym_count; i++) {
            if (ELF64_ST_TYPE(syms[i].st_info) != STT_FUNC)
                continue;
            if (syms[i].st_shndx == SHN_UNDEF)
                continue;  /* undefined = imported */

            const char *name = strtab + syms[i].st_name;
            if (!name[0]) continue;

            int syscall_nr, cat;
            if (find_syscall_for_sym(name, &syscall_nr, &cat)) {
                switch (cat) {
                case CAT_SAFE:
                    if (allowed_cnt < allowed_cap)
                        allowed[allowed_cnt++] = syscall_nr;
                    break;
                case CAT_NET:
                    a->has_network = 1;
                    if (traced_cnt < traced_cap)
                        traced[traced_cnt++] = syscall_nr;
                    break;
                case CAT_WRITE:
                    a->has_file_write = 1;
                    if (traced_cnt < traced_cap)
                        traced[traced_cnt++] = syscall_nr;
                    break;
                case CAT_EXEC:
                    a->has_exec = 1;
                    if (traced_cnt < traced_cap)
                        traced[traced_cnt++] = syscall_nr;
                    break;
                case CAT_THREAD:
                    a->has_threads = 1;
                    if (allowed_cnt < allowed_cap)
                        allowed[allowed_cnt++] = syscall_nr;
                    break;
                case CAT_MOUNT:
                    a->has_mount = 1;
                    if (traced_cnt < traced_cap)
                        traced[traced_cnt++] = syscall_nr;
                    break;
                }
            }

            /* Check for dlopen */
            if (strcmp(name, "dlopen") == 0 || strcmp(name, "__libc_dlopen_mode") == 0)
                a->has_dlopen = 1;
        }
    }

    a->is_dynamic = (dynsym_sh != NULL);
    a->allowed_syscalls = allowed;
    a->allowed_count = allowed_cnt;
    a->traced_syscalls = traced;
    a->traced_count = traced_cnt;

    /* Walk .dynamic to find linked libraries */
    if (dynamic_sh) {
        Elf64_Dyn *dyn = (Elf64_Dyn *)((char *)map + dynamic_sh->sh_offset);
        size_t dyn_count = dynamic_sh->sh_size / sizeof(Elf64_Dyn);

        /* Find DT_STRTAB */
        const char *dynstr = NULL;
        for (size_t i = 0; i < dyn_count; i++) {
            if (dyn[i].d_tag == DT_STRTAB) {
                /* DT_STRTAB is a virtual address - find it via program headers */
                Elf64_Phdr *phdrs = (Elf64_Phdr *)((char *)map + ehdr->e_phoff);
                for (int j = 0; j < ehdr->e_phnum; j++) {
                    if (phdrs[j].p_type == PT_LOAD &&
                        dyn[i].d_un.d_ptr >= phdrs[j].p_vaddr &&
                        dyn[i].d_un.d_ptr < phdrs[j].p_vaddr + phdrs[j].p_memsz) {
                        dynstr = (const char *)map + phdrs[j].p_offset +
                                 (dyn[i].d_un.d_ptr - phdrs[j].p_vaddr);
                        break;
                    }
                }
                break;
            }
        }

        if (dynstr) {
            char **libs = NULL;
            size_t lib_cap = 16, lib_cnt = 0;
            libs = malloc(lib_cap * sizeof(char *));

            for (size_t i = 0; i < dyn_count; i++) {
                if (dyn[i].d_tag == DT_NEEDED) {
                    const char *libname = dynstr + dyn[i].d_un.d_val;
                    if (lib_cnt < lib_cap)
                        libs[lib_cnt++] = strdup(libname);
                    if (strstr(libname, "libdl") || strstr(libname, "libdl.so"))
                        a->has_dlopen = 1;
                }
            }

            a->libs = libs;
            a->lib_count = lib_cnt;
        }
    }

    /* Parse dynamic relocations (PLT + GOT) so `ii` can show GOT slot
     * addresses and `s <import_name>` can seek to them. Walks .dynamic
     * for DT_JMPREL/DT_REL/DT_RELA + DT_SYMTAB, then for each reloc
     * entry resolves the symbol name via DT_SYMTAB[sym_idx] and stores
     * {name, r_offset, type, is_rela, is_plt} in a->relocs. */
    if (dynamic_sh && dynsym_sh && dynstr_sh) {
        Elf64_Dyn *dyn = (Elf64_Dyn *)((char *)map + dynamic_sh->sh_offset);
        size_t dyn_count = dynamic_sh->sh_size / sizeof(Elf64_Dyn);
        Elf64_Phdr *phdrs = (Elf64_Phdr *)((char *)map + ehdr->e_phoff);

        /* .dynsym symbols: needed to resolve sym_idx -> name. */
        Elf64_Sym *dynsyms = (Elf64_Sym *)((char *)map + dynsym_sh->sh_offset);
        size_t dynsym_count = dynsym_sh->sh_size / sizeof(Elf64_Sym);
        const char *dynstr = (const char *)map + dynstr_sh->sh_offset;

        /* Collect DT_ entries we care about. d_un.d_val is a vaddr for
         * DT_JMPREL/DT_REL/DT_RELA/DT_SYMTAB, a byte size for the *SZ
         * entries, and DT_REL/DT_RELA for DT_PLTREL. */
        uint64_t jmprel_va = 0, rel_va = 0, rela_va = 0, symtab_va = 0;
        uint64_t jmprel_sz = 0, rel_sz = 0, rela_sz = 0, syment = 0;
        int pltrel_type = DT_RELA;   /* default per the gABI */
        int have_jmprel = 0, have_rel = 0, have_rela = 0, have_symtab = 0;

        for (size_t i = 0; i < dyn_count; i++) {
            switch (dyn[i].d_tag) {
            case DT_JMPREL:    jmprel_va = dyn[i].d_un.d_ptr; have_jmprel = 1; break;
            case DT_PLTRELSZ:  jmprel_sz = dyn[i].d_un.d_val; break;
            case DT_PLTREL:    pltrel_type = (int)dyn[i].d_un.d_val; break;
            case DT_REL:       rel_va = dyn[i].d_un.d_ptr; have_rel = 1; break;
            case DT_RELSZ:     rel_sz = dyn[i].d_un.d_val; break;
            case DT_RELA:      rela_va = dyn[i].d_un.d_ptr; have_rela = 1; break;
            case DT_RELASZ:    rela_sz = dyn[i].d_un.d_val; break;
            case DT_SYMTAB:    symtab_va = dyn[i].d_un.d_ptr; have_symtab = 1; break;
            case DT_SYMENT:    syment = dyn[i].d_un.d_val; break;
            default:           break;
            }
        }
        (void)syment;   /* informational; we use sizeof(Elf64_Sym) */

        /* Translate a vaddr into a file offset via PT_LOAD segments.
         * Mirrors the DT_STRTAB translation above; returns NULL on
         * miss. We need this because DT_JMPREL/DT_REL/DT_RELA point
         * at virtual addresses, not file offsets. */
        const void *vaddr_to_ptr(uint64_t va) {
            for (int j = 0; j < ehdr->e_phnum; j++) {
                if (phdrs[j].p_type != PT_LOAD) continue;
                if (va >= phdrs[j].p_vaddr &&
                    va < phdrs[j].p_vaddr + phdrs[j].p_filesz)
                    return (const char *)map + phdrs[j].p_offset +
                           (va - phdrs[j].p_vaddr);
            }
            return NULL;
        }

        /* Resolve a symbol index to a name + st_value using DT_SYMTAB
         * if available (handles stripped-section cases), else fall
         * back to the .dynsym section we already located. */
        const Elf64_Sym *resolve_sym(size_t idx, const char **name_out,
                                     uint64_t *val_out) {
            const Elf64_Sym *sym = NULL;
            if (have_symtab) {
                const char *base = (const char *)vaddr_to_ptr(symtab_va);
                if (base) sym = (const Elf64_Sym *)base + idx;
            }
            if (!sym && idx < dynsym_count)
                sym = &dynsyms[idx];
            if (!sym) return NULL;
            /* DT_STRTAB and .dynstr's sh_offset point at the same
             * string table in normal binaries. Prefer the offset we
             * already have on the section. */
            *name_out = dynstr + sym->st_name;
            *val_out = sym->st_value;
            return sym;
        }

        /* Append one reloc to a->relocs (growing in 16-entry slabs,
         * matching analysis_add_symbol's growth policy). */
        void add_reloc(const char *name, uint64_t vaddr, uint64_t addend,
                       uint32_t type, int is_rela, int is_plt) {
            size_t cap = (a->relocs == NULL) ? 0 :
                         ((a->reloc_count + 15) & ~(size_t)15);
            if (a->reloc_count + 1 > cap) {
                cap = a->reloc_count + 16;
                debag_elf_reloc_t *nr = realloc(a->relocs,
                                                cap * sizeof(*nr));
                if (!nr) return;
                a->relocs = nr;
            }
            debag_elf_reloc_t *r = &a->relocs[a->reloc_count++];
            r->sym_name  = name ? strdup(name) : NULL;
            r->vaddr     = vaddr;
            r->addend    = addend;
            r->type      = type;
            r->is_rela   = is_rela;
            r->is_plt    = is_plt;
            r->sym_value = 0;
        }

        /* Walk one relocation table. `base` is the file-mapped start
         * of the table, `count` is the number of entries, `is_rela`
         * selects Elf64_Rela vs Elf64_Rel, `is_plt` tags the source
         * (DT_JMPREL = PLT/GOT.PLT, DT_REL/DT_RELA = regular GOT). */
        void walk_table(const void *base, size_t bytes, int is_rela,
                        int is_plt) {
            if (!base || bytes == 0) return;
            size_t ent_sz = is_rela ? sizeof(Elf64_Rela) : sizeof(Elf64_Rel);
            size_t count = bytes / ent_sz;
            for (size_t i = 0; i < count; i++) {
                uint64_t r_offset, r_info, r_addend = 0;
                if (is_rela) {
                    const Elf64_Rela *e = (const Elf64_Rela *)base + i;
                    r_offset = e->r_offset;
                    r_info   = e->r_info;
                    r_addend = e->r_addend;
                } else {
                    const Elf64_Rel *e = (const Elf64_Rel *)base + i;
                    r_offset = e->r_offset;
                    r_info   = e->r_info;
                }
                size_t sym_idx = ELF64_R_SYM(r_info);
                uint32_t type  = ELF64_R_TYPE(r_info);
                const char *name = NULL;
                uint64_t val = 0;
                const Elf64_Sym *sym = resolve_sym(sym_idx, &name, &val);
                (void)sym;
                add_reloc(name && name[0] ? name : NULL,
                          r_offset, r_addend, type, is_rela, is_plt);
                /* Patch the sym_value into the just-added reloc. */
                if (a->reloc_count > 0)
                    a->relocs[a->reloc_count - 1].sym_value = val;
            }
        }

        /* PLT relocations: type comes from DT_PLTREL (DT_REL or
         * DT_RELA). */
        if (have_jmprel) {
            const void *base = vaddr_to_ptr(jmprel_va);
            int is_rela = (pltrel_type == DT_RELA);
            walk_table(base, jmprel_sz, is_rela, /*is_plt=*/1);
        }
        /* Regular relocations: DT_RELA (RELA form) and DT_REL (REL
         * form) are mutually exclusive on x86-64 but both can appear
         * on other arches; walk whichever are present. */
        if (have_rela) {
            const void *base = vaddr_to_ptr(rela_va);
            walk_table(base, rela_sz, /*is_rela=*/1, /*is_plt=*/0);
        }
        if (have_rel) {
            const void *base = vaddr_to_ptr(rel_va);
            walk_table(base, rel_sz, /*is_rela=*/0, /*is_plt=*/0);
        }
    }

    munmap(map, st.st_size);
    return a;
}

void debag_analysis_free(debag_analysis_t *a)
{
    if (!a) return;
    free(a->binary_path);
    free(a->arch_name);
    free(a->binary_type);
    free(a->allowed_syscalls);
    free(a->traced_syscalls);
    if (a->libs) {
        for (size_t i = 0; i < a->lib_count; i++)
            free(a->libs[i]);
        free(a->libs);
    }
    if (a->sections) {
        for (size_t i = 0; i < a->section_count; i++)
            free(a->sections[i].name);
        free(a->sections);
    }
    free(a->segments);
    if (a->symbols) {
        for (size_t i = 0; i < a->symbol_count; i++)
            free(a->symbols[i].name);
        free(a->symbols);
    }
    if (a->relocs) {
        for (size_t i = 0; i < a->reloc_count; i++)
            free(a->relocs[i].sym_name);
        free(a->relocs);
    }
    free(a);
}

void debag_analysis_print(const debag_analysis_t *a, FILE *out)
{
    if (!a) {
        fprintf(out, "debag: no analysis\n");
        return;
    }

    fprintf(out, "=== Debag Static Analysis ===\n");
    fprintf(out, "Binary:     %s\n", a->binary_path);
    if (a->arch_name)
        fprintf(out, "Arch:       %s (%d-bit, %s endian)\n",
                a->arch_name, a->bits,
                a->is_big_endian ? "big" : "little");
    if (a->binary_type)
        fprintf(out, "Type:       %s\n", a->binary_type);
    if (a->entry_point || a->bits)
        fprintf(out, "Entry:      0x%016llx\n", (unsigned long long)a->entry_point);
    fprintf(out, "Dynamic:    %s\n", a->is_dynamic ? "yes" : "no (static)");
    fprintf(out, "Network:    %s\n", a->has_network ? "yes" : "no");
    fprintf(out, "File write: %s\n", a->has_file_write ? "yes" : "no");
    fprintf(out, "Exec:       %s\n", a->has_exec ? "yes" : "no");
    fprintf(out, "dlopen:     %s\n", a->has_dlopen ? "yes (dynamic loading - may bypass filter)" : "no");
    fprintf(out, "Threads:    %s\n", a->has_threads ? "yes" : "no");
    fprintf(out, "Mount:      %s\n", a->has_mount ? "yes" : "no");

    fprintf(out, "\nLinked libraries (%zu):\n", a->lib_count);
    for (size_t i = 0; i < a->lib_count; i++)
        fprintf(out, "  %s\n", a->libs[i]);

    fprintf(out, "\nAllowed syscalls (seccomp fast path, %zu):\n", a->allowed_count);
    for (size_t i = 0; i < a->allowed_count; i++)
        fprintf(out, "  %d\n", a->allowed_syscalls[i]);

    fprintf(out, "\nTraced syscalls (ptrace slow path, %zu):\n", a->traced_count);
    for (size_t i = 0; i < a->traced_count; i++)
        fprintf(out, "  %d\n", a->traced_syscalls[i]);

    if (a->has_dlopen) {
        fprintf(out, "\n⚠ Warning: binary uses dlopen. The seccomp filter may be\n");
        fprintf(out, "  bypassed by dynamically loaded code. The ptrace slow path\n");
        fprintf(out, "  will catch dangerous syscalls, but performance will degrade.\n");
    }
}
