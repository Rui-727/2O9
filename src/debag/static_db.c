/* static_db.c - rizin-inspired read-only ELF REPL for Debag.
 *
 * `209 debag --static-db -- <binary>` drops into an interactive prompt
 * modelled on rizin (librz/core). The design is deliberately small: a
 * longest-prefix command dispatcher, a single `uint64_t seek` cursor,
 * `pread()` straight off the binary's fd for every read. No block cache,
 * no IO layer, no tree-sitter shell parser. One line in, one command out.
 *
 * The ELF data (sections, segments, symbols, entry point, arch, bits,
 * endianness) is parsed once by debag_analyze() in static_analysis.c and
 * handed to us as a `debag_analysis_t`. We only do read-only inspection
 * here: list, hexdump, disassemble, seek. Disassembly is gated behind
 * libcapstone (optional); without it `pd`/`pdd` print a hint and return.
 *
 * See /home/z/my-project/tool-results/rizin-research.md for the source
 * study that informed this design.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <elf.h>

#include "debag.h"

#ifdef HAVE_CAPSTONE
#include <capstone/capstone.h>
#endif

/* ── REPL state ───────────────────────────────────────────────────
 *
 * Mirrors rizin's RzCore in the parts we care about: an analysis
 * pointer (the parsed ELF), an fd for direct pread(), a single
 * `offset` cursor (the seek position), and a `quit` flag the `q`
 * handler sets to break the outer loop.
 *
 * Seek history (rizin's `u`/`U`) is a pair of bounded stacks:
 * `seek_hist` records every position we left via `s <addr>` (oldest
 * to newest, capped at SEEK_HIST_MAX entries — older entries shift
 * out), `redo_stack` records positions we left via `u` so `U` can
 * re-apply them. Any new `s <addr>` clears `redo_stack`, matching
 * rizin's rz_core_seek_undo semantics. */
#define SEEK_HIST_MAX 32

typedef struct {
    debag_analysis_t *a;
    int fd;
    uint64_t offset;        /* current seek, mirrors rizin's core->offset */
    int quit;
    uint64_t seek_hist[SEEK_HIST_MAX];
    size_t   seek_hist_count;
    uint64_t redo_stack[SEEK_HIST_MAX];
    size_t   redo_count;
} repl_t;

/* ── Forward declarations for the command table ─────────────────── */
static void cmd_info(repl_t *r, int argc, char **argv);
static void cmd_info_sections(repl_t *r, int argc, char **argv);
static void cmd_info_segments(repl_t *r, int argc, char **argv);
static void cmd_info_symbols(repl_t *r, int argc, char **argv);
static void cmd_info_entry(repl_t *r, int argc, char **argv);
static void cmd_info_imports(repl_t *r, int argc, char **argv);
static void cmd_info_strings(repl_t *r, int argc, char **argv);
static void cmd_print_hex(repl_t *r, int argc, char **argv);
static void cmd_print_hexw(repl_t *r, int argc, char **argv);
static void cmd_print_hexq(repl_t *r, int argc, char **argv);
static void cmd_print_string(repl_t *r, int argc, char **argv);
static void cmd_seek(repl_t *r, int argc, char **argv);
static void cmd_seek_undo(repl_t *r, int argc, char **argv);
static void cmd_seek_redo(repl_t *r, int argc, char **argv);
static void cmd_seek_history(repl_t *r, int argc, char **argv);
static void cmd_print_disasm(repl_t *r, int argc, char **argv);
static void cmd_print_disasm_at(repl_t *r, int argc, char **argv);
static void cmd_help(repl_t *r, int argc, char **argv);
static void cmd_quit(repl_t *r, int argc, char **argv);

/* Command table. Sorted in dispatch order — the longest-prefix matcher
 * walks this table once per attempt and prefers the longest match, so
 * ordering doesn't matter for correctness, but keeping related commands
 * grouped makes the help output read naturally. */
static const struct {
    const char *name;
    const char *summary;
    void (*fn)(repl_t *, int, char **);
} cmds[] = {
    {"i",     "print binary info",                     cmd_info},
    {"iS",    "list sections",                         cmd_info_sections},
    {"iSS",   "list segments / program headers",       cmd_info_segments},
    {"is",    "list symbols",                          cmd_info_symbols},
    {"ie",    "list entry points",                     cmd_info_entry},
    {"iz",    "list strings in .rodata/.data",         cmd_info_strings},
    {"ii",    "list imports (with GOT slot addresses)",  cmd_info_imports},
    {"px",    "hex dump at <addr> <len>",              cmd_print_hex},
    {"pxw",   "hex dump as 32-bit LE words",           cmd_print_hexw},
    {"pxq",   "hex dump as 64-bit LE words",           cmd_print_hexq},
    {"ps",    "print string at <addr> <len>",          cmd_print_string},
    {"s",     "seek to <addr|section|symbol|entry0>",  cmd_seek},
    {"sh",    "show seek history",                     cmd_seek_history},
    {"u",     "undo seek (pop seek history)",          cmd_seek_undo},
    {"U",     "redo seek (pop redo stack)",            cmd_seek_redo},
    {"pd",    "disassemble <n> insns at current seek", cmd_print_disasm},
    {"pdd",   "disassemble <n> insns at <addr>",       cmd_print_disasm_at},
    {"?",     "show this help",                        cmd_help},
    {"q",     "quit",                                  cmd_quit},
    {NULL, NULL, NULL}
};

/* ── Helpers ────────────────────────────────────────────────────── */

/* Address column width. rizin uses 18 chars (`0x%016llx`) on 64-bit
 * binaries and 10 chars (`0x%08llx`) on 32-bit, picked from
 * asm.bits. We mirror that. */
static int addr_width(const repl_t *r)
{
    return (r->a && r->a->bits == 64) ? 18 : 10;
}

/* ELF flag tests we need for the string-extraction pass. */
#define SHF_ALLOC_X     (uint64_t)0x2
#define SHF_EXECINSTR_X (uint64_t)0x4

/* vaddr -> file offset. Mirrors rizin's `Elf_(rz_bin_elf_v2p)`: first
 * scan sections for one that contains the vaddr, fall back to PT_LOAD
 * segments, then give up. The section path is the common case for
 * non-relocatable executables; the segment path covers stripped or
 * unusual layouts. Returns (uint64_t)-1 on miss. */
static uint64_t vaddr_to_offset(const repl_t *r, uint64_t vaddr)
{
    const debag_analysis_t *a = r->a;

    /* Section path: SHF_ALLOC sections have a meaningful vaddr. */
    for (size_t i = 0; a->sections && i < a->section_count; i++) {
        const debag_elf_section_t *s = &a->sections[i];
        if (!(s->flags & SHF_ALLOC_X)) continue;
        if (vaddr >= s->vaddr && vaddr < s->vaddr + s->size)
            return s->offset + (vaddr - s->vaddr);
    }

    /* Segment path: PT_LOAD. */
    for (size_t i = 0; a->segments && i < a->segment_count; i++) {
        const debag_elf_segment_t *seg = &a->segments[i];
        if (seg->type != PT_LOAD) continue;
        if (vaddr >= seg->vaddr && vaddr < seg->vaddr + seg->filesz)
            return seg->offset + (vaddr - seg->vaddr);
    }

    return (uint64_t)-1;
}

/* Read `n` bytes starting at virtual address `vaddr`. Translates via
 * vaddr_to_offset, then pread()s the bytes. Short reads (EOF or
 * unmapped tail) get zero-padded — rizin does the same with its
 * `io_unalloc_ch` character, except we always use 0x00. Returns the
 * number of bytes actually read (may be less than n on EOF). */
static ssize_t read_at(const repl_t *r, uint64_t vaddr, void *buf, size_t n)
{
    uint64_t off = vaddr_to_offset(r, vaddr);
    if (off == (uint64_t)-1) {
        memset(buf, 0, n);
        return 0;
    }

    ssize_t got = pread(r->fd, buf, n, (off_t)off);
    if (got < 0) {
        memset(buf, 0, n);
        return 0;
    }
    if ((size_t)got < n)
        memset((char *)buf + got, 0, n - got);
    return got;
}

/* Resolve a single token to a virtual address. Order matches rizin's
 * num_callback (core.c:890-932): entry0 first, then section names,
 * then symbol names, then hex/decimal numbers. Returns 0 on success
 * and writes the address to *out; returns -1 and prints a diagnostic
 * otherwise. */
static int resolve_expr(repl_t *r, const char *s, uint64_t *out)
{
    if (!s || !*s) return -1;

    /* entry0 -> e_entry (always defined for executables). */
    if (strcmp(s, "entry0") == 0 || strcmp(s, "entry") == 0) {
        *out = r->a->entry_point;
        return 0;
    }

    /* Section name? Linear scan. */
    for (size_t i = 0; r->a->sections && i < r->a->section_count; i++) {
        if (r->a->sections[i].name &&
            strcmp(r->a->sections[i].name, s) == 0) {
            *out = r->a->sections[i].vaddr;
            return 0;
        }
    }

    /* Symbol name? Skip imports (vaddr == 0). */
    for (size_t i = 0; r->a->symbols && i < r->a->symbol_count; i++) {
        if (r->a->symbols[i].name &&
            r->a->symbols[i].vaddr != 0 &&
            strcmp(r->a->symbols[i].name, s) == 0) {
            *out = r->a->symbols[i].vaddr;
            return 0;
        }
    }

    /* Import name? Resolve via the relocation table: the matching
     * JUMP_SLOT / GLOB_DAT reloc's r_offset is the GOT slot address.
     * This makes `s printf` seek to the GOT slot, so `px 8` shows the
     * resolved function pointer. Mirrors rizin's get_import_addr
     * (elf_imports.c:400-418). */
    for (size_t i = 0; r->a->relocs && i < r->a->reloc_count; i++) {
        const debag_elf_reloc_t *rl = &r->a->relocs[i];
        if (!rl->sym_name || strcmp(rl->sym_name, s) != 0) continue;
        int is_import_reloc = 0;
#ifdef R_X86_64_JUMP_SLOT
        if (rl->type == R_X86_64_JUMP_SLOT) is_import_reloc = 1;
#endif
#ifdef R_X86_64_GLOB_DAT
        if (rl->type == R_X86_64_GLOB_DAT) is_import_reloc = 1;
#endif
#ifdef R_X86_64_COPY
        if (rl->type == R_X86_64_COPY) is_import_reloc = 1;
#endif
#ifdef R_AARCH64_JUMP_SLOT
        if (rl->type == R_AARCH64_JUMP_SLOT) is_import_reloc = 1;
#endif
#ifdef R_AARCH64_GLOB_DAT
        if (rl->type == R_AARCH64_GLOB_DAT) is_import_reloc = 1;
#endif
        if (!is_import_reloc) continue;
        *out = rl->vaddr;
        return 0;
    }

    /* Hex / decimal number. */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char *end = NULL;
        errno = 0;
        unsigned long long v = strtoull(s + 2, &end, 16);
        if (errno == 0 && end && *end == '\0') { *out = v; return 0; }
    } else if (isdigit((unsigned char)s[0])) {
        char *end = NULL;
        errno = 0;
        unsigned long long v = strtoull(s, &end, 10);
        if (errno == 0 && end && *end == '\0') { *out = v; return 0; }
    }

    fprintf(stderr, "debag: unknown symbol '%s'\n", s);
    return -1;
}

/* Parse a single numeric argument. Accepts `0x...` hex, decimal, or a
 * symbol/section name. Returns 0 on success, -1 on miss. */
static int parse_uint(repl_t *r, const char *s, uint64_t *out)
{
    return resolve_expr(r, s, out);
}

/* Print one 16-byte row of a hex dump in rizin's `px` style. `word_size`
 * selects px (1), pxw (4), or pxq (8) layout. `row_len` is the number of
 * valid bytes in `p` (may be < 16 for the final partial row); the rest
 * is space-padded so columns line up. Extracted from print_hexdump so
 * the sparse-collapse walker can emit a row at any address. */
static void print_hexdump_row(uint64_t vaddr, const uint8_t *p, size_t row_len,
                              int word_size, int words_per_row,
                              const char *addr_fmt)
{
    printf(addr_fmt, vaddr);
    if (word_size == 1) {
        printf("  ");
        for (size_t j = 0; j < 16; j++) {
            if (j < row_len) printf("%02x", p[j]);
            else             printf("  ");
            /* extra space between byte pairs (after j=1,3,5,...,15) */
            if (j & 1) printf(" ");
        }
        printf(" ");
        for (size_t j = 0; j < 16; j++) {
            if (j < row_len) {
                uint8_t c = p[j];
                putchar(isprint(c) ? c : '.');
            } else {
                putchar(' ');
            }
        }
        printf("\n");
    } else {
        printf(" ");
        for (int w = 0; w < words_per_row; w++) {
            size_t off = (size_t)w * (size_t)word_size;
            if (off >= row_len) {
                /* pad short tail */
                if (word_size == 4) printf("  0x00000000");
                else                printf("    0x0000000000000000");
                continue;
            }
            uint64_t v = 0;
            for (int b = 0; b < word_size && off + b < row_len; b++)
                v |= (uint64_t)p[off + b] << (8 * b);
            if (word_size == 4) printf(" 0x%08" PRIx64, v);
            else                printf("  0x%016" PRIx64, v);
        }
        printf("\n");
    }
}

/* Print a hex dump in rizin's `px` style. Layout per row:
 *   0xADDR  b0 b1 b2 b3  b4 b5 b6 b7  b8 b9 ba bb  bc bd be bf  |ascii|
 * Address column is 18 chars on 64-bit binaries, 10 on 32-bit. Bytes
 * are grouped in pairs with an extra space between pairs (mirrors
 * rz_print_hexdump_str). Non-printable ASCII -> '.'.
 *
 * `word_size` selects px (1), pxw (4), or pxq (8) layout. For word
 * modes the hex column is a sequence of `0x%08x ` / `0x%016llx ` words
 * read in native (little-endian) byte order — matches rizin's
 * rz_read_ble default for x86.
 *
 * Sparse collapse: when 3+ consecutive rows are byte-identical (all
 * zeros, or any repeated pattern), print the first row, print `...`,
 * print the last row (with its real address so the user knows where
 * they are). Mirrors rizin's checkSparse loop in rz_print_hexdump_str
 * (librz/util/print.c:763-782). */
static void print_hexdump(repl_t *r, uint64_t vaddr,
                          const uint8_t *buf, size_t n, int word_size)
{
    int aw = addr_width(r);
    const char *addr_fmt = (aw == 18) ? "0x%016" PRIx64 : "0x%08"  PRIx64;
    int words_per_row = (word_size == 1) ? 0 : 16 / word_size;

    uint8_t prev[16];          /* bytes of the first row in the current run */
    size_t prev_len = 0;       /* valid bytes in prev (last row may be short) */
    uint64_t run_first_addr = 0;
    uint64_t run_last_addr = 0;
    int run = 0;               /* consecutive identical rows after the first */
    int have_prev = 0;

    for (size_t i = 0; i < n; i += 16) {
        size_t row_len = (n - i < 16) ? (n - i) : 16;
        const uint8_t *row = buf + i;

        if (have_prev && row_len == prev_len &&
            memcmp(row, prev, row_len) == 0) {
            /* Extends the current run. */
            run++;
            run_last_addr = vaddr + i;
            continue;
        }

        /* Flush the previous run. */
        if (have_prev) {
            print_hexdump_row(run_first_addr, prev, prev_len,
                              word_size, words_per_row, addr_fmt);
            if (run >= 2) {
                /* 3+ identical rows: print "..." then the last row. */
                printf("...\n");
                print_hexdump_row(run_last_addr, prev, prev_len,
                                  word_size, words_per_row, addr_fmt);
            } else if (run == 1) {
                /* 2 identical rows: print the second one too. */
                print_hexdump_row(run_last_addr, prev, prev_len,
                                  word_size, words_per_row, addr_fmt);
            }
        }

        /* Start a new run with this row. */
        memcpy(prev, row, row_len);
        prev_len = row_len;
        run_first_addr = vaddr + i;
        run_last_addr = vaddr + i;
        run = 0;
        have_prev = 1;
    }

    /* Flush the trailing run. */
    if (have_prev) {
        print_hexdump_row(run_first_addr, prev, prev_len,
                          word_size, words_per_row, addr_fmt);
        if (run >= 2) {
            printf("...\n");
            print_hexdump_row(run_last_addr, prev, prev_len,
                              word_size, words_per_row, addr_fmt);
        } else if (run == 1) {
            print_hexdump_row(run_last_addr, prev, prev_len,
                              word_size, words_per_row, addr_fmt);
        }
    }
}

/* ── Command handlers ───────────────────────────────────────────── */

static void cmd_info(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    const debag_analysis_t *a = r->a;
    printf("path:      %s\n", a->binary_path ? a->binary_path : "?");
    printf("arch:      %s\n", a->arch_name ? a->arch_name : "?");
    printf("bits:      %d\n", a->bits);
    printf("endian:    %s\n", a->is_big_endian ? "big" : "little");
    printf("type:      %s\n", a->binary_type ? a->binary_type : "?");
    printf("entry:     0x%016" PRIx64 "\n", a->entry_point);
    printf("sections:  %zu\n", a->section_count);
    printf("segments:  %zu\n", a->segment_count);
    printf("symbols:   %zu\n", a->symbol_count);
}

static void cmd_info_sections(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    const debag_analysis_t *a = r->a;
    printf("  #  %-20s %-18s %-12s %-10s %s\n",
           "name", "vaddr", "size", "type", "flags");
    for (size_t i = 0; a->sections && i < a->section_count; i++) {
        const debag_elf_section_t *s = &a->sections[i];
        char flags[16];
        debag_elf_section_flags_str(s->flags, flags, sizeof(flags));
        printf("%3zu  %-20.20s 0x%016" PRIx64 " %-12zu %-10s %s\n",
               i,
               s->name ? s->name : "",
               s->vaddr,
               s->size,
               debag_elf_section_type_name(s->type),
               flags);
    }
}

static void cmd_info_segments(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    const debag_analysis_t *a = r->a;
    printf("%-12s %-18s %-12s %-12s %-12s %s\n",
           "type", "vaddr", "offset", "filesz", "memsz", "flg");
    for (size_t i = 0; a->segments && i < a->segment_count; i++) {
        const debag_elf_segment_t *s = &a->segments[i];
        char flg[4] = {0};
        size_t k = 0;
        if (k < 3 && (s->flags & PF_R)) flg[k++] = 'R';
        if (k < 3 && (s->flags & PF_W)) flg[k++] = 'W';
        if (k < 3 && (s->flags & PF_X)) flg[k++] = 'X';
        printf("%-12s 0x%016" PRIx64 " 0x%010" PRIx64 " 0x%-10" PRIx64 " 0x%-10" PRIx64 " %s\n",
               debag_elf_segment_type_name(s->type),
               s->vaddr, s->offset, s->filesz, s->memsz, flg);
    }
}

static void cmd_info_symbols(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    const debag_analysis_t *a = r->a;
    printf("%-40s %-18s %-10s %-8s %s\n",
           "name", "vaddr", "size", "type", "bind");
    for (size_t i = 0; a->symbols && i < a->symbol_count; i++) {
        const debag_elf_symbol_t *s = &a->symbols[i];
        if (s->is_import) continue;   /* imports belong to `ii` */
        printf("%-40.40s 0x%016" PRIx64 " %-10zu %-8s %s\n",
               s->name ? s->name : "",
               s->vaddr, s->size,
               debag_elf_symbol_type_name(s->type),
               debag_elf_symbol_bind_name(s->bind));
    }
}

static void cmd_info_imports(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    const debag_analysis_t *a = r->a;

    /* Two sources of "imports":
     *  (a) Dynamic relocations whose symbol resolves to a name. These
     *      are the imports with a real GOT slot (R_X86_64_JUMP_SLOT for
     *      PLT imports, R_X86_64_GLOB_DAT for GOT imports). r_offset is
     *      the GOT slot address. This is the rizin `ii` style.
     *  (b) Undefined dynamic symbols with no corresponding reloc. Rare
     *      (typically weak undefined symbols); shown with vaddr=0.
     *
     * Name-matching caveat: .symtab often stores version-suffixed names
     * like "puts@GLIBC_2.2.5" while .dynstr (which the reloc sym_name
     * is read from) stores bare "puts". So we compare with a helper
     * that accepts either form.
     */
    int name_matches(const char *sym, const char *reloc) {
        size_t rl = strlen(reloc);
        if (strncmp(sym, reloc, rl) != 0) return 0;
        char c = sym[rl];
        return c == '\0' || c == '@';
    }

    printf("%-4s %-18s %-22s %s\n", "#", "vaddr", "type", "name");
    size_t idx = 0;
    int *covered = NULL;

    /* Build a "covered" bitmap over a->symbols so we can show imports
     * with no reloc at the bottom. Indexed by symbol array index. */
    if (a->symbols && a->symbol_count > 0) {
        covered = calloc(a->symbol_count, sizeof(int));
        if (covered) {
            for (size_t i = 0; i < a->symbol_count; i++) {
                const debag_elf_symbol_t *s = &a->symbols[i];
                if (!s->is_import || !s->name) continue;
                for (size_t k = 0; a->relocs && k < a->reloc_count; k++) {
                    if (a->relocs[k].sym_name &&
                        name_matches(s->name, a->relocs[k].sym_name)) {
                        covered[i] = 1;
                        break;
                    }
                }
            }
        }
    }

    /* (a) imports with a GOT slot, from the relocation table. */
    for (size_t k = 0; a->relocs && k < a->reloc_count; k++) {
        const debag_elf_reloc_t *rl = &a->relocs[k];
        if (!rl->sym_name || !rl->sym_name[0]) continue;
        /* Only show meaningful reloc types: JUMP_SLOT (PLT call) and
         * GLOB_DAT (GOT data). Skip R_X86_64_RELATIVE / _64 / _PC32
         * etc. which are linker-internal, not imports. */
        int is_import_reloc = 0;
#ifdef R_X86_64_JUMP_SLOT
        if (rl->type == R_X86_64_JUMP_SLOT) is_import_reloc = 1;
#endif
#ifdef R_X86_64_GLOB_DAT
        if (rl->type == R_X86_64_GLOB_DAT) is_import_reloc = 1;
#endif
#ifdef R_X86_64_COPY
        if (rl->type == R_X86_64_COPY) is_import_reloc = 1;
#endif
        if (!is_import_reloc) continue;
        printf("%-4zu 0x%016" PRIx64 "  %-22s %s\n",
               idx++, rl->vaddr,
               debag_elf_reloc_type_name(rl->type),
               rl->sym_name);
    }

    /* (b) undefined dynamic symbols with no GOT slot. */
    for (size_t i = 0; a->symbols && i < a->symbol_count; i++) {
        const debag_elf_symbol_t *s = &a->symbols[i];
        if (!s->is_import || !s->name) continue;
        if (covered && covered[i]) continue;
        printf("%-4zu 0x%016" PRIx64 "  %-22s %s\n",
               idx++, (uint64_t)0, "-",
               s->name);
    }
    free(covered);
}

static void cmd_info_entry(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    const debag_analysis_t *a = r->a;
    printf("entry0: 0x%016" PRIx64 "\n", a->entry_point);
}

/* List printable-ASCII runs of length >= 4 in SHF_ALLOC && !SHF_EXECINSTR
 * sections (.rodata, .data, .data.rel.ro, etc.). Each run is printed
 * as:  `vaddr  length  "string"` */
static void cmd_info_strings(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    const debag_analysis_t *a = r->a;

    for (size_t i = 0; a->sections && i < a->section_count; i++) {
        const debag_elf_section_t *s = &a->sections[i];
        if (!(s->flags & SHF_ALLOC_X))  continue;
        if (s->flags & SHF_EXECINSTR_X) continue;
        if (s->type == SHT_NOBITS)      continue;   /* .bss has no file bytes */
        if (s->size == 0)               continue;

        uint8_t *buf = malloc(s->size);
        if (!buf) continue;
        ssize_t got = pread(r->fd, buf, s->size, (off_t)s->offset);
        if (got <= 0) { free(buf); continue; }

        size_t run_start = 0;
        size_t run_len = 0;
        for (size_t j = 0; j < (size_t)got; j++) {
            uint8_t c = buf[j];
            if (isprint(c) && c != 0) {
                if (run_len == 0) run_start = j;
                run_len++;
            } else {
                if (run_len >= 4) {
                    printf("0x%016" PRIx64 "  %-4zu  %.*s\n",
                           s->vaddr + run_start, run_len,
                           (int)run_len, buf + run_start);
                }
                run_len = 0;
            }
        }
        if (run_len >= 4) {
            printf("0x%016" PRIx64 "  %-4zu  %.*s\n",
                   s->vaddr + run_start, run_len,
                   (int)run_len, buf + run_start);
        }
        free(buf);
    }
}

/* `px <addr> <len>` (default len = 16). Reads len bytes via read_at()
 * and prints a hex dump. */
static void do_print_hex(repl_t *r, uint64_t addr, size_t len, int word_size)
{
    if (len == 0) len = 16;
    if (len > 0x100000) {
        fprintf(stderr, "debag: length too large (max 0x100000)\n");
        return;
    }
    uint8_t *buf = malloc(len);
    if (!buf) { perror("malloc"); return; }
    read_at(r, addr, buf, len);
    print_hexdump(r, addr, buf, len, word_size);
    free(buf);
}

static void cmd_print_hex(repl_t *r, int argc, char **argv)
{
    /* px [addr] [len] -- with no args, dump 16 bytes at current seek. */
    uint64_t addr = r->offset;
    size_t len = 16;
    if (argc >= 2) {
        if (parse_uint(r, argv[1], &addr) < 0) return;
    }
    if (argc >= 3) {
        uint64_t v;
        if (parse_uint(r, argv[2], &v) < 0) return;
        len = (size_t)v;
    }
    do_print_hex(r, addr, len, 1);
}

static void cmd_print_hexw(repl_t *r, int argc, char **argv)
{
    uint64_t addr = r->offset;
    size_t len = 16;
    if (argc >= 2) {
        if (parse_uint(r, argv[1], &addr) < 0) return;
    }
    if (argc >= 3) {
        uint64_t v;
        if (parse_uint(r, argv[2], &v) < 0) return;
        len = (size_t)v;
    }
    do_print_hex(r, addr, len, 4);
}

static void cmd_print_hexq(repl_t *r, int argc, char **argv)
{
    uint64_t addr = r->offset;
    size_t len = 16;
    if (argc >= 2) {
        if (parse_uint(r, argv[1], &addr) < 0) return;
    }
    if (argc >= 3) {
        uint64_t v;
        if (parse_uint(r, argv[2], &v) < 0) return;
        len = (size_t)v;
    }
    do_print_hex(r, addr, len, 8);
}

static void cmd_print_string(repl_t *r, int argc, char **argv)
{
    uint64_t addr = r->offset;
    size_t len = 256;
    if (argc >= 2) {
        if (parse_uint(r, argv[1], &addr) < 0) return;
    }
    if (argc >= 3) {
        uint64_t v;
        if (parse_uint(r, argv[2], &v) < 0) return;
        len = (size_t)v;
    }
    if (len > 0x100000) {
        fprintf(stderr, "debag: length too large (max 0x100000)\n");
        return;
    }
    uint8_t *buf = malloc(len);
    if (!buf) { perror("malloc"); return; }
    ssize_t got = read_at(r, addr, buf, len);
    /* Print until NUL, EOF, or len. */
    size_t i = 0;
    while (i < (size_t)got && i < len && buf[i] != 0) {
        if (isprint(buf[i]) || buf[i] == '\t' || buf[i] == '\n')
            putchar(buf[i]);
        else
            putchar('.');
        i++;
    }
    putchar('\n');
    free(buf);
}

/* Push `v` onto the seek history stack. If the stack is full, the
 * oldest entry (index 0) is dropped via a one-element memmove shift.
 * Order is oldest-first, newest-last. */
static void seek_hist_push(repl_t *r, uint64_t v)
{
    if (r->seek_hist_count == SEEK_HIST_MAX) {
        memmove(r->seek_hist, r->seek_hist + 1,
                (SEEK_HIST_MAX - 1) * sizeof(r->seek_hist[0]));
        r->seek_hist[SEEK_HIST_MAX - 1] = v;
    } else {
        r->seek_hist[r->seek_hist_count++] = v;
    }
}

/* Push `v` onto the redo stack (same bounded-stack semantics). */
static void redo_stack_push(repl_t *r, uint64_t v)
{
    if (r->redo_count == SEEK_HIST_MAX) {
        memmove(r->redo_stack, r->redo_stack + 1,
                (SEEK_HIST_MAX - 1) * sizeof(r->redo_stack[0]));
        r->redo_stack[SEEK_HIST_MAX - 1] = v;
    } else {
        r->redo_stack[r->redo_count++] = v;
    }
}

static void cmd_seek(repl_t *r, int argc, char **argv)
{
    if (argc < 2) {
        /* bare `s`: print current seek (rizin prints in the same style). */
        printf("0x%016" PRIx64 "\n", r->offset);
        return;
    }
    uint64_t v;
    if (resolve_expr(r, argv[1], &v) < 0) return;
    if (v == r->offset) return;   /* no-op seek: don't touch history */
    seek_hist_push(r, r->offset);
    r->redo_count = 0;            /* any new seek clears redo, rizin-style */
    r->offset = v;
}

/* `u`: undo seek. Pop the most recent entry from seek_hist, push the
 * current position to redo_stack, restore the popped entry. Prints
 * "no seek history" if the history is empty. */
static void cmd_seek_undo(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (r->seek_hist_count == 0) {
        fprintf(stderr, "no seek history\n");
        return;
    }
    uint64_t prev = r->seek_hist[--r->seek_hist_count];
    redo_stack_push(r, r->offset);
    r->offset = prev;
}

/* `U`: redo seek. Pop the most recent entry from redo_stack, push
 * the current position back to seek_hist, restore the popped entry.
 * Prints "no seek redo history" if the redo stack is empty. */
static void cmd_seek_redo(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (r->redo_count == 0) {
        fprintf(stderr, "no seek redo history\n");
        return;
    }
    uint64_t prev = r->redo_stack[--r->redo_count];
    seek_hist_push(r, r->offset);
    r->offset = prev;
}

/* `sh`: print the seek history stack (oldest to newest), then the
 * current position. The current position is marked with `<=` so it's
 * visually distinct from the historical entries. */
static void cmd_seek_history(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (r->seek_hist_count == 0 && r->redo_count == 0) {
        printf("(no seek history)\n");
        return;
    }
    for (size_t i = 0; i < r->seek_hist_count; i++)
        printf("  0x%016" PRIx64 "\n", r->seek_hist[i]);
    printf("  0x%016" PRIx64 "  <= current\n", r->offset);
    for (size_t i = 0; i < r->redo_count; i++)
        printf("  0x%016" PRIx64 "  (redo)\n",
               r->redo_stack[r->redo_count - 1 - i]);
}

#ifdef HAVE_CAPSTONE
/* Pick Capstone arch/mode from the analysis. Returns 0 on success. */
static int pick_capstone(const debag_analysis_t *a, cs_arch *arch, cs_mode *mode)
{
    *mode = CS_MODE_LITTLE_ENDIAN;
    if (a->is_big_endian) *mode = CS_MODE_BIG_ENDIAN;

    if (!a->arch_name) return -1;

    if (strcmp(a->arch_name, "x86-64") == 0) {
        *arch = CS_ARCH_X86;
        *mode |= CS_MODE_64;
        return 0;
    }
    if (strcmp(a->arch_name, "x86") == 0) {
        *arch = CS_ARCH_X86;
        *mode |= CS_MODE_32;
        return 0;
    }
    if (strcmp(a->arch_name, "aarch64") == 0) {
        *arch = CS_ARCH_ARM64;
        return 0;
    }
    if (strcmp(a->arch_name, "arm") == 0) {
        *arch = CS_ARCH_ARM;
        *mode |= CS_MODE_32;
        return 0;
    }
#ifdef CS_ARCH_RISCV
    if (strcmp(a->arch_name, "riscv") == 0) {
        *arch = CS_ARCH_RISCV;
        *mode |= (a->bits == 64) ? CS_MODE_RISCV64 : CS_MODE_RISCV32;
        return 0;
    }
#endif
    if (strcmp(a->arch_name, "ppc64") == 0) {
        *arch = CS_ARCH_PPC;
        *mode |= CS_MODE_64;
        return 0;
    }
    if (strcmp(a->arch_name, "s390x") == 0) {
        *arch = CS_ARCH_SYSZ;
        return 0;
    }
    return -1;
}

/* Disassemble up to `count` instructions starting at vaddr `addr`.
 * Format mirrors rizin's compact `pdi` output:
 *   0xADDR  <hex bytes>  mnemonic operands
 * Bytes are concatenated (no separator), right-justified to a 16-wide
 * column so the asm column lines up.
 *
 * PLT annotation: when a `call`/`jmp`/`jcc` instruction targets an
 * address inside `.plt` / `.plt.got` / `.plt.sec`, the corresponding
 * GOT slot is resolved by reading the PLT entry's first instruction
 * (`ff 25 <rel32>` = `jmp [rip+disp32]`), computing GOT_slot = target
 * + 6 + disp32, and looking that slot up in the reloc table. The
 * resolved symbol name is appended as `  ; -> 0xGOT (name)`. */
static void do_disasm(repl_t *r, uint64_t addr, size_t count)
{
    if (count == 0) count = 16;
    if (count > 4096) count = 4096;

    cs_arch arch;
    cs_mode mode;
    if (pick_capstone(r->a, &arch, &mode) < 0) {
        fprintf(stderr,
                "debag: unsupported arch '%s' for disassembly\n",
                r->a->arch_name ? r->a->arch_name : "?");
        return;
    }

    csh handle;
    if (cs_open(arch, mode, &handle) != CS_ERR_OK) {
        fprintf(stderr, "debag: cs_open failed\n");
        return;
    }

    /* Cache the .plt* section ranges so we can recognise call targets
     * that land in the PLT. NULL range_start means "not present". */
    uint64_t plt_lo = 0, plt_hi = 0;
    int have_plt = 0;
    for (size_t i = 0; r->a->sections && i < r->a->section_count; i++) {
        const debag_elf_section_t *s = &r->a->sections[i];
        if (!s->name) continue;
        if (strcmp(s->name, ".plt") == 0 ||
            strcmp(s->name, ".plt.got") == 0 ||
            strcmp(s->name, ".plt.sec") == 0) {
            if (!have_plt) {
                plt_lo = s->vaddr;
                plt_hi = s->vaddr + s->size;
                have_plt = 1;
            } else {
                /* Expand the range to cover all .plt* sections. */
                if (s->vaddr < plt_lo) plt_lo = s->vaddr;
                if (s->vaddr + s->size > plt_hi)
                    plt_hi = s->vaddr + s->size;
            }
        }
    }

    /* Capstone may need a slightly larger buffer than count * 15 (max
     * x86 insn length). Read generously and stop when we've decoded
     * `count` instructions. */
    size_t bufsz = count * 16 + 64;
    uint8_t *buf = malloc(bufsz);
    if (!buf) { cs_close(&handle); return; }
    read_at(r, addr, buf, bufsz);

    cs_insn *insns = NULL;
    size_t n = cs_disasm(handle, buf, bufsz, addr, count, &insns);
    if (n == 0) {
        fprintf(stderr, "debag: no instructions decoded at 0x%" PRIx64 "\n",
                addr);
    } else {
        for (size_t i = 0; i < n; i++) {
            printf("0x%016" PRIx64 "  ", insns[i].address);
            char hex[64] = "";
            size_t hl = 0;
            for (size_t j = 0; j < insns[i].size && hl < sizeof(hex) - 2; j++)
                hl += (size_t)snprintf(hex + hl, sizeof(hex) - hl,
                                       "%02x", insns[i].bytes[j]);
            printf("%-16s %s %s", hex, insns[i].mnemonic, insns[i].op_str);

            /* PLT call annotation. Only `call`/`jmp`/`jcc` with a
             * `0x...` immediate operand can target the PLT. */
            if (have_plt && insns[i].op_str[0] == '0' &&
                insns[i].op_str[1] == 'x') {
                char *end = NULL;
                errno = 0;
                unsigned long long tgt =
                    strtoull(insns[i].op_str + 2, &end, 16);
                if (errno == 0 && end && *end == '\0' &&
                    tgt >= plt_lo && tgt < plt_hi) {
                    /* Read the first 6 bytes of the PLT entry: expect
                     * `ff 25 <rel32>` (jmp [rip+disp32]). The GOT
                     * slot is tgt + 6 + (int32_t)disp. */
                    uint8_t plt_entry[6];
                    read_at(r, (uint64_t)tgt, plt_entry, sizeof(plt_entry));
                    if (plt_entry[0] == 0xff && plt_entry[1] == 0x25) {
                        int32_t disp = (int32_t)(
                            (uint32_t)plt_entry[2] |
                            ((uint32_t)plt_entry[3] << 8) |
                            ((uint32_t)plt_entry[4] << 16) |
                            ((uint32_t)plt_entry[5] << 24));
                        uint64_t got = (uint64_t)tgt + 6 + (int64_t)disp;
                        /* Look up the GOT slot in the reloc table. */
                        for (size_t k = 0;
                             r->a->relocs && k < r->a->reloc_count;
                             k++) {
                            if (r->a->relocs[k].vaddr == got &&
                                r->a->relocs[k].sym_name) {
                                printf("  ; -> 0x%016" PRIx64 " (%s)",
                                       got, r->a->relocs[k].sym_name);
                                break;
                            }
                        }
                    }
                }
            }
            printf("\n");
        }
        cs_free(insns, n);
    }
    free(buf);
    cs_close(&handle);
}
#else
static void do_disasm(repl_t *r, uint64_t addr, size_t count)
{
    (void)r; (void)addr; (void)count;
    fprintf(stderr,
            "debag: disassembly requires capstone "
            "(install libcapstone-dev)\n");
}
#endif

static void cmd_print_disasm(repl_t *r, int argc, char **argv)
{
    size_t count = 16;
    if (argc >= 2) {
        uint64_t v;
        if (parse_uint(r, argv[1], &v) < 0) return;
        count = (size_t)v;
    }
    do_disasm(r, r->offset, count);
}

static void cmd_print_disasm_at(repl_t *r, int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: pdd <addr> <n>\n");
        return;
    }
    uint64_t addr;
    if (parse_uint(r, argv[1], &addr) < 0) return;
    uint64_t v;
    if (parse_uint(r, argv[2], &v) < 0) return;
    do_disasm(r, addr, (size_t)v);
}

static void cmd_help(repl_t *r, int argc, char **argv)
{
    (void)r; (void)argc; (void)argv;
    printf("Commands (longest-prefix match):\n");
    for (size_t i = 0; cmds[i].name; i++)
        printf("  %-4s  %s\n", cmds[i].name, cmds[i].summary);
    printf("\nNumbers: decimal or 0x-hex. Symbols resolve as entry0,\n");
    printf("section name, symbol name, or import name (-> GOT slot).\n");
}

static void cmd_quit(repl_t *r, int argc, char **argv)
{
    (void)argc; (void)argv;
    r->quit = 1;
}

/* ── Tokenizer + dispatcher ───────────────────────────────────────
 *
 * We don't need rizin's tree-sitter rzshell parser. A simple
 * whitespace tokenizer is enough: the first token is the command name
 * (matched by longest-prefix), the rest are argv[] (each token parsed
 * by the handler). */
static int dispatch_line(repl_t *r, char *line)
{
    /* Strip trailing newline / whitespace. */
    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) *--end = '\0';

    /* Skip leading whitespace. */
    char *s = line;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return 0;   /* empty line, re-prompt */

    /* Tokenize on whitespace. */
    char *argv[16];
    int argc = 0;
    char *tok = strtok(s, " \t");
    while (tok && argc < 15) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[argc] = NULL;

    /* Aliases: quit / exit -> q. */
    if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "exit") == 0) {
        r->quit = 1;
        return 0;
    }
    if (strcmp(argv[0], "help") == 0) {
        cmd_help(r, argc, argv);
        return 0;
    }

    /* Longest-prefix match: scan the table, prefer the longest name
     * that argv[0] starts with. This mirrors rizin's
     * cmd_get_desc_best() (cmd_api.c:342) — except we don't bother
     * with char-chopping; we just walk the table once and track the
     * best hit. */
    size_t best_len = 0;
    int best_idx = -1;
    size_t arg_len = strlen(argv[0]);
    for (int i = 0; cmds[i].name; i++) {
        size_t nl = strlen(cmds[i].name);
        if (nl > arg_len) continue;
        if (nl <= best_len) continue;
        if (strncmp(cmds[i].name, argv[0], nl) == 0) {
            best_len = nl;
            best_idx = i;
        }
    }

    if (best_idx < 0) {
        fprintf(stderr, "debag: unknown command '%s' (try ?)\n", argv[0]);
        return 0;
    }

    cmds[best_idx].fn(r, argc, argv);
    return 0;
}

/* ── Public entry point ─────────────────────────────────────────── */

int debag_static_db_repl(const char *binary_path)
{
    if (!binary_path) return 1;

    debag_analysis_t *a = debag_analyze(binary_path);
    if (!a) {
        fprintf(stderr, "debag: cannot analyze '%s'\n", binary_path);
        return 1;
    }

    int fd = open(binary_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "debag: cannot open '%s': %s\n",
                binary_path, strerror(errno));
        debag_analysis_free(a);
        return 1;
    }

    repl_t r = { .a = a, .fd = fd, .offset = a->entry_point, .quit = 0 };

    printf("debag static-db: %s (%s, %d-bit)\n",
           binary_path,
           a->arch_name ? a->arch_name : "?",
           a->bits);
    printf("Type 'q' to quit, '?' for help.\n");

    char line[4096];
    while (!r.quit) {
        printf("0x%016" PRIx64 "> ", r.offset);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        dispatch_line(&r, line);
    }

    close(fd);
    debag_analysis_free(a);
    return 0;
}
