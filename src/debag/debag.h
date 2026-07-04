/* debag.h - 2O9 hybrid sandbox (seccomp fast path + ptrace slow path)
 *
 * Phase 6: Debag ("debug + bag"). A hybrid sandbox that combines
 * seccomp-bpf (kernel-level, nanosecond overhead) with ptrace
 * (userspace, microsecond overhead) for the best of both worlds.
 *
 * How it works:
 *   1. Static analysis scans the ELF binary - extracts syscalls from
 *      dynamic symbols, detects linked libraries, identifies network ops
 *   2. A seccomp-bpf filter is compiled: safe syscalls ALLOW directly
 *      in kernel; dangerous syscalls (execve, connect, open) return
 *      SECCOMP_RET_TRACE to trigger ptrace; unexpected syscalls KILL
 *   3. The target runs at ~90% native speed (seccomp fast path)
 *   4. Only the dangerous 5% of syscalls fall through to ptrace
 *      (slow path) for deep argument inspection
 *
 * vs. Trakker: Trakker uses pure ptrace (every syscall intercepted,
 * ~20% native speed). Debag uses seccomp for the fast path and ptrace
 * only for syscalls that need argument inspection (~90% native speed).
 *
 * Usage: 209 debag [flags] [--] <command> [args...]
 *   --static-scan     Show static analysis results without running
 *   --dynamic-block   Block dangerous syscalls (default: log them)
 *   --fast-mode       Seccomp-only, no ptrace (fastest, least inspection)
 *   --no-net          Block all network syscalls via seccomp
 *   --no-write        Block all write syscalls via seccomp
 *   --verbose         Log every ptrace interception
 */

#ifndef TWO9_DEBAG_H
#define TWO9_DEBAG_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

/* ── Static analysis result ───────────────────────────────────────── */

/* ELF section descriptor, exposed so the static-db REPL can seek/dump. */
typedef struct debag_elf_section {
    char *name;
    uint64_t vaddr;
    uint64_t offset;
    uint64_t size;
    uint32_t type;          /* SHT_* */
    uint64_t flags;         /* SHF_* */
} debag_elf_section_t;

/* ELF program-header / segment descriptor. */
typedef struct debag_elf_segment {
    uint32_t type;          /* PT_* */
    uint32_t flags;         /* PF_* */
    uint64_t vaddr;
    uint64_t offset;
    uint64_t filesz;
    uint64_t memsz;
} debag_elf_segment_t;

/* ELF symbol descriptor. Covers both .symtab and .dynsym entries. */
typedef struct debag_elf_symbol {
    char *name;
    uint64_t vaddr;
    uint64_t size;
    int type;               /* STT_FUNC, STT_OBJECT, ... */
    int bind;               /* STB_LOCAL, STB_GLOBAL, STB_WEAK */
    int is_import;          /* 1 if undefined (from .dynsym, imported) */
} debag_elf_symbol_t;

/* ELF relocation descriptor, populated from the dynamic section's
 * DT_JMPREL (PLT) / DT_REL / DT_RELA tables. The `vaddr` field is
 * r_offset, i.e. the GOT slot address. Used by the static-db REPL's
 * `ii` (to show import GOT addresses) and `s <import_name>` (to seek
 * to the GOT slot). */
typedef struct debag_elf_reloc {
    char *sym_name;        /* resolved name from DT_SYMTAB[sym_idx], or NULL */
    uint64_t vaddr;        /* r_offset, the GOT slot address */
    uint64_t sym_value;    /* st_value of the resolved symbol */
    uint64_t addend;       /* Elf64_Rela.r_addend (0 for Elf64_Rel) */
    uint32_t type;         /* ELF64_R_TYPE(r_info), e.g. R_X86_64_JUMP_SLOT */
    int is_rela;           /* 1 if Elf64_Rela, 0 if Elf64_Rel */
    int is_plt;            /* 1 if from DT_JMPREL, 0 if from DT_REL/DT_RELA */
} debag_elf_reloc_t;

typedef struct debag_analysis {
    char *binary_path;
    int is_dynamic;         /* 1 if dynamically linked */
    int has_network;        /* 1 if binary uses socket/connect/etc */
    int has_file_write;     /* 1 if binary uses open(O_WRONLY)/unlink/etc */
    int has_exec;           /* 1 if binary uses execve/execveat */
    int has_dlopen;         /* 1 if binary links libdl or calls dlopen */
    int has_threads;        /* 1 if binary uses pthread_create/clone */
    int has_mount;          /* 1 if binary uses mount/umount */

    /* Syscalls inferred from dynamic symbols */
    int *allowed_syscalls;  /* array of syscall numbers to ALLOW */
    size_t allowed_count;
    int *traced_syscalls;   /* array of syscall numbers to TRACE */
    size_t traced_count;

    /* Linked libraries */
    char **libs;            /* NULL-terminated list of .so names */
    size_t lib_count;

    /* ELF metadata, populated by debag_analyze(). Used by the static-db
     * and dynamic-db interactive REPLs. Optional - older callers ignore. */
    uint64_t entry_point;   /* e_entry */
    int bits;               /* 32 or 64 */
    int is_big_endian;      /* 1 = big endian, 0 = little endian */
    char *arch_name;        /* "x86-64", "x86", "aarch64", "arm", ... */
    char *binary_type;      /* "EXEC", "DYN" (PIE), "CORE", "REL" */
    debag_elf_section_t  *sections;
    size_t section_count;
    debag_elf_segment_t  *segments;
    size_t segment_count;
    debag_elf_symbol_t   *symbols;   /* .dynsym + .symtab merged, deduped */
    size_t symbol_count;
    debag_elf_reloc_t   *relocs;     /* DT_JMPREL/DT_REL/DT_RELA, dyn-only */
    size_t reloc_count;
} debag_analysis_t;

/* ── Policy ───────────────────────────────────────────────────────── */

typedef struct debag_policy {
    int static_scan_only;   /* --static-scan: analyze but don't run */
    int dynamic_block;      /* --dynamic-block: block dangerous syscalls */
    int fast_mode;          /* --fast-mode: seccomp only, no ptrace */
    int no_net;             /* --no-net: block network syscalls */
    int no_write;           /* --no-write: block write syscalls */
    int verbose;            /* --verbose: log every ptrace event */
} debag_policy_t;

/* ── Result ───────────────────────────────────────────────────────── */

typedef struct debag_result {
    int exit_code;
    unsigned long duration_ms;
    size_t syscalls_total;      /* total syscalls intercepted by ptrace */
    size_t syscalls_blocked;    /* syscalls blocked by policy */
    size_t syscalls_allowed;    /* syscalls allowed by ptrace */
    char **blocked_list;        /* NULL-terminated list of blocked syscall names */
    size_t blocked_count;
} debag_result_t;

/* ── API ──────────────────────────────────────────────────────────── */

/* Statically analyze an ELF binary. Returns NULL on failure.
 * Caller must free with debag_analysis_free(). */
debag_analysis_t *debag_analyze(const char *binary_path);

/* Free an analysis result */
void debag_analysis_free(debag_analysis_t *a);

/* Print analysis results to a file (for --static-scan) */
void debag_analysis_print(const debag_analysis_t *a, FILE *out);

/* Run a command under the hybrid sandbox.
 * Returns a result struct, or NULL on failure.
 * Caller must free with debag_result_free(). */
debag_result_t *debag_run(int argc, const char **argv,
                           const debag_policy_t *policy,
                           const debag_analysis_t *analysis);

/* Free a result */
void debag_result_free(debag_result_t *r);

/* Print a result to a file (JSON format) */
void debag_result_print(const debag_result_t *r, FILE *out);

/* ── .install script analysis ────────────────────────────────────── */

typedef struct script_intent script_intent_t;
typedef struct script_analysis script_analysis_t;

/* Parse a .install script and extract intent (what each function does) */
script_analysis_t *debag_analyze_script(const char *script_path);

/* Free a script analysis */
void debag_free_script_analysis(script_analysis_t *a);

/* Print script analysis in human-readable format */
void debag_print_script_analysis(const script_analysis_t *a, FILE *out);

/* ── Interactive REPLs (db = "debugging") ────────────────────────── */

/* `209 debag --static-db -- <binary>` : rizin-style read-only ELF REPL.
 * Returns process exit code (0 on clean quit). */
int debag_static_db_repl(const char *binary_path);

/* `209 debag --dynamic-db -- <argv>` : gdb-style live debugger REPL.
 * argv[0] is the binary, argv[1..] are its args (NULL-terminated).
 * Returns process exit code (0 on clean quit). */
int debag_dynamic_db_repl(int argc, char **argv);

/* ── ELF pretty-printing helpers (shared by static_analysis + REPLs) ─ */

const char *debag_elf_section_type_name(uint32_t sh_type);
void debag_elf_section_flags_str(uint64_t flags, char *buf, size_t bufsz);
const char *debag_elf_symbol_type_name(int st_type);
const char *debag_elf_symbol_bind_name(int st_bind);
const char *debag_elf_segment_type_name(uint32_t p_type);
const char *debag_elf_reloc_type_name(uint32_t r_type);

#endif /* TWO9_DEBAG_H */
