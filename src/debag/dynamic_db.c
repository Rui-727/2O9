/* dynamic_db.c - gdb-style interactive live debugger REPL for 2O9 debag.
 *
 * `209 debag --dynamic-db -- <binary> [args...]`
 *
 * Forks the target under PTRACE_TRACEME, stops it at the entry point,
 * and presents an interactive debugger. Software breakpoints via the
 * INT3 (0xCC) trick, /proc/PID/mem for memory access (with a
 * PTRACE_PEEKDATA fallback), rbp-chain walk for backtraces, and a
 * minimal x86-64 instruction-length decoder for step-over.
 *
 * Design cribbed from GDB (clean-room reimplementation; GDB is GPL-3.0,
 * 2O9 is GPL-2.0-only). See /home/z/my-project/tool-results/gdb-research.md
 * for the full design notes.
 *
 * Usage: see `help` inside the REPL. The command set is rizin-style
 * (db, dc, ds, dso, dr, px, ps, bt, sym, info, q).
 *
 * Limitations:
 *  - x86-64 only.
 *  - Single-threaded: if the tracee spawns a child or thread, it runs
 *    untraced (we don't set PTRACE_O_TRACEFORK/CLONE).
 *  - Frameless functions (compiled with -fomit-frame-pointer) break the
 *    rbp-chain backtrace walk. The walk terminates gracefully via the
 *    `next_rbp > rbp` sanity check.
 *  - ELF symbols only (no DWARF); symbol resolution falls back to
 *    <unknown + 0x...> for stripped binaries with no useful .symtab
 *    or .dynsym entries.
 *  - Step-over (`dso`) uses a 50-line mini x86-64 instruction decoder
 *    that handles the common call forms (E8 rel32, FF /2). Exotic call
 *    encodings (e.g. with multiple legacy prefixes) may be miscounted;
 *    in that case `dso` falls back to a single step. With libcapstone
 *    available (HAVE_CAPSTONE), the full decoder is used instead.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <elf.h>
#include <limits.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/personality.h>

#ifdef HAVE_CAPSTONE
#include <capstone/capstone.h>
#endif

#include "debag.h"

/* ── Constants ───────────────────────────────────────────────────── */

#define MAX_BPS         64
#define MAX_FRAMES      64
#define LINE_MAX_LEN    512
#define INT3            0xCC

/* ── Breakpoint table ────────────────────────────────────────────── */

typedef struct {
    int        used;          /* slot is occupied */
    int        enabled;       /* user toggle */
    int        inserted;      /* 0xCC currently in target memory */
    int        temporary;     /* auto-remove on hit (for step-over) */
    uintptr_t  addr;          /* runtime address of the breakpoint */
    uint8_t    shadow;        /* original byte saved before 0xCC write */
    char       spec[128];     /* original user input for `db` (no args) */
} dyn_bp_t;

/* ── Session state ───────────────────────────────────────────────── */

typedef struct {
    pid_t                pid;
    int                  mem_fd;          /* /proc/PID/mem, or -1 */
    int                  alive;           /* child still running */
    int                  last_sig;        /* last stop signal (0 = none) */
    int                  exited_code;     /* WEXITSTATUS if exited */
    int                  signaled_sig;    /* WTERMSIG if killed by signal */
    uintptr_t            pending_bp;      /* bp at current rip whose 0xCC has
                                           * been removed and must be
                                           * re-inserted after the next
                                           * resume */
    uintptr_t            load_offset;     /* PIE base offset:
                                           * runtime_rip_at_entry - e_entry */
    debag_analysis_t    *analysis;        /* static analysis result */
    dyn_bp_t             bps[MAX_BPS];
    char                 last_cmd[LINE_MAX_LEN];
    struct user_regs_struct regs;         /* last-fetched register snapshot */
    int                  regs_valid;
    /* Signal disposition tables, indexed by signal number (1..NSIG-1).
     * Mirrors gdb's signal_stop/signal_print/signal_program model
     * (gdb/infrun.c:332-334). Clean-room reimplementation; gdb is
     * GPL-3.0, 2O9 is GPL-2.0-only.
     *   sig_stop[sig]    = should debag stop and return to the REPL?
     *   sig_print[sig]   = should debag print a one-line notification?
     *   sig_program[sig] = should debag forward the signal to the child? */
    unsigned char        sig_stop[NSIG];
    unsigned char        sig_print[NSIG];
    unsigned char        sig_program[NSIG];
} dyn_session_t;

/* ── Forward declarations ────────────────────────────────────────── */

static int  session_init(dyn_session_t *s, int argc, char **argv);
static void session_destroy(dyn_session_t *s);
static int  refresh_regs(dyn_session_t *s);
static int  handle_stop(dyn_session_t *s, int status);
static int  resume_step_over_bp(dyn_session_t *s);

static ssize_t mem_read(dyn_session_t *s, uintptr_t addr,
                        void *buf, size_t len);
static int     mem_write_byte(dyn_session_t *s, uintptr_t addr, uint8_t b);

/* ── Signal disposition helpers ──────────────────────────────────── */

/* Populate sig_stop/sig_print/sig_program with gdb's default
 * dispositions. Most signals stop+print+pass; a small set of benign
 * signals that programs typically handle internally (SIGALRM, SIGCHLD,
 * SIGUSR1/2, SIGIO, SIGURG, SIGWINCH, SIGPIPE) pass through without
 * stopping, so that debag doesn't break timers, child reaping, or
 * window-resize handling by default. */
static void sig_init_defaults(dyn_session_t *s)
{
    for (int i = 0; i < NSIG; i++) {
        s->sig_stop[i]    = 1;
        s->sig_print[i]   = 1;
        s->sig_program[i] = 1;
    }
    /* Pass-through (nostop, noprint, pass) for benign signals. */
    static const int pass_through[] = {
        SIGALRM, SIGCHLD, SIGUSR1, SIGUSR2, SIGIO, SIGURG, SIGWINCH, SIGPIPE,
        SIGVTALRM, SIGPROF, 0
    };
    for (const int *p = pass_through; *p; p++) {
        if (*p > 0 && *p < NSIG) {
            s->sig_stop[*p]    = 0;
            s->sig_print[*p]   = 0;
            /* sig_program stays 1 (pass). */
        }
    }
}

/* Return "SIGSEGV", "SIGINT", "SIGALRM", ... for a signal number.
 * Uses a static buffer (REPL is single-threaded). Returns "?" for
 * unknown signals. */
static const char *sig_full_name(int sig)
{
    static char buf[32];
    if (sig < 1 || sig >= NSIG) return "?";
    const char *abbr = sigabbrev_np(sig);
    if (!abbr) return "?";
    snprintf(buf, sizeof(buf), "SIG%s", abbr);
    return buf;
}

/* Parse a signal name. Accepts "SIGSEGV", "SEGV", or a decimal number
 * like "11". Returns the signal number, or -1 on failure. */
static int sig_from_name(const char *name)
{
    if (!name || !*name) return -1;

    /* Numeric form. */
    if (isdigit((unsigned char)name[0])) {
        char *end = NULL;
        long v = strtol(name, &end, 10);
        if (*end == '\0' && v > 0 && v < NSIG) return (int)v;
        return -1;
    }

    /* Strip an optional "SIG" prefix (case-insensitive). */
    const char *n = name;
    if (strncasecmp(n, "SIG", 3) == 0) n += 3;

    /* Match against sigabbrev_np for every valid signal number. */
    for (int sig = 1; sig < NSIG; sig++) {
        const char *abbr = sigabbrev_np(sig);
        if (abbr && strcasecmp(abbr, n) == 0)
            return sig;
    }
    return -1;
}

/* ── Symbol lookup ───────────────────────────────────────────────── */

/* Find the STT_FUNC symbol whose [vaddr, vaddr+size) range contains
 * runtime_addr (after applying load_offset). Returns the symbol name
 * (borrowed pointer) and the offset within the symbol. Returns NULL
 * if no symbol covers the address. */
static const char *sym_for_addr(dyn_session_t *s, uintptr_t runtime_addr,
                                uint64_t *offset_out)
{
    if (!s->analysis || s->analysis->symbol_count == 0)
        return NULL;

    uint64_t file_addr = (uint64_t)runtime_addr - s->load_offset;
    const debag_elf_symbol_t *best = NULL;

    for (size_t i = 0; i < s->analysis->symbol_count; i++) {
        const debag_elf_symbol_t *sym = &s->analysis->symbols[i];
        if (sym->is_import) continue;
        if (sym->type != STT_FUNC && sym->type != STT_GNU_IFUNC) continue;
        if (sym->vaddr == 0) continue;

        if (sym->size > 0) {
            /* Range is well-defined: check containment. */
            if (file_addr >= sym->vaddr && file_addr < sym->vaddr + sym->size) {
                best = sym;
                break;
            }
        } else {
            /* Unknown size: accept if vaddr <= file_addr and pick the
             * largest such vaddr (closest preceding symbol). */
            if (sym->vaddr <= file_addr) {
                if (!best || sym->vaddr > best->vaddr)
                    best = sym;
            }
        }
    }

    if (!best) return NULL;
    if (offset_out) *offset_out = file_addr - best->vaddr;
    return best->name;
}

/* Find a function symbol by name. Returns its runtime address
 * (vaddr + load_offset) or 0 if not found. */
static uintptr_t sym_by_name(dyn_session_t *s, const char *name)
{
    if (!s->analysis || !name) return 0;
    for (size_t i = 0; i < s->analysis->symbol_count; i++) {
        const debag_elf_symbol_t *sym = &s->analysis->symbols[i];
        if (sym->is_import) continue;
        if (sym->type != STT_FUNC && sym->type != STT_GNU_IFUNC) continue;
        if (sym->name && strcmp(sym->name, name) == 0)
            return (uintptr_t)(sym->vaddr + s->load_offset);
    }
    return 0;
}

/* ── Address / register token parser ─────────────────────────────── */

/* Register name table (x86-64). Used for both `dr <reg>=<val>` and
 * `px <reg> <len>`. Returns a pointer into the regs struct, or NULL. */
struct reg_entry { const char *name; size_t offset; };
static const struct reg_entry reg_table[] = {
    {"rax",    offsetof(struct user_regs_struct, rax)},
    {"rbx",    offsetof(struct user_regs_struct, rbx)},
    {"rcx",    offsetof(struct user_regs_struct, rcx)},
    {"rdx",    offsetof(struct user_regs_struct, rdx)},
    {"rsi",    offsetof(struct user_regs_struct, rsi)},
    {"rdi",    offsetof(struct user_regs_struct, rdi)},
    {"rbp",    offsetof(struct user_regs_struct, rbp)},
    {"rsp",    offsetof(struct user_regs_struct, rsp)},
    {"r8",     offsetof(struct user_regs_struct, r8)},
    {"r9",     offsetof(struct user_regs_struct, r9)},
    {"r10",    offsetof(struct user_regs_struct, r10)},
    {"r11",    offsetof(struct user_regs_struct, r11)},
    {"r12",    offsetof(struct user_regs_struct, r12)},
    {"r13",    offsetof(struct user_regs_struct, r13)},
    {"r14",    offsetof(struct user_regs_struct, r14)},
    {"r15",    offsetof(struct user_regs_struct, r15)},
    {"rip",    offsetof(struct user_regs_struct, rip)},
    {"eflags", offsetof(struct user_regs_struct, eflags)},
    {"orig_rax", offsetof(struct user_regs_struct, orig_rax)},
    {"cs",     offsetof(struct user_regs_struct, cs)},
    {"ss",     offsetof(struct user_regs_struct, ss)},
    {"fs_base",offsetof(struct user_regs_struct, fs_base)},
    {"gs_base",offsetof(struct user_regs_struct, gs_base)},
    {NULL, 0}
};

static uint64_t *reg_lookup(struct user_regs_struct *regs, const char *name)
{
    if (!regs || !name) return NULL;
    for (const struct reg_entry *e = reg_table; e->name; e++) {
        if (strcasecmp(e->name, name) == 0)
            return (uint64_t *)((char *)regs + e->offset);
    }
    return NULL;
}

/* Try to parse a token as a register name (returns the value) or as a
 * hex/decimal address. Returns 1 on success, 0 on failure. */
static int parse_addr_or_reg(dyn_session_t *s, const char *tok,
                             uintptr_t *out_addr)
{
    if (!tok || !*tok) return 0;

    /* Register reference? */
    if (s->regs_valid) {
        uint64_t *p = reg_lookup(&s->regs, tok);
        if (p) {
            *out_addr = (uintptr_t)*p;
            return 1;
        }
    }

    /* Symbol name? (only if not parseable as a number) */
    int looks_numeric =
        (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) ||
        isdigit((unsigned char)tok[0]) ||
        tok[0] == '-';

    if (!looks_numeric) {
        uintptr_t a = sym_by_name(s, tok);
        if (a) { *out_addr = a; return 1; }
        return 0;
    }

    /* Numeric: try hex first (gdb-style), fall back to decimal. */
    errno = 0;
    char *end = NULL;
    unsigned long long v;
    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))
        v = strtoull(tok + 2, &end, 16);
    else
        v = strtoull(tok, &end, 16);   /* gdb convention: bare numbers are hex */
    if (end == tok || (end && *end != '\0')) {
        /* try decimal */
        errno = 0;
        v = strtoull(tok, &end, 10);
        if (end == tok || (end && *end != '\0')) return 0;
    }
    *out_addr = (uintptr_t)v;
    return 1;
}

/* ── Memory access ───────────────────────────────────────────────── */

static ssize_t mem_read(dyn_session_t *s, uintptr_t addr,
                        void *buf, size_t len)
{
    if (!s->alive) return -1;

    if (s->mem_fd >= 0) {
        ssize_t n = pread(s->mem_fd, buf, len, (off_t)addr);
        if (n == (ssize_t)len) return n;
        if (n > 0) return n;     /* partial: caller decides what to do */
        /* fall through to PEEKDATA on EIO/etc. */
    }

    /* PTRACE_PEEKDATA fallback: word-by-word. */
    size_t copied = 0;
    while (copied < len) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, s->pid,
                           (void *)(addr + copied), NULL);
        if (word == -1 && errno != 0) return (ssize_t)copied;
        size_t chunk = len - copied;
        if (chunk > sizeof(long)) chunk = sizeof(long);
        memcpy((char *)buf + copied, &word, chunk);
        copied += chunk;
    }
    return (ssize_t)copied;
}

/* Write a single byte to target memory. Used for inserting and
 * removing INT3 breakpoints. PTRACE_POKETEXT writes a full word, so
 * we read-modify-write to avoid clobbering adjacent bytes. */
static int mem_write_byte(dyn_session_t *s, uintptr_t addr, uint8_t b)
{
    if (!s->alive) return -1;
    errno = 0;
    long word = ptrace(PTRACE_PEEKTEXT, s->pid, (void *)addr, NULL);
    if (word == -1 && errno != 0) return -1;
    long newword = (word & ~(long)0xff) | (long)b;
    if (ptrace(PTRACE_POKETEXT, s->pid, (void *)addr, (void *)newword) < 0)
        return -1;
    return 0;
}

/* ── Breakpoint helpers ──────────────────────────────────────────── */

static dyn_bp_t *bp_find(dyn_session_t *s, uintptr_t addr)
{
    for (int i = 0; i < MAX_BPS; i++)
        if (s->bps[i].used && s->bps[i].addr == addr)
            return &s->bps[i];
    return NULL;
}

static dyn_bp_t *bp_alloc(dyn_session_t *s)
{
    for (int i = 0; i < MAX_BPS; i++)
        if (!s->bps[i].used) return &s->bps[i];
    return NULL;
}

static int bp_insert(dyn_session_t *s, uintptr_t addr,
                     int temporary, const char *spec)
{
    if (bp_find(s, addr)) {
        fprintf(stderr, "breakpoint already set at 0x%lx\n",
                (unsigned long)addr);
        return -1;
    }
    dyn_bp_t *bp = bp_alloc(s);
    if (!bp) {
        fprintf(stderr, "breakpoint table full (max %d)\n", MAX_BPS);
        return -1;
    }

    /* Save the original byte, then write 0xCC. */
    errno = 0;
    long word = ptrace(PTRACE_PEEKTEXT, s->pid, (void *)addr, NULL);
    if (word == -1 && errno != 0) {
        fprintf(stderr, "cannot read memory at 0x%lx: %s\n",
                (unsigned long)addr, strerror(errno));
        return -1;
    }
    uint8_t orig = (uint8_t)(word & 0xff);

    bp->used       = 1;
    bp->enabled    = 1;
    bp->inserted   = 0;
    bp->temporary  = temporary;
    bp->addr       = addr;
    bp->shadow     = orig;
    if (spec) {
        strncpy(bp->spec, spec, sizeof(bp->spec) - 1);
        bp->spec[sizeof(bp->spec) - 1] = '\0';
    } else {
        bp->spec[0] = '\0';
    }

    if (mem_write_byte(s, addr, INT3) < 0) {
        fprintf(stderr, "cannot write breakpoint at 0x%lx: %s\n",
                (unsigned long)addr, strerror(errno));
        bp->used = 0;
        return -1;
    }
    bp->inserted = 1;
    return 0;
}

static int bp_remove(dyn_session_t *s, uintptr_t addr)
{
    dyn_bp_t *bp = bp_find(s, addr);
    if (!bp) {
        fprintf(stderr, "no breakpoint at 0x%lx\n", (unsigned long)addr);
        return -1;
    }
    if (bp->inserted) {
        if (mem_write_byte(s, bp->addr, bp->shadow) < 0) {
            fprintf(stderr, "warning: could not restore byte at 0x%lx\n",
                    (unsigned long)bp->addr);
        }
        bp->inserted = 0;
    }
    if (s->pending_bp == bp->addr) s->pending_bp = 0;
    bp->used = 0;
    return 0;
}

static void bp_list(dyn_session_t *s)
{
    int any = 0;
    for (int i = 0; i < MAX_BPS; i++) {
        dyn_bp_t *bp = &s->bps[i];
        if (!bp->used) continue;
        any = 1;
        printf("  %s 0x%016lx  %s%s\n",
               bp->enabled ? "en" : "di",
               (unsigned long)bp->addr,
               bp->spec[0] ? bp->spec : "",
               bp->temporary ? "  (temp)" : "");
    }
    if (!any) printf("  (no breakpoints)\n");
}

/* ── Resume helpers ──────────────────────────────────────────────── */

/* If the child is currently sitting on a breakpoint whose 0xCC has
 * been removed (pending_bp), single-step it past the bp byte and
 * re-insert the 0xCC. After this call, the child is stopped again
 * one instruction past the bp, with all breakpoints back in place.
 *
 * Returns 0 on success, -1 on error. */
static int resume_step_over_bp(dyn_session_t *s)
{
    if (!s->pending_bp) return 0;
    uintptr_t bp_addr = s->pending_bp;

    if (ptrace(PTRACE_SINGLESTEP, s->pid, NULL, NULL) < 0) {
        perror("ptrace SINGLESTEP");
        return -1;
    }
    int status;
    if (waitpid(s->pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }

    /* Re-insert the 0xCC at the bp address. */
    dyn_bp_t *bp = bp_find(s, bp_addr);
    if (bp && bp->used && !bp->inserted) {
        if (mem_write_byte(s, bp->addr, INT3) == 0)
            bp->inserted = 1;
    }
    s->pending_bp = 0;

    /* Did the single-step itself cause the child to exit or signal? */
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        s->alive = 0;
        if (WIFEXITED(status))
            s->exited_code = WEXITSTATUS(status);
        else
            s->signaled_sig = WTERMSIG(status);
        return -1;
    }
    return 0;
}

/* Decode a SIGTRAP stop. On x86-64, when the kernel reports SIGTRAP
 * for an INT3, rip points to the byte AFTER the 0xCC. We rewind rip
 * by 1, then look up the breakpoint. If found, restore the shadow
 * byte and mark pending_bp so the next resume single-steps over it. */
static int handle_sigtrap(dyn_session_t *s)
{
    if (refresh_regs(s) < 0) return -1;

    /* Distinguish syscall traps (sig | 0x80) from real SIGTRAPs.
     * We don't use PTRACE_SYSCALL in this REPL, so we shouldn't see
     * them, but be defensive. */
    siginfo_t si;
    if (ptrace(PTRACE_GETSIGINFO, s->pid, NULL, &si) == 0) {
        if (si.si_code == TRAP_TRACE) {
            /* Single-step trap. No bp to rewind. The SIGTRAP is
             * synthetic (generated by PTRACE_SINGLESTEP), so do NOT
             * forward it on the next continue. */
            s->last_sig = 0;
            return 0;
        }
    }

    /* Assume breakpoint trap: rewind rip. */
    s->regs.rip -= 1;
    if (ptrace(PTRACE_SETREGS, s->pid, NULL, &s->regs) < 0) {
        perror("ptrace SETREGS");
        return -1;
    }

    dyn_bp_t *bp = bp_find(s, (uintptr_t)s->regs.rip);
    if (bp && bp->inserted) {
        /* Restore the original byte so the next resume can step over. */
        if (mem_write_byte(s, bp->addr, bp->shadow) == 0) {
            bp->inserted = 0;
            s->pending_bp = bp->addr;
        }
        if (bp->temporary) {
            printf("hit temp breakpoint at 0x%016llx",
                   (unsigned long long)s->regs.rip);
            uint64_t off;
            const char *name = sym_for_addr(s, (uintptr_t)s->regs.rip, &off);
            if (name) printf(" <%s+0x%llx>", name, (unsigned long long)off);
            printf("\n");
            /* Auto-remove temp bps. */
            bp->used = 0;
            s->pending_bp = 0;  /* it's already restored; no re-insert */
        } else {
            printf("hit breakpoint at 0x%016llx",
                   (unsigned long long)s->regs.rip);
            uint64_t off;
            const char *name = sym_for_addr(s, (uintptr_t)s->regs.rip, &off);
            if (name) printf(" <%s+0x%llx>", name, (unsigned long long)off);
            printf("\n");
        }
    } else {
        /* SIGTRAP not at a known bp. Probably an INT3 the program
         * executed itself, or a single-step trap. Leave rip as-is. */
        printf("SIGTRAP at 0x%016llx (no breakpoint; possibly int3 in code)\n",
               (unsigned long long)s->regs.rip);
    }
    /* SIGTRAP from a breakpoint (or unknown int3) is synthetic from
     * ptrace's perspective. Do NOT forward it on the next continue —
     * forwarding SIGTRAP would confuse the child's signal handling
     * and turn every breakpoint hit into a SIGTRAP delivery. */
    s->last_sig = 0;
    return 0;
}

/* Top-level stop handler. Returns 0 if the child is still alive and
 * stopped (REPL can continue), -1 if it exited or was killed.
 *
 * For non-SIGTRAP signals, consults the sig_stop/sig_print/sig_program
 * tables. If sig_stop[sig] is false, the signal is auto-continued
 * (forwarded iff sig_program[sig]) without returning to the REPL —
 * this lets timers, SIGCHLD, etc. run without bothering the user. */
static int handle_stop(dyn_session_t *s, int status)
{
    while (1) {
        if (WIFEXITED(status)) {
            s->alive = 0;
            s->exited_code = WEXITSTATUS(status);
            printf("[child exited with code %d]\n", s->exited_code);
            return -1;
        }
        if (WIFSIGNALED(status)) {
            s->alive = 0;
            s->signaled_sig = WTERMSIG(status);
            printf("[child killed by signal %d (%s)]\n",
                   s->signaled_sig, strsignal(s->signaled_sig));
            return -1;
        }
        if (!WIFSTOPPED(status)) {
            fprintf(stderr, "[unexpected waitpid status 0x%x]\n", status);
            return -1;
        }

        int sig = WSTOPSIG(status);
        s->last_sig = sig;

        /* SIGTRAP is always a debugger event (breakpoint or single-step).
         * Always stop here and let handle_sigtrap decide. */
        if (sig == SIGTRAP) {
            return handle_sigtrap(s);
        }

        /* Out-of-range signal: be conservative, stop and let the user
         * inspect. */
        if (sig < 1 || sig >= NSIG) {
            refresh_regs(s);
            printf("[child stopped by signal %d at 0x%016llx]\n",
                   sig, (unsigned long long)s->regs.rip);
            return 0;
        }

        /* Print a gdb-style notification? */
        if (s->sig_print[sig]) {
            printf("Program received signal %s, %s.\n",
                   sig_full_name(sig), strsignal(sig));
        }

        /* Stop and return to the REPL? */
        if (s->sig_stop[sig]) {
            refresh_regs(s);
            printf("[stopped at 0x%016llx]\n",
                   (unsigned long long)s->regs.rip);
            return 0;
        }

        /* Auto-continue: forward the signal iff sig_program says so. */
        int deliver = s->sig_program[sig] ? sig : 0;
        if (ptrace(PTRACE_CONT, s->pid, NULL,
                   (void *)(intptr_t)deliver) < 0) {
            perror("ptrace CONT (auto-continue)");
            return -1;
        }
        if (waitpid(s->pid, &status, 0) < 0) {
            perror("waitpid");
            return -1;
        }
        /* Loop back to interpret the next stop. */
    }
}

/* ── Mini x86-64 instruction decoder for `dso` ───────────────────── */

/* Returns the instruction length if the instruction at `insn` is a
 * call (E8 rel32 or FF /2 or FF /3 indirect), 0 if not a call,
 * -1 on decode error / not enough bytes. */
static int call_insn_length(const uint8_t *insn, size_t avail)
{
#ifdef HAVE_CAPSTONE
    csh handle;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) == CS_ERR_OK) {
        cs_insn *insns;
        size_t n = cs_disasm(handle, insn, avail, 0, 1, &insns);
        int result = -1;
        if (n > 0) {
            if (insns[0].id == X86_INS_CALL || insns[0].id == X86_INS_LCALL)
                result = (int)insns[0].size;
            else
                result = 0;
            cs_free(insns, n);
        }
        cs_close(&handle);
        if (result >= 0) return result;
        /* fall through to manual decoder on capstone failure */
    }
#endif

    size_t i = 0;
    /* Skip legacy prefixes (up to 14, the x86 max). */
    while (i < avail && i < 14) {
        uint8_t b = insn[i];
        if (b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
            b == 0x64 || b == 0x65 || b == 0x66 || b == 0x67) {
            i++;
        } else break;
    }
    if (i >= avail) return -1;

    /* REX prefix. */
    if ((insn[i] & 0xF0) == 0x40) {
        i++;
        if (i >= avail) return -1;
    }

    uint8_t op = insn[i];
    size_t after_op = i + 1;

    if (op == 0xE8) {
        /* E8 rel32: 5 bytes total from opcode start. */
        if (after_op + 4 > avail) return -1;
        return (int)(after_op + 4);
    }
    if (op == 0xFF) {
        if (after_op >= avail) return -1;
        uint8_t modrm = insn[after_op];
        int reg = (modrm >> 3) & 7;
        int mod = (modrm >> 6) & 3;
        int rm  = modrm & 7;
        if (reg != 2 && reg != 3) return 0;  /* not a call form */

        size_t p = after_op + 1;
        if (mod != 3 && rm == 4) {
            /* SIB byte. */
            if (p >= avail) return -1;
            uint8_t sib = insn[p];
            p++;
            int base = sib & 7;
            if (mod == 0 && base == 5) p += 4;  /* disp32 */
        } else if (mod == 0 && rm == 5) {
            p += 4;  /* RIP-relative disp32 */
        } else if (mod == 1) {
            p += 1;  /* disp8 */
        } else if (mod == 2) {
            p += 4;  /* disp32 */
        }
        if (p > avail) return -1;
        return (int)p;
    }
    return 0;
}

/* ── eflags pretty-print ─────────────────────────────────────────── */

static void eflags_str(uint64_t efl, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "[ %s%s%s%s%s%s%s%s%s]",
        (efl & 0x001) ? "CF " : "",
        (efl & 0x004) ? "PF " : "",
        (efl & 0x010) ? "AF " : "",
        (efl & 0x040) ? "ZF " : "",
        (efl & 0x080) ? "SF " : "",
        (efl & 0x100) ? "TF " : "",
        (efl & 0x200) ? "IF " : "",
        (efl & 0x400) ? "DF " : "",
        (efl & 0x800) ? "OF " : "");
}

/* ── Register refresh ────────────────────────────────────────────── */

static int refresh_regs(dyn_session_t *s)
{
    if (!s->alive) { s->regs_valid = 0; return -1; }
    if (ptrace(PTRACE_GETREGS, s->pid, NULL, &s->regs) < 0) {
        perror("ptrace GETREGS");
        s->regs_valid = 0;
        return -1;
    }
    s->regs_valid = 1;
    return 0;
}

/* ── Command handlers ────────────────────────────────────────────── */

/* Each handler returns 0 on success, -1 on error. A return value of
 * 1 signals "quit the REPL". Any other return continues the loop. */

static int cmd_db(dyn_session_t *s, char *args)
{
    if (!args || !*args) {
        bp_list(s);
        return 0;
    }
    /* Parse: either a symbol name (single token, no leading 0x or
     * digit) or an address (hex). */
    char *tok = strtok(args, " \t");
    if (!tok) { bp_list(s); return 0; }

    uintptr_t addr;
    if (parse_addr_or_reg(s, tok, &addr)) {
        return bp_insert(s, addr, 0, tok);
    }
    fprintf(stderr, "cannot resolve '%s' as address or symbol\n", tok);
    return -1;
}

static int cmd_db_remove(dyn_session_t *s, char *args)
{
    if (!args || !*args) {
        fprintf(stderr, "usage: db- <addr>\n");
        return -1;
    }
    char *tok = strtok(args, " \t");
    uintptr_t addr;
    if (!parse_addr_or_reg(s, tok, &addr)) {
        fprintf(stderr, "cannot parse '%s' as address\n", tok);
        return -1;
    }
    return bp_remove(s, addr);
}

static int cmd_dc(dyn_session_t *s, char *args)
{
    (void)args;
    if (!s->alive) {
        fprintf(stderr, "child is not running\n");
        return -1;
    }
    /* If we're sitting on a bp, step past it first. */
    if (resume_step_over_bp(s) < 0) {
        if (!s->alive) return 0;
        return -1;
    }
    /* Forward the signal that last stopped the child to it on continue,
     * gated by the sig_program disposition table. Without this, any
     * program using SIGALRM/SIGCHLD/SIGUSR1/SIGUSR2/SIGIO for legitimate
     * purposes is silently broken, and a SIGSEGV becomes an infinite
     * re-fault loop (the kernel re-executes the faulting instruction,
     * faults again, we swallow, repeat).
     *
     * s->last_sig is 0 after a breakpoint hit (handle_sigtrap clears
     * it) and after the initial execve stop (session_init never sets
     * it), so those cases correctly forward nothing. Real signals are
     * forwarded iff the user hasn't set `handle <sig> nopass`. */
    int deliver = 0;
    if (s->last_sig > 0 && s->last_sig < NSIG && s->sig_program[s->last_sig])
        deliver = s->last_sig;
    if (ptrace(PTRACE_CONT, s->pid, NULL, (void *)(intptr_t)deliver) < 0) {
        perror("ptrace CONT");
        return -1;
    }
    int status;
    if (waitpid(s->pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    handle_stop(s, status);
    return 0;
}

static int cmd_ds(dyn_session_t *s, char *args)
{
    (void)args;
    if (!s->alive) {
        fprintf(stderr, "child is not running\n");
        return -1;
    }
    /* If we're sitting on a bp, step past the bp byte (this counts as
     * the one instruction step). Otherwise, just single-step. */
    if (s->pending_bp) {
        if (resume_step_over_bp(s) < 0) return -1;
    } else {
        if (ptrace(PTRACE_SINGLESTEP, s->pid, NULL, NULL) < 0) {
            perror("ptrace SINGLESTEP");
            return -1;
        }
        int status;
        if (waitpid(s->pid, &status, 0) < 0) {
            perror("waitpid");
            return -1;
        }
        if (handle_stop(s, status) < 0) return 0;
    }
    refresh_regs(s);
    return 0;
}

static int cmd_dso(dyn_session_t *s, char *args)
{
    (void)args;
    if (!s->alive) {
        fprintf(stderr, "child is not running\n");
        return -1;
    }
    if (!s->regs_valid) refresh_regs(s);

    /* If we're sitting on a bp, the 0xCC has already been removed, so
     * reading memory at rip gives the original instruction. Good. */
    uint8_t insn[16] = {0};
    if (mem_read(s, (uintptr_t)s->regs.rip, insn, sizeof(insn)) <= 0) {
        fprintf(stderr, "cannot read instruction at 0x%llx\n",
                (unsigned long long)s->regs.rip);
        return -1;
    }
    int len = call_insn_length(insn, sizeof(insn));

    /* If we're sitting on a bp, step past the bp byte first. This
     * executes the instruction at rip. */
    if (s->pending_bp) {
        if (resume_step_over_bp(s) < 0) return -1;
        /* After stepping past the bp, we're now at rip + (bp instruction
         * length). For dso on a call, this is the call's return address
         * — which is exactly what we want (we've "stepped over" by one
         * instruction). So just refresh regs and return. */
        refresh_regs(s);
        return 0;
    }

    if (len <= 0) {
        /* Not a call, or decode failed. Single-step instead. */
        if (ptrace(PTRACE_SINGLESTEP, s->pid, NULL, NULL) < 0) {
            perror("ptrace SINGLESTEP");
            return -1;
        }
        int status;
        if (waitpid(s->pid, &status, 0) < 0) {
            perror("waitpid");
            return -1;
        }
        if (handle_stop(s, status) < 0) return 0;
        refresh_regs(s);
        return 0;
    }

    /* It's a call. Set a temp bp at rip + len, continue, hit it,
     * remove it. */
    uintptr_t next_pc = (uintptr_t)s->regs.rip + (uintptr_t)len;
    if (bp_insert(s, next_pc, 1, "step-over temp") < 0) {
        /* Couldn't set temp bp. Fall back to single-step. */
        if (ptrace(PTRACE_SINGLESTEP, s->pid, NULL, NULL) < 0) {
            perror("ptrace SINGLESTEP");
            return -1;
        }
        int status;
        if (waitpid(s->pid, &status, 0) < 0) {
            perror("waitpid");
            return -1;
        }
        if (handle_stop(s, status) < 0) return 0;
        refresh_regs(s);
        return 0;
    }

    /* Forward any pending signal (gated by sig_program) so that `dso`
     * after a signal stop still delivers the signal to the child. */
    int dso_deliver = 0;
    if (s->last_sig > 0 && s->last_sig < NSIG && s->sig_program[s->last_sig])
        dso_deliver = s->last_sig;
    if (ptrace(PTRACE_CONT, s->pid, NULL, (void *)(intptr_t)dso_deliver) < 0) {
        perror("ptrace CONT");
        return -1;
    }
    int status;
    if (waitpid(s->pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    /* If the stop was at our temp bp, handle_sigtrap auto-removes it
     * (because temporary=1 clears bp->used on hit). If the child
     * exited or was killed, clean up the slot since the temp bp has
     * no further purpose. Otherwise the child stopped somewhere other
     * than next_pc (a different breakpoint, a signal with sig_stop
     * set, etc.) — LEAVE the temp bp installed so a later `dc` will
     * hit it when the interrupting source is resolved. This mirrors
     * gdb's step-resume breakpoint (gdb/breakpoint.h:119,
     * bp_step_resume), which stays installed across signal stops and
     * is hit when the handler returns (gdb/infrun.c:8013-8048). */
    int orig_alive = s->alive;
    handle_stop(s, status);

    dyn_bp_t *tbp = bp_find(s, next_pc);
    if (tbp && tbp->used && tbp->temporary) {
        if (!s->alive) {
            /* Child is gone; just clear the slot. The mem_write_byte
             * may fail on a dead child — that's fine, the slot is
             * purely bookkeeping at this point. */
            if (tbp->inserted) {
                mem_write_byte(s, tbp->addr, tbp->shadow);
                tbp->inserted = 0;
            }
            tbp->used = 0;
        } else {
            /* Step-over was interrupted at a different rip. Leave the
             * temp bp installed; it'll be auto-removed by
             * handle_sigtrap when next_pc is eventually hit (via `dc`
             * or another `dso`), or the user can clear it with `db-`. */
            refresh_regs(s);
            printf("step-over interrupted at 0x%016llx; "
                   "temp bp at 0x%016lx remains\n",
                   (unsigned long long)s->regs.rip,
                   (unsigned long)next_pc);
        }
    }
    if (orig_alive) refresh_regs(s);
    return 0;
}

static int cmd_dr(dyn_session_t *s, char *args)
{
    if (!s->alive) {
        fprintf(stderr, "child is not running\n");
        return -1;
    }
    if (!args || !*args) {
        /* Print all GP regs in 2-column format. */
        if (refresh_regs(s) < 0) return -1;
        struct user_regs_struct *r = &s->regs;
        char efl[64];
        eflags_str(r->eflags, efl, sizeof(efl));
        printf("rax 0x%016llx  rbx 0x%016llx\n",
               (unsigned long long)r->rax, (unsigned long long)r->rbx);
        printf("rcx 0x%016llx  rdx 0x%016llx\n",
               (unsigned long long)r->rcx, (unsigned long long)r->rdx);
        printf("rsi 0x%016llx  rdi 0x%016llx\n",
               (unsigned long long)r->rsi, (unsigned long long)r->rdi);
        printf("rbp 0x%016llx  rsp 0x%016llx\n",
               (unsigned long long)r->rbp, (unsigned long long)r->rsp);
        printf("r8  0x%016llx  r9  0x%016llx\n",
               (unsigned long long)r->r8,  (unsigned long long)r->r9);
        printf("r10 0x%016llx  r11 0x%016llx\n",
               (unsigned long long)r->r10, (unsigned long long)r->r11);
        printf("r12 0x%016llx  r13 0x%016llx\n",
               (unsigned long long)r->r12, (unsigned long long)r->r13);
        printf("r14 0x%016llx  r15 0x%016llx\n",
               (unsigned long long)r->r14, (unsigned long long)r->r15);
        printf("rip 0x%016llx  eflags %s\n",
               (unsigned long long)r->rip, efl);
        return 0;
    }

    /* dr <reg>=<val> */
    char *eq = strchr(args, '=');
    if (!eq) {
        /* dr <reg> — print single register. */
        char *tok = strtok(args, " \t");
        if (!tok) return -1;
        if (refresh_regs(s) < 0) return -1;
        uint64_t *p = reg_lookup(&s->regs, tok);
        if (!p) {
            fprintf(stderr, "unknown register '%s'\n", tok);
            return -1;
        }
        char efl[64];
        if (strcasecmp(tok, "eflags") == 0) {
            eflags_str(*p, efl, sizeof(efl));
            printf("%s 0x%016llx  %s\n", tok, (unsigned long long)*p, efl);
        } else {
            printf("%s 0x%016llx\n", tok, (unsigned long long)*p);
        }
        return 0;
    }
    *eq = '\0';
    char *regname = args;
    char *valstr  = eq + 1;
    /* Trim trailing space on regname. */
    char *end = regname + strlen(regname);
    while (end > regname && isspace((unsigned char)*(end-1))) *--end = '\0';
    /* Trim leading space on valstr. */
    while (*valstr && isspace((unsigned char)*valstr)) valstr++;

    if (refresh_regs(s) < 0) return -1;
    uint64_t *p = reg_lookup(&s->regs, regname);
    if (!p) {
        fprintf(stderr, "unknown register '%s'\n", regname);
        return -1;
    }
    errno = 0;
    char *vend = NULL;
    unsigned long long v;
    if (valstr[0] == '0' && (valstr[1] == 'x' || valstr[1] == 'X'))
        v = strtoull(valstr + 2, &vend, 16);
    else
        v = strtoull(valstr, &vend, 16);
    if (vend == valstr || (*vend != '\0' && !isspace((unsigned char)*vend))) {
        fprintf(stderr, "cannot parse value '%s'\n", valstr);
        return -1;
    }
    *p = (uint64_t)v;
    if (ptrace(PTRACE_SETREGS, s->pid, NULL, &s->regs) < 0) {
        perror("ptrace SETREGS");
        return -1;
    }
    printf("%s = 0x%016llx\n", regname, (unsigned long long)v);
    return 0;
}

static int cmd_px(dyn_session_t *s, char *args)
{
    if (!s->alive) {
        fprintf(stderr, "child is not running\n");
        return -1;
    }
    if (!args || !*args) {
        fprintf(stderr, "usage: px <addr|reg> <len>\n");
        return -1;
    }
    char *tok1 = strtok(args, " \t");
    char *tok2 = strtok(NULL, " \t");
    if (!tok2) {
        fprintf(stderr, "usage: px <addr|reg> <len>\n");
        return -1;
    }
    uintptr_t addr;
    if (!parse_addr_or_reg(s, tok1, &addr)) {
        fprintf(stderr, "cannot resolve '%s' as address or register\n", tok1);
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long len = strtoul(tok2, &end, 0);
    if (end == tok2 || *end != '\0') {
        fprintf(stderr, "invalid length '%s'\n", tok2);
        return -1;
    }
    if (len == 0) return 0;
    if (len > 65536) len = 65536;   /* cap to avoid runaway dumps */

    uint8_t *buf = malloc(len);
    if (!buf) { perror("malloc"); return -1; }
    ssize_t n = mem_read(s, addr, buf, len);
    if (n <= 0) {
        fprintf(stderr, "cannot read memory at 0x%lx: %s\n",
                (unsigned long)addr, strerror(errno));
        free(buf);
        return -1;
    }

    /* hexdump -C style: 16 bytes per line, ASCII column. */
    for (ssize_t off = 0; off < n; off += 16) {
        printf("0x%016lx:  ", (unsigned long)(addr + off));
        ssize_t row = (n - off < 16) ? (n - off) : 16;
        for (ssize_t i = 0; i < 16; i++) {
            if (i < row) printf("%02x ", buf[off + i]);
            else         printf("   ");
            if (i == 7)  printf(" ");
        }
        printf(" |");
        for (ssize_t i = 0; i < row; i++) {
            uint8_t c = buf[off + i];
            putchar((c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
    free(buf);
    return 0;
}

static int cmd_ps(dyn_session_t *s, char *args)
{
    if (!s->alive) {
        fprintf(stderr, "child is not running\n");
        return -1;
    }
    if (!args || !*args) {
        fprintf(stderr, "usage: ps <addr|reg> <len>\n");
        return -1;
    }
    char *tok1 = strtok(args, " \t");
    char *tok2 = strtok(NULL, " \t");
    if (!tok2) {
        fprintf(stderr, "usage: ps <addr|reg> <len>\n");
        return -1;
    }
    uintptr_t addr;
    if (!parse_addr_or_reg(s, tok1, &addr)) {
        fprintf(stderr, "cannot resolve '%s' as address or register\n", tok1);
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long len = strtoul(tok2, &end, 0);
    if (end == tok2 || *end != '\0') {
        fprintf(stderr, "invalid length '%s'\n", tok2);
        return -1;
    }
    if (len == 0) return 0;
    if (len > 65536) len = 65536;

    uint8_t *buf = malloc(len);
    if (!buf) { perror("malloc"); return -1; }
    ssize_t n = mem_read(s, addr, buf, len);
    if (n <= 0) {
        fprintf(stderr, "cannot read memory at 0x%lx: %s\n",
                (unsigned long)addr, strerror(errno));
        free(buf);
        return -1;
    }
    printf("0x%016lx: \"", (unsigned long)addr);
    for (ssize_t i = 0; i < n; i++) {
        uint8_t c = buf[i];
        if (c == '"')      printf("\\\"");
        else if (c == '\\') printf("\\\\");
        else if (c == '\n') printf("\\n");
        else if (c == '\t') printf("\\t");
        else if (c == '\r') printf("\\r");
        else if (c >= 32 && c < 127) putchar(c);
        else printf("\\x%02x", c);
    }
    printf("\"\n");
    free(buf);
    return 0;
}

static int cmd_bt(dyn_session_t *s, char *args)
{
    (void)args;
    if (!s->alive) {
        fprintf(stderr, "child is not running\n");
        return -1;
    }
    if (refresh_regs(s) < 0) return -1;

    uintptr_t pc  = (uintptr_t)s->regs.rip;
    uintptr_t rbp = (uintptr_t)s->regs.rbp;

    /* Frame 0: current PC. */
    int level = 0;
    while (level < MAX_FRAMES) {
        uint64_t off = 0;
        const char *name = sym_for_addr(s, pc, &off);
        printf("#%-2d 0x%016lx", level, (unsigned long)pc);
        if (name) printf(" <%s+0x%llx>", name, (unsigned long long)off);
        printf("\n");

        /* Read saved rbp and return address from the stack. */
        uint64_t next_rbp = 0, ret_addr = 0;
        ssize_t n1 = mem_read(s, rbp,     &next_rbp, 8);
        ssize_t n2 = mem_read(s, rbp + 8, &ret_addr, 8);
        if (n1 < 8 || n2 < 8) break;
        if (next_rbp == 0 || ret_addr == 0) break;
        /* Sanity check: stack grows down, so each frame's rbp is
         * strictly higher than the previous. */
        if ((uintptr_t)next_rbp <= rbp) break;
        pc  = (uintptr_t)ret_addr;
        rbp = (uintptr_t)next_rbp;
        level++;
    }
    if (level == 0) {
        printf("  (no frames walked; rbp = 0x%lx may be invalid - "
               "frameless function?)\n", (unsigned long)rbp);
    }
    return 0;
}

static int cmd_sym(dyn_session_t *s, char *args)
{
    if (!args || !*args) {
        fprintf(stderr, "usage: sym <addr>\n");
        return -1;
    }
    char *tok = strtok(args, " \t");
    uintptr_t addr;
    if (!parse_addr_or_reg(s, tok, &addr)) {
        fprintf(stderr, "cannot parse '%s' as address\n", tok);
        return -1;
    }
    uint64_t off = 0;
    const char *name = sym_for_addr(s, addr, &off);
    if (name) {
        printf("0x%016lx: <%s+0x%llx>\n",
               (unsigned long)addr, name, (unsigned long long)off);
    } else {
        printf("0x%016lx: <unknown>\n", (unsigned long)addr);
    }
    return 0;
}

static int cmd_info(dyn_session_t *s, char *args)
{
    (void)args;
    printf("pid:           %d\n", s->pid);
    printf("state:         %s\n",
           s->alive ? "stopped" : "exited");
    if (!s->alive) {
        if (s->signaled_sig)
            printf("exit reason:   killed by signal %d (%s)\n",
                   s->signaled_sig, strsignal(s->signaled_sig));
        else
            printf("exit code:     %d\n", s->exited_code);
    } else {
        if (s->last_sig)
            printf("last signal:   %d (%s)\n",
                   s->last_sig, strsignal(s->last_sig));
        if (s->regs_valid)
            printf("rip:           0x%016llx\n",
                   (unsigned long long)s->regs.rip);
        if (s->pending_bp)
            printf("pending bp:    0x%016lx (will be re-inserted on resume)\n",
                   (unsigned long)s->pending_bp);
    }
    if (s->analysis) {
        printf("binary:        %s (%s, %d-bit)\n",
               s->analysis->binary_path ? s->analysis->binary_path : "?",
               s->analysis->arch_name ? s->analysis->arch_name : "?",
               s->analysis->bits);
        printf("load offset:   0x%lx\n", (unsigned long)s->load_offset);
        printf("symbols:       %zu\n", s->analysis->symbol_count);
    }
    /* Count breakpoints. */
    int bp_count = 0;
    for (int i = 0; i < MAX_BPS; i++)
        if (s->bps[i].used) bp_count++;
    printf("breakpoints:   %d\n", bp_count);
    return 0;
}

/* Print one row of the signal disposition table. */
static void sig_print_row(dyn_session_t *s, int sig)
{
    const char *a = sigabbrev_np(sig);
    if (!a) a = "?";
    printf("SIG%-10s %-5s %-5s %-5s %s\n",
           a,
           s->sig_stop[sig]    ? "Yes" : "No",
           s->sig_print[sig]   ? "Yes" : "No",
           s->sig_program[sig] ? "Yes" : "No",
           strsignal(sig));
}

static int cmd_handle(dyn_session_t *s, char *args)
{
    /* `handle` with no args: print the full disposition table for the
     * standard signals (1..31). Realtime signals (32+) are omitted for
     * brevity; the user can still `handle 32 ...` to set them. */
    if (!args || !*args) {
        printf("Signal        Stop  Print Pass  Description\n");
        for (int sig = 1; sig <= 31 && sig < NSIG; sig++) {
            if (!sigabbrev_np(sig)) continue;
            sig_print_row(s, sig);
        }
        return 0;
    }

    /* `handle <signal> <keywords...>`: set the disposition. */
    char *tok = strtok(args, " \t");
    if (!tok) return -1;
    int sig = sig_from_name(tok);
    if (sig < 0) {
        fprintf(stderr, "unknown signal '%s' (try a name like SIGSEGV "
                "or a number 1..%d)\n", tok, NSIG - 1);
        return -1;
    }

    int set_stop = -1, set_print = -1, set_pass = -1;
    char *kw;
    while ((kw = strtok(NULL, " \t")) != NULL) {
        if      (strcasecmp(kw, "stop")    == 0) set_stop  = 1;
        else if (strcasecmp(kw, "nostop")  == 0) set_stop  = 0;
        else if (strcasecmp(kw, "print")   == 0) set_print = 1;
        else if (strcasecmp(kw, "noprint") == 0) set_print = 0;
        else if (strcasecmp(kw, "pass")    == 0) set_pass  = 1;
        else if (strcasecmp(kw, "nopass")  == 0) set_pass  = 0;
        else {
            fprintf(stderr, "unknown keyword '%s' (expected "
                    "stop/nostop/print/noprint/pass/nopass)\n", kw);
            return -1;
        }
    }
    if (set_stop  >= 0) s->sig_stop[sig]    = (unsigned char)set_stop;
    if (set_print >= 0) s->sig_print[sig]   = (unsigned char)set_print;
    if (set_pass  >= 0) s->sig_program[sig] = (unsigned char)set_pass;

    /* Echo the updated row. */
    printf("Signal        Stop  Print Pass  Description\n");
    sig_print_row(s, sig);
    return 0;
}

static int cmd_help(dyn_session_t *s, char *args)
{
    (void)s; (void)args;
    printf("Commands:\n");
    printf("  db <addr>       set breakpoint at address\n");
    printf("  db <sym>        set breakpoint at symbol name\n");
    printf("  db              list all breakpoints\n");
    printf("  db- <addr>      remove breakpoint\n");
    printf("  dc              continue execution\n");
    printf("  ds              single step (one instruction)\n");
    printf("  dso             step over (past a call instruction)\n");
    printf("  dr              print all GP registers + rip + eflags\n");
    printf("  dr <reg>        print single register\n");
    printf("  dr <reg>=<val>  set register (hex)\n");
    printf("  px <a> <len>    hex dump of memory at address or register\n");
    printf("  ps <a> <len>    string at memory\n");
    printf("  bt              backtrace (rbp chain, max %d frames)\n",
           MAX_FRAMES);
    printf("  sym <addr>      find nearest symbol at or before address\n");
    printf("  info            process status\n");
    printf("  handle          list signal dispositions (stop/print/pass)\n");
    printf("  handle <sig> <stop|nostop> <print|noprint> <pass|nopass>\n");
    printf("                  set signal disposition (e.g. handle SIGINT nostop noprint pass)\n");
    printf("  help | ?        this help\n");
    printf("  q | quit | exit kill child and quit\n");
    printf("\nNumbers are hex by default (gdb convention).\n");
    printf("Empty line repeats the last command.\n");
    return 0;
}

/* ── Command dispatch ────────────────────────────────────────────── */

static int dispatch(dyn_session_t *s, char *line)
{
    /* Skip leading whitespace. */
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0' || *line == '#') return 0;

    /* Make a working copy so strtok can mutate. */
    char buf[LINE_MAX_LEN];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Split off the command word. */
    char *sp = buf;
    while (*sp && !isspace((unsigned char)*sp)) sp++;
    char *args = NULL;
    if (*sp) { *sp = '\0'; args = sp + 1; }

    const char *cmd = buf;

    if      (strcmp(cmd, "db")  == 0) return cmd_db(s, args);
    else if (strcmp(cmd, "db-") == 0) return cmd_db_remove(s, args);
    else if (strcmp(cmd, "dc")  == 0) return cmd_dc(s, args);
    else if (strcmp(cmd, "ds")  == 0) return cmd_ds(s, args);
    else if (strcmp(cmd, "dso") == 0) return cmd_dso(s, args);
    else if (strcmp(cmd, "dr")  == 0) return cmd_dr(s, args);
    else if (strcmp(cmd, "px")  == 0) return cmd_px(s, args);
    else if (strcmp(cmd, "ps")  == 0) return cmd_ps(s, args);
    else if (strcmp(cmd, "bt")  == 0) return cmd_bt(s, args);
    else if (strcmp(cmd, "sym") == 0) return cmd_sym(s, args);
    else if (strcmp(cmd, "info")== 0) return cmd_info(s, args);
    else if (strcmp(cmd, "handle")== 0) return cmd_handle(s, args);
    else if (strcmp(cmd, "help")== 0) return cmd_help(s, args);
    else if (strcmp(cmd, "?")   == 0) return cmd_help(s, args);
    else if (strcmp(cmd, "q")   == 0) return 1;  /* signal: quit */
    else if (strcmp(cmd, "quit")== 0) return 1;
    else if (strcmp(cmd, "exit")== 0) return 1;

    fprintf(stderr, "unknown command '%s' (try 'help')\n", cmd);
    return 0;
}

/* ── Session lifecycle ───────────────────────────────────────────── */

/* Read /proc/PID/maps and find the load base of the binary at
 * `binary_path`. Returns the base address (the start of the first
 * executable mapping matching the path), or 0 if not found. */
static uintptr_t find_load_base(pid_t pid, const char *binary_path)
{
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *f = fopen(maps_path, "r");
    if (!f) return 0;

    /* Resolve the binary path to an absolute path, since /proc/PID/maps
     * always shows absolute paths. */
    char abs[PATH_MAX];
    const char *needle = binary_path;
    if (realpath(binary_path, abs)) needle = abs;

    uintptr_t base = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Lines look like:
         *   555555554000-555555556000 r--p 00000000 08:01 1234 /path/to/binary
         * We want the first line whose path field matches `needle`. */
        char *path_field = strchr(line, '/');
        if (!path_field) continue;
        /* Strip trailing newline. */
        size_t L = strlen(path_field);
        while (L > 0 && (path_field[L-1] == '\n' || path_field[L-1] == ' '))
            path_field[--L] = '\0';
        if (strcmp(path_field, needle) != 0) continue;

        /* Parse the start address. */
        unsigned long long start = 0;
        if (sscanf(line, "%llx-", &start) == 1) {
            base = (uintptr_t)start;
            break;
        }
    }
    fclose(f);
    return base;
}

static int session_init(dyn_session_t *s, int argc, char **argv)
{
    if (argc < 1 || !argv[0]) {
        fprintf(stderr, "209 debag --dynamic-db: no binary specified\n");
        return -1;
    }

    memset(s, 0, sizeof(*s));
    s->mem_fd = -1;
    s->alive  = 1;
    sig_init_defaults(s);

    /* Run static analysis on the binary (for symbol resolution). */
    s->analysis = debag_analyze(argv[0]);
    if (!s->analysis) {
        fprintf(stderr, "warning: cannot analyze '%s' "
                "(symbol resolution will be limited)\n", argv[0]);
    } else if (s->analysis->bits != 64 ||
               !s->analysis->arch_name ||
               strcmp(s->analysis->arch_name, "x86-64") != 0) {
        fprintf(stderr, "warning: target is %s %d-bit; dynamic-db is "
                "x86-64-only, behavior may be incorrect\n",
                s->analysis->arch_name ? s->analysis->arch_name : "?",
                s->analysis->bits);
    }

    /* Fork + exec the child under ptrace. */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* Child. */
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
            perror("ptrace TRACEME");
            _exit(127);
        }
        /* Disable ASLR so addresses are reproducible (and so symbol
         * vaddrs + load_offset match between runs). */
        personality(ADDR_NO_RANDOMIZE);
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }

    /* Parent. Wait for the child to stop at execve. */
    s->pid = pid;
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "child did not stop after exec\n");
        s->alive = 0;
        return -1;
    }

    /* Set ptrace options: TRACESYSGOOD so syscall traps (if any) get
     * sig | 0x80, and EXITKILL so the child dies if we do. */
    long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL;
    if (ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)opts) < 0) {
        /* EXITKILL may be unsupported on older kernels; try without. */
        if (ptrace(PTRACE_SETOPTIONS, pid, NULL,
                   (void *)PTRACE_O_TRACESYSGOOD) < 0) {
            perror("ptrace SETOPTIONS");
            /* Continue anyway — TRACESYSGOOD is nice-to-have. */
        }
    }

    /* Read rip at the entry point to compute the PIE load offset.
     * Note: after execve under ptrace, the child stops at the dynamic
     * linker's _start (in ld-linux.so), NOT the binary's _start. So we
     * cannot compute load_offset as (rip - e_entry); that would give a
     * bogus value. Instead:
     *  - ET_EXEC (non-PIE): symbols have absolute addresses, no offset.
     *  - ET_DYN  (PIE or shared): parse /proc/PID/maps to find the
     *    binary's actual load base. */
    if (ptrace(PTRACE_GETREGS, pid, NULL, &s->regs) == 0) {
        s->regs_valid = 1;
    }
    if (s->analysis) {
        if (s->analysis->binary_type &&
            strcmp(s->analysis->binary_type, "EXEC") == 0) {
            s->load_offset = 0;
        } else {
            /* ET_DYN: read the binary's load base from /proc/PID/maps. */
            uintptr_t base = find_load_base(pid, argv[0]);
            if (base) {
                /* The binary's PT_LOAD segments start at file offset 0,
                 * with p_vaddr = base_addr_in_file (often 0 for PIE).
                 * The kernel loads them at `base` in memory. So the
                 * runtime address of a symbol is sym.vaddr + base.
                 * load_offset = base. */
                s->load_offset = base;
            }
        }
    }

    /* Open /proc/PID/mem for fast memory reads. */
    char mempath[64];
    snprintf(mempath, sizeof(mempath), "/proc/%d/mem", pid);
    s->mem_fd = open(mempath, O_RDWR);
    if (s->mem_fd < 0) {
        /* Try read-only. */
        s->mem_fd = open(mempath, O_RDONLY);
        if (s->mem_fd < 0) {
            /* Will fall back to PTRACE_PEEKDATA. */
            s->mem_fd = -1;
        }
    }

    return 0;
}

static void session_destroy(dyn_session_t *s)
{
    if (s->alive && s->pid > 0) {
        /* Kill the child if it's still alive. */
        kill(s->pid, SIGKILL);
        int status;
        /* Reap to avoid zombie. */
        while (waitpid(s->pid, &status, 0) < 0 && errno == EINTR) {}
        s->alive = 0;
    }
    if (s->mem_fd >= 0) {
        close(s->mem_fd);
        s->mem_fd = -1;
    }
    if (s->analysis) {
        debag_analysis_free(s->analysis);
        s->analysis = NULL;
    }
}

/* ── Public entry ────────────────────────────────────────────────── */

int debag_dynamic_db_repl(int argc, char **argv)
{
    dyn_session_t s;
    if (session_init(&s, argc, argv) < 0) {
        session_destroy(&s);
        return 1;
    }

    printf("209 dynamic-db: target pid %d, stopped at entry rip 0x%016llx\n",
           s.pid, (unsigned long long)s.regs.rip);
    if (s.load_offset)
        printf("  (PIE binary, load offset 0x%lx)\n",
               (unsigned long)s.load_offset);
    printf("  type 'help' for commands, 'dc' to continue, 'q' to quit.\n");

    char line[LINE_MAX_LEN];
    s.last_cmd[0] = '\0';

    while (s.alive) {
        printf("(209-db) ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;  /* EOF */
        }
        /* Strip trailing newline. */
        size_t L = strlen(line);
        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r'))
            line[--L] = '\0';

        /* Empty line repeats the last command (gdb convention). */
        char *cmdline = line;
        while (*cmdline && isspace((unsigned char)*cmdline)) cmdline++;
        if (*cmdline == '\0') {
            if (s.last_cmd[0] == '\0') continue;
            strncpy(line, s.last_cmd, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
        } else {
            strncpy(s.last_cmd, line, sizeof(s.last_cmd) - 1);
            s.last_cmd[sizeof(s.last_cmd) - 1] = '\0';
        }

        int rc = dispatch(&s, line);
        if (rc == 1) break;  /* quit requested */
    }

    /* If child is still alive, kill it. */
    int exit_code = 0;
    if (s.alive) {
        /* User quit explicitly. */
    } else if (s.signaled_sig) {
        exit_code = 128 + s.signaled_sig;
    } else {
        exit_code = s.exited_code;
    }

    session_destroy(&s);
    return exit_code == 0 ? 0 : 1;
}
