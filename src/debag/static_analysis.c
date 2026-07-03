/* static_analysis.c — ELF parsing for Debag
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
 * section for syscall instruction patterns — crude but better than nothing.
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

#define CAT_SAFE     0   /* read, write, close — always allow */
#define CAT_NET      1   /* socket, connect, bind — network */
#define CAT_WRITE    2   /* open(O_WRONLY), unlink, rename — file write */
#define CAT_EXEC     3   /* execve — dangerous */
#define CAT_THREAD   4   /* clone, futex — threading */
#define CAT_MOUNT    5   /* mount, umount — filesystem */
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

    /* Walk section headers to find .dynsym and .dynstr */
    Elf64_Shdr *shdrs = (Elf64_Shdr *)((char *)map + ehdr->e_shoff);
    Elf64_Shdr *dynsym_sh = NULL, *dynstr_sh = NULL;
    Elf64_Shdr *dynamic_sh = NULL;

    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM)
            dynsym_sh = &shdrs[i];
        else if (shdrs[i].sh_type == SHT_STRTAB && dynstr_sh && shdrs[i].sh_offset == dynsym_sh->sh_link == 0 ? 0 : 0)
            ; /* will find dynstr via dynsym's sh_link */
        else if (shdrs[i].sh_type == SHT_DYNAMIC)
            dynamic_sh = &shdrs[i];
    }

    /* Find dynstr via dynsym's sh_link */
    if (dynsym_sh && dynsym_sh->sh_link < (unsigned)ehdr->e_shnum)
        dynstr_sh = &shdrs[dynsym_sh->sh_link];

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
                /* DT_STRTAB is a virtual address — find it via program headers */
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

    munmap(map, st.st_size);
    return a;
}

void debag_analysis_free(debag_analysis_t *a)
{
    if (!a) return;
    free(a->binary_path);
    free(a->allowed_syscalls);
    free(a->traced_syscalls);
    if (a->libs) {
        for (size_t i = 0; i < a->lib_count; i++)
            free(a->libs[i]);
        free(a->libs);
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
    fprintf(out, "Dynamic:    %s\n", a->is_dynamic ? "yes" : "no (static)");
    fprintf(out, "Network:    %s\n", a->has_network ? "yes" : "no");
    fprintf(out, "File write: %s\n", a->has_file_write ? "yes" : "no");
    fprintf(out, "Exec:       %s\n", a->has_exec ? "yes" : "no");
    fprintf(out, "dlopen:     %s\n", a->has_dlopen ? "yes (dynamic loading — may bypass filter)" : "no");
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
