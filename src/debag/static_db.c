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
static void cmd_info_strings_all(repl_t *r, int argc, char **argv);
static void cmd_print_hex(repl_t *r, int argc, char **argv);
static void cmd_print_hexw(repl_t *r, int argc, char **argv);
static void cmd_print_hexq(repl_t *r, int argc, char **argv);
static void cmd_print_hexr(repl_t *r, int argc, char **argv);
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
    {"izz",   "list strings in ALL sections",          cmd_info_strings_all},
    {"ii",    "list imports (with GOT slot addresses)",  cmd_info_imports},
    {"px",    "hex dump at <addr> <len>",              cmd_print_hex},
    {"pxw",   "hex dump as 32-bit LE words",           cmd_print_hexw},
    {"pxq",   "hex dump as 64-bit LE words",           cmd_print_hexq},
    {"pxr",   "pointer-chase dump: 8-byte words with -> symbol",  cmd_print_hexr},
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
 * num_callback (core.c:890-932): $-tokens first, then entry0, then
 * section names, then symbol names, then hex/decimal numbers. Returns
 * 0 on success and writes the address to *out; returns -1 and prints
 * a diagnostic otherwise.
 *
 * $-tokens (rizin's secret sauce, core.c:593-692):
 *   $$ = current seek (r->offset)
 *   $s = binary size (file bytes, via fstat)
 *   $e = entry point address (same as entry0)
 * These resolve BEFORE symbol/number lookup so a binary that happens
 * to define a symbol named "s" or "e" doesn't shadow the tokens. */
static int resolve_expr(repl_t *r, const char *s, uint64_t *out)
{
    if (!s || !*s) return -1;

    /* $-tokens. */
    if (s[0] == '$') {
        if (s[1] == '$' && s[2] == '\0') {
            *out = r->offset;
            return 0;
        }
        if (s[1] == 's' && s[2] == '\0') {
            struct stat st;
            if (fstat(r->fd, &st) == 0) {
                *out = (uint64_t)st.st_size;
                return 0;
            }
            fprintf(stderr, "debag: $s fstat failed\n");
            return -1;
        }
        if (s[1] == 'e' && s[2] == '\0') {
            *out = r->a->entry_point;
            return 0;
        }
        fprintf(stderr, "debag: unknown $-token '%s' "
                "(known: $$, $s, $e)\n", s);
        return -1;
    }

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

/* ── String extraction (iz / izz) ──────────────────────────────────
 *
 * Clean-room reimplementation of rizin's string-extraction pass
 * (librz/util/str_search.c:303-342 process_one_string + the
 * check_ascii_freq / RzStrEnc dispatch). We support two encodings:
 *
 *   ascii  — printable (0x20..0x7e) run, length >= 4 bytes, terminated
 *            by a non-printable byte (or end of section).
 *   utf16  — UTF-16LE run: pairs of (printable_lo, 0x00), length >= 4
 *            chars (8 bytes), terminated by a pair whose high byte
 *            isn't 0 or whose low byte isn't printable.
 *
 * Output format:
 *   iz:   0xADDR  ascii  "string"     (or `utf16` for UTF-16LE)
 *   izz:  0xADDR  <section>  ascii  "string"
 *         (section name column added so the user can see which
 *         section the string came from when scanning the whole binary)
 *
 * iz subcommands:
 *   iz           — both ASCII and UTF-16LE (default)
 *   iz ascii     — ASCII only
 *   iz utf16     — UTF-16LE only (also accepts `utf16le`)
 *
 * izz subcommands: same three forms, same semantics.
 */

/* Escape-print one character of a string body. `c` is the byte to emit;
 * it's already known to be printable (the scanners only collect printable
 * runs), but we still escape `"` and `\` so the output is unambiguous. */
static void print_string_char(uint8_t c)
{
    if (c == '"')      printf("\\\"");
    else if (c == '\\') printf("\\\\");
    else                putchar(c);
}

/* Print an ASCII string at vaddr. `p` points at the first byte, `len`
 * is the run length in bytes. If `section_name` is non-NULL it's emitted
 * between the address and the type column (izz style); if NULL the
 * column is omitted (iz style). */
static void print_string_ascii(uint64_t vaddr, const uint8_t *p, size_t len,
                                const char *section_name)
{
    if (section_name)
        printf("0x%016" PRIx64 "  %-12s ascii  \"",
               vaddr, section_name);
    else
        printf("0x%016" PRIx64 "  ascii  \"", vaddr);
    for (size_t i = 0; i < len; i++) print_string_char(p[i]);
    printf("\"\n");
}

/* Print a UTF-16LE string at vaddr. `p` points at the first byte (the
 * low byte of the first char); `char_count` is the number of 2-byte
 * chars. The scanners already verified high-byte == 0 for every char,
 * so we just emit the low bytes. */
static void print_string_utf16(uint64_t vaddr, const uint8_t *p, size_t char_count,
                                const char *section_name)
{
    if (section_name)
        printf("0x%016" PRIx64 "  %-12s utf16  \"",
               vaddr, section_name);
    else
        printf("0x%016" PRIx64 "  utf16  \"", vaddr);
    for (size_t i = 0; i < char_count; i++) print_string_char(p[i * 2]);
    printf("\"\n");
}

/* Scan a byte buffer for ASCII and UTF-16LE strings, calling the
 * per-string printer for each one found. `buf` is the section bytes
 * (or whole-file bytes for izz); `bufsz` is the size; `vaddr_base` is
 * the virtual address of buf[0]; `section_name` is forwarded to the
 * printer (NULL = omit the section column). `want_ascii` and
 * `want_utf16` select which encodings to emit. */
static void scan_strings_in_buf(const uint8_t *buf, size_t bufsz,
                                uint64_t vaddr_base,
                                const char *section_name,
                                int want_ascii, int want_utf16)
{
    if (want_ascii) {
        size_t run_start = 0, run_len = 0;
        for (size_t j = 0; j < bufsz; j++) {
            uint8_t c = buf[j];
            if (isprint(c) && c != 0) {
                if (run_len == 0) run_start = j;
                run_len++;
            } else {
                if (run_len >= 4)
                    print_string_ascii(vaddr_base + run_start,
                                       buf + run_start, run_len,
                                       section_name);
                run_len = 0;
            }
        }
        if (run_len >= 4)
            print_string_ascii(vaddr_base + run_start,
                               buf + run_start, run_len, section_name);
    }

    if (want_utf16) {
        /* Walk 2 bytes at a time. A run is a sequence of (printable, 0x00)
         * pairs. Minimum 4 chars = 8 bytes. The +1 in `j + 1 < bufsz`
         * guards the high-byte read. */
        size_t run_start = 0, run_chars = 0;
        for (size_t j = 0; j + 1 < bufsz; j += 2) {
            uint8_t lo = buf[j];
            uint8_t hi = buf[j + 1];
            if (isprint(lo) && lo != 0 && hi == 0) {
                if (run_chars == 0) run_start = j;
                run_chars++;
            } else {
                if (run_chars >= 4)
                    print_string_utf16(vaddr_base + run_start,
                                       buf + run_start, run_chars,
                                       section_name);
                run_chars = 0;
            }
        }
        if (run_chars >= 4)
            print_string_utf16(vaddr_base + run_start,
                               buf + run_start, run_chars, section_name);
    }
}

/* Parse the iz/izz subcommand argument. Returns:
 *   0 on success (with *want_ascii and *want_utf16 filled in),
 *  -1 on unrecognized subcommand (after printing a diagnostic). */
static int parse_string_filter(int argc, char **argv,
                                int *want_ascii, int *want_utf16)
{
    *want_ascii = 1;
    *want_utf16 = 1;
    if (argc < 2) return 0;
    if (strcmp(argv[1], "ascii") == 0) {
        *want_utf16 = 0;
        return 0;
    }
    if (strcmp(argv[1], "utf16") == 0 || strcmp(argv[1], "utf16le") == 0) {
        *want_ascii = 0;
        return 0;
    }
    fprintf(stderr, "debag: unknown string subcommand '%s' "
            "(expected 'ascii' or 'utf16')\n", argv[1]);
    return -1;
}

/* `iz [ascii|utf16]` — list printable strings in SHF_ALLOC && !SHF_EXECINSTR
 * sections (.rodata, .data, .data.rel.ro, etc.). Detects both 8-bit
 * ASCII runs and UTF-16LE runs, minimum 4 chars. Output format:
 *   0xADDR  ascii  "string"
 *   0xADDR  utf16  "string"
 * With no subcommand, prints both. `iz ascii` / `iz utf16` filter. */
static void cmd_info_strings(repl_t *r, int argc, char **argv)
{
    int want_ascii, want_utf16;
    if (parse_string_filter(argc, argv, &want_ascii, &want_utf16) < 0) return;

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
        scan_strings_in_buf(buf, (size_t)got, s->vaddr,
                            /*section_name=*/NULL,
                            want_ascii, want_utf16);
        free(buf);
    }
}

/* `izz [ascii|utf16]` — like `iz` but scans ALL sections (drops the
 * SHF_ALLOC filter), including `.text`, `.comment`, `.note.*`, etc.
 * Useful for finding strings in unusual places (e.g. error messages
 * embedded in `.text`, compiler version stamps in `.comment`). The
 * section name is printed before the type column so the user can see
 * where each string came from:
 *   0xADDR  .rodata   ascii  "Hello"
 *   0xADDR  .text     ascii  "Error: %s"
 *   0xADDR  .comment  ascii  "GCC: 12.2.0"
 */
static void cmd_info_strings_all(repl_t *r, int argc, char **argv)
{
    int want_ascii, want_utf16;
    if (parse_string_filter(argc, argv, &want_ascii, &want_utf16) < 0) return;

    const debag_analysis_t *a = r->a;
    for (size_t i = 0; a->sections && i < a->section_count; i++) {
        const debag_elf_section_t *s = &a->sections[i];
        if (s->type == SHT_NOBITS)      continue;
        if (s->size == 0)               continue;

        uint8_t *buf = malloc(s->size);
        if (!buf) continue;
        ssize_t got = pread(r->fd, buf, s->size, (off_t)s->offset);
        if (got <= 0) { free(buf); continue; }
        /* For SHF_ALLOC sections the address column is the virtual
         * address (matches iz). For non-ALLOC sections (.comment,
         * .strtab, .note.*, etc.) the vaddr is meaningless (typically
         * 0), so we use the file offset instead — same convention as
         * rizin's izz. The user can tell the two apart by the section
         * name in the middle column. */
        uint64_t addr_base = (s->flags & SHF_ALLOC_X) ? s->vaddr : s->offset;
        scan_strings_in_buf(buf, (size_t)got, addr_base,
                            s->name ? s->name : "?",
                            want_ascii, want_utf16);
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

/* Find the closest defined symbol whose vaddr is <= value and whose
 * range contains value. For symbols with a known size, containment is
 * [vaddr, vaddr+size); for size==0 symbols, only an exact vaddr match
 * counts (we can't confirm an in-range offset without a size). Returns
 * the symbol name (borrowed from a->symbols) and writes the offset
 * (value - vaddr) to *off_out. Returns NULL if no symbol contains
 * value. Used by `pxr` to annotate 8-byte pointer values with the
 * function/object they point to. Mirrors rizin's pxr resolve logic
 * (librz/util/print.c:749, 863-883). */
static const char *sym_containing(const debag_analysis_t *a,
                                  uint64_t value, uint64_t *off_out)
{
    const debag_elf_symbol_t *best = NULL;
    for (size_t i = 0; a->symbols && i < a->symbol_count; i++) {
        const debag_elf_symbol_t *s = &a->symbols[i];
        if (s->is_import) continue;
        if (s->vaddr == 0 || !s->name) continue;
        if (s->vaddr > value) continue;

        if (s->size > 0) {
            /* Known size: require containment. */
            if (value < s->vaddr + s->size) {
                /* Containment hit. Prefer this over a size==0 fallback. */
                best = s;
                break;
            }
            /* Otherwise keep scanning; another symbol may contain value. */
            continue;
        }

        /* Unknown size: only an exact vaddr match counts. */
        if (s->vaddr == value) {
            best = s;
            break;
        }
    }
    if (!best) return NULL;
    if (off_out) *off_out = value - best->vaddr;
    return best->name;
}

/* `pxr <addr> <len>` -- pointer-chase hex dump. Like `pxq` (one 64-bit
 * word per line) with two extras:
 *   (1) all-zero words are suppressed entirely (different from the
 *       sparse collapse in `px`/`pxw`/`pxq`, which collapses repeats;
 *       `pxr` just hides zeros);
 *   (2) each non-zero word is annotated `-> symname` if the word value
 *       falls inside a defined symbol's [vaddr, vaddr+size) range, or
 *       `-> symname+0xOFF` for an in-range non-exact match. Words that
 *       don't fall in any symbol's range print `-> (no symbol)`.
 * Default len = 128 (16 qwords). Mirrors rizin's pxr
 * (librz/util/print.c:749, 863-883). */
static void cmd_print_hexr(repl_t *r, int argc, char **argv)
{
    uint64_t addr = r->offset;
    size_t len = 128;  /* 16 qwords */
    if (argc >= 2) {
        if (parse_uint(r, argv[1], &addr) < 0) return;
    }
    if (argc >= 3) {
        uint64_t v;
        if (parse_uint(r, argv[2], &v) < 0) return;
        len = (size_t)v;
    }
    if (len == 0) len = 128;
    if (len > 0x100000) {
        fprintf(stderr, "debag: length too large (max 0x100000)\n");
        return;
    }

    uint8_t *buf = malloc(len);
    if (!buf) { perror("malloc"); return; }
    read_at(r, addr, buf, len);

    int aw = addr_width(r);
    const char *addr_fmt = (aw == 18) ? "0x%016" PRIx64 : "0x%08"  PRIx64;

    for (size_t i = 0; i + 8 <= len; i += 8) {
        uint64_t word = 0;
        for (int b = 0; b < 8; b++)
            word |= (uint64_t)buf[i + b] << (8 * b);
        if (word == 0) continue;  /* suppress all-zero words */

        uint64_t off = 0;
        const char *name = sym_containing(r->a, word, &off);
        printf(addr_fmt, addr + i);
        printf("  0x%016" PRIx64, word);
        if (name && off == 0)
            printf(" -> %s\n", name);
        else if (name)
            printf(" -> %s+0x%" PRIx64 "\n", name, off);
        else
            printf(" -> (no symbol)\n");
    }

    free(buf);
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

/* If `tgt` is a PLT entry (in the .plt / .plt.got / .plt.sec range)
 * and its first instruction is `ff 25 <disp32>` (jmp [rip+disp32]),
 * compute the GOT slot = tgt + 6 + (int32_t)disp and look it up in
 * the reloc table. On a hit, returns the import name (borrowed from
 * a->relocs[].sym_name) and writes the GOT slot address to *got_out.
 * Returns NULL if `tgt` is not a PLT entry or the GOT slot has no
 * matching reloc. Handles the CET .plt.sec layout where `ff 25` is
 * at offset 4 (after endbr64) as well as the standard offset 0. */
static const char *resolve_plt_call(const debag_analysis_t *a,
                                    const repl_t *r,
                                    uint64_t tgt,
                                    uint64_t plt_lo, uint64_t plt_hi,
                                    int have_plt,
                                    uint64_t *got_out)
{
    if (!have_plt || tgt < plt_lo || tgt >= plt_hi) return NULL;
    uint8_t plt_entry[8];
    read_at(r, tgt, plt_entry, sizeof(plt_entry));
    /* Standard PLT: `ff 25 <disp32>` at offset 0.
     * CET .plt.sec: `f3 0f 1e fb` (endbr64) then `ff 25 <disp32>` at
     * offset 4. Accept either layout. */
    int disp_off = -1;
    if (plt_entry[0] == 0xff && plt_entry[1] == 0x25) disp_off = 0;
    else if (plt_entry[0] == 0xf3 && plt_entry[1] == 0x0f &&
             plt_entry[2] == 0x1e && plt_entry[3] == 0xfb &&
             plt_entry[4] == 0xff && plt_entry[5] == 0x25) disp_off = 4;
    if (disp_off < 0) return NULL;

    int32_t disp = (int32_t)(
        (uint32_t)plt_entry[disp_off + 2] |
        ((uint32_t)plt_entry[disp_off + 3] << 8) |
        ((uint32_t)plt_entry[disp_off + 4] << 16) |
        ((uint32_t)plt_entry[disp_off + 5] << 24));
    /* The disp is relative to the end of the `ff 25` instruction,
     * which is at tgt + disp_off + 6. */
    uint64_t got = tgt + (uint64_t)disp_off + 6 + (int64_t)disp;
    for (size_t k = 0; a->relocs && k < a->reloc_count; k++) {
        if (a->relocs[k].vaddr == got && a->relocs[k].sym_name) {
            if (got_out) *got_out = got;
            return a->relocs[k].sym_name;
        }
    }
    return NULL;
}

/* Disassemble up to `count` instructions starting at vaddr `addr`.
 * Format mirrors rizin's compact `pdi` output:
 *   0xADDR  <hex bytes>  mnemonic operands  ; <annotation>
 * Bytes are concatenated (no separator), right-justified to a 16-wide
 * column so the asm column lines up. Address column is fixed-width
 * (`0x` + 16 hex chars + 2 spaces) so the per-line annotations line
 * up vertically.
 *
 * Annotations (clean-room reimplementation of rizin's reflines
 * logic at librz/arch/reflines.c:88-285; no rizin code copied,
 * LGPL-3 vs GPL-2):
 *
 *   - Jumps (jmp, je, jne, jz, ...): if the operand is an immediate
 *     `0x<target>`, append `  ; -> <symname>` (or `  ; -> <symname>+0xN`
 *     if the target falls inside a symbol's range but not at its
 *     start) when the target resolves to a known symbol, otherwise
 *     `  ; -> 0x<target>`. If the target is outside the disassembly
 *     window, also append ` (out)`. The simpler text-arrow form is
 *     used in place of rizin's multi-line ASCII art.
 *
 *   - Calls: if the target is a PLT entry (in .plt / .plt.got /
 *     .plt.sec), resolve the GOT slot via the entry's `jmp [rip+disp]`
 *     and look the slot up in the reloc table; append `  ; <name>@plt`.
 *     Otherwise, if the target resolves to a known symbol, append
 *     `  ; <symname>` (or `  ; <symname>+0xN`). Direct calls to
 *     unknown targets are not annotated. */
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
        /* Collect the in-window instruction addresses so we can tell
         * whether a jump target lands inside the current `pd` window. */
        uint64_t *window_addrs = malloc(n * sizeof(uint64_t));
        if (window_addrs) {
            for (size_t i = 0; i < n; i++)
                window_addrs[i] = insns[i].address;
        }

        for (size_t i = 0; i < n; i++) {
            printf("0x%016" PRIx64 "  ", insns[i].address);
            char hex[64] = "";
            size_t hl = 0;
            for (size_t j = 0; j < insns[i].size && hl < sizeof(hex) - 2; j++)
                hl += (size_t)snprintf(hex + hl, sizeof(hex) - hl,
                                       "%02x", insns[i].bytes[j]);
            printf("%-16s %s %s", hex, insns[i].mnemonic, insns[i].op_str);

            /* Classify by mnemonic. Capstone emits lowercase mnemonics
             * for x86. `j` covers jmp/je/jne/jz/jg/jl/... and the
             * jcxz/jecxz/jrcxz family. `call` is matched exactly so
             * we don't accidentally pick up e.g. "callf" (far call). */
            const char *m = insns[i].mnemonic;
            int is_jump = (m[0] == 'j' && m[1] != '\0');
            int is_call = (m[0] == 'c' && m[1] == 'a' && m[2] == 'l' &&
                           m[3] == 'l' && m[4] == '\0');

            /* Extract the immediate target if op_str is `0x...`. */
            uint64_t tgt = 0;
            int have_tgt = 0;
            if (insns[i].op_str[0] == '0' && insns[i].op_str[1] == 'x') {
                char *end = NULL;
                errno = 0;
                unsigned long long v =
                    strtoull(insns[i].op_str + 2, &end, 16);
                if (errno == 0 && end && *end == '\0') {
                    tgt = (uint64_t)v;
                    have_tgt = 1;
                }
            }

            if (is_jump && have_tgt) {
                /* Is the target inside the disassembly window? */
                int in_window = 0;
                if (window_addrs) {
                    for (size_t k = 0; k < n; k++) {
                        if (window_addrs[k] == tgt) {
                            in_window = 1;
                            break;
                        }
                    }
                }
                uint64_t off = 0;
                const char *name = sym_containing(r->a, tgt, &off);
                if (name && off == 0)
                    printf("  ; -> %s", name);
                else if (name)
                    printf("  ; -> %s+0x%" PRIx64, name, off);
                else
                    printf("  ; -> 0x%" PRIx64, tgt);
                if (!in_window)
                    printf(" (out)");
            } else if (is_call && have_tgt) {
                /* Try PLT first: PLT calls have a known import name
                 * via the reloc table. Format: `; <name>@plt`. */
                uint64_t got = 0;
                const char *plt_name = resolve_plt_call(r->a, r, tgt,
                                                        plt_lo, plt_hi,
                                                        have_plt, &got);
                if (plt_name) {
                    printf("  ; %s@plt", plt_name);
                } else {
                    /* Direct call: try to resolve to a known symbol.
                     * No annotation if the target doesn't resolve. */
                    uint64_t off = 0;
                    const char *name = sym_containing(r->a, tgt, &off);
                    if (name && off == 0)
                        printf("  ; %s", name);
                    else if (name)
                        printf("  ; %s+0x%" PRIx64, name, off);
                }
            }
            printf("\n");
        }
        free(window_addrs);
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
