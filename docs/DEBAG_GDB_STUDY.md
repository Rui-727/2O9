# Debag GDB Study

Recommendations for what 2O9's `src/debag/dynamic_db.c` should port from
gdb (binutils-gdb `gdb/` tree, commit `1dcfefd` as cloned). All line
numbers below are to that snapshot unless prefixed `2O9:` (which refers
to `src/debag/dynamic_db.c` in this repo).

## What 2O9's --dynamic-db has today

`src/debag/dynamic_db.c` is a 1460-line single-file live debugger REPL.
It forks the target with `PTRACE_TRACEME`, disables ASLR via
`personality(ADDR_NO_RANDOMIZE)`, stops at the exec trap, computes the
PIE load offset by parsing `/proc/PID/maps`, and opens `/proc/PID/mem`
for memory access (falling back to `PTRACE_PEEKDATA` per word). It
implements software breakpoints via the INT3 (0xCC) swap with a
fixed-size table of 64 slots (`dyn_bp_t` at 2O9:69-77), supports
temporary breakpoints used by `dso`, walks the `rbp` chain for `bt`
(`cmd_bt` at 2O9:1049), and uses a 50-line hand-rolled x86-64
instruction-length decoder for `dso` (with an optional libcapstone path)
at 2O9:550-624. The command set is rizin-style: `db`, `db-`, `dc`, `ds`,
`dso`, `dr`, `px`, `ps`, `bt`, `sym`, `info`, `quit`. Single-threaded
only; does not set `PTRACE_O_TRACEFORK/CLONE/EXEC`. Signals are always
swallowed on `dc` (`PTRACE_CONT` with sig=0 at 2O9:707), which is a
correctness bug for any program that handles signals.

## What gdb does better, with file:line citations

### 1. Breakpoint management

gdb separates the user-facing concept of a breakpoint
(`gdb/breakpoint.h:626` `struct breakpoint`) from the target-side
mechanism (`gdb/breakpoint.h:330` `class bp_location`, and
`gdb/breakpoint.h:262` `struct bp_target_info`). The high-level struct
tracks, at `gdb/breakpoint.h:810-918`:

- `bptype type` (line 810) — one of 30+ kinds from `enum bptype` at
  `gdb/breakpoint.h:89-218` (bp_breakpoint, bp_hardware_breakpoint,
  bp_watchpoint, bp_hardware_watchpoint, bp_read_watchpoint,
  bp_access_watchpoint, bp_longjmp, bp_step_resume, bp_shlib_event,
  bp_thread_event, bp_catchpoint, bp_tracepoint, bp_dprintf, etc.)
- `enable_state enable_state` (line 812) — bp_disabled / bp_enabled /
  bp_call_disabled
- `bpdisp disposition` (line 814, enum at line 241-248) — disp_del,
  disp_del_at_next_stop, disp_disable, disp_donttouch
- `int number` (line 816) — user-visible breakpoint number
- `bool silent` (line 820)
- `int ignore_count` (line 825) — auto-continue N times before stopping
- `int enable_count` (line 829)
- `counted_command_line commands` (line 833) — gdb command list to run
  on hit
- `frame_id frame_id` (line 836) — break only at this frame
- `gdb::unique_xmalloc_ptr<char> cond_string` (line 874) — condition
  expression source
- `int thread` (line 888), `int inferior` (line 892), `int task` (line
  896) — thread/task scoping
- `int hit_count` (line 902) — how many times this bp was hit

2O9's `dyn_bp_t` at `2O9:69-77` has only: `used`, `enabled`,
`inserted`, `temporary`, `addr`, `shadow` (one byte), `spec[128]`.
Missing everything except `enabled`/`addr`/`shadow`.

The split between breakpoint and bp_location matters because gdb lets
one logical breakpoint have many resolved locations (e.g. `break foo`
when `foo` is inlined into five call sites). 2O9 has no inline support
and conflates the two — that's fine for a first cut.

**Recommendation.** Add to `dyn_bp_t`: `int number`, `int hit_count`,
`int ignore_count`, `char cond[256]`. Wire `hit_count` into
`handle_sigtrap` (2O9:448): increment on every hit, print `hit
breakpoint N at 0x... (count=3)`. Implement `ignore_count`: decrement on
hit, auto-continue when >0. Implement `cond` as a tiny expression
evaluator over registers (`rax==0`, `rdx!=0`): parse the comparison,
fetch the register, compare. ~60 lines for the evaluator. New commands:
`db <addr> if <expr>`, `db <addr> ignore <N>`, `db <addr> count` (print
count). Do NOT port gdb's full expression language — that lives in
`gdb/eval.c` (~5000 lines) and depends on `gdb/value.c` (~5000 lines).

### 2. Continue / step semantics

`gdb/infrun.c` (11009 lines) is the heart of execution control. The
relevant pieces:

- gdb's `next` over a call (`gdb/infrun.c:8013-8048`) sets a
  step-resume breakpoint at the **caller's return address** and
  continues, rather than single-stepping through the callee. 2O9's
  `dso` (`2O9:747-841`) does the same thing at instruction granularity:
  it decodes whether the current instruction is a call, sets a temp bp
  at `rip + insn_len` (the return address), and continues. **This is
  correct for instruction-level `nexti`** — what gdb calls `nexti`.
  gdb's `next` is line-oriented and needs source line info.

- gdb tracks `step_over_info` (`gdb/infrun.c:1521`) so that other
  threads don't trip the breakpoint being stepped over. 2O9 is
  single-threaded; this doesn't matter yet.

- gdb installs **longjmp master breakpoints** (`gdb/breakpoint.h:180`
  `bp_longjmp_master`) at every longjmp target. When a stepped-over
  function does `longjmp` out, gdb's step-resume bp is bypassed — but
  the longjmp bp fires, gdb notices the unwind, and re-evaluates. 2O9's
  `dso` over a function that longjmps would lose the temp bp entirely
  (it would just stay set at the original return address, never hit).

- gdb keeps the step-resume breakpoint installed across signal stops.
  2O9's `dso` removes the temp bp if `dc` stops anywhere other than
  `next_pc` (`2O9:831-838`). That means: if `dso` is interrupted by
  SIGALRM, the temp bp is gone and the user cannot `dc` to land at the
  step-over target. Bug.

- gdb's signal disposition (next section) decides whether `dc` forwards
  the stop signal. 2O9 always passes 0 as the signal arg to
  `PTRACE_CONT` (`2O9:707`), swallowing the signal. For SIGALRM-based
  timers, SIGCHLD handlers, or any program that needs the signal
  delivered, this **breaks the program**.

**Recommendation.** Three changes:

1. **Forward `s->last_sig` on `dc`** unless the user has changed the
   disposition (see #7). One-line fix at `2O9:707`: replace `NULL` with
   `(void *)(intptr_t)s->last_sig`. Critical correctness bug.

2. **Don't delete the `dso` temp bp on unrelated stops.** Change
   `2O9:831-838` to only clean up the temp bp if the stop was at
   `next_pc`. If the stop was elsewhere, leave the temp bp in place;
   the next `dc` will hit it. ~10 lines.

3. **Track `dso` intent.** Add `int in_step_over; uintptr_t step_over_target;`
   to `dyn_session_t`. On `dc`, if `in_step_over` is set and the next
   stop matches `step_over_target`, clear it silently (don't print
   "hit breakpoint"). This mimics gdb's silent step-resume bp.

Skip: gdb's displaced stepping (`gdb/infrun.c:1591+`), which is only
needed for non-stop multi-threaded mode. Skip the longjmp master
breakpoint machinery — it's the right answer but requires parsing
libpthread internals, not worth it for v1.

### 3. Memory access

`gdb/linux-nat.c:179-233` documents gdb's strategy in a long comment:
strongly prefer `/proc/PID/mem`, fall back to ptrace only if
`/proc/PID/mem` is not writable. gdb opens the fd once at attach and
holds it (`gdb/linux-nat.c:226-233`) **specifically to avoid the
post-exec race** where re-opening would read the new address space.
gdb re-opens `/proc/PID/mem` after `PTRACE_EVENT_EXEC`
(`gdb/linux-nat.c:2117` comment, `:3995-4082`) because the old fd
returns EOF (zero bytes) on the post-exec address space.

2O9 already matches gdb's fast-path design: opens `/proc/PID/mem` once
in `session_init` (`2O9:1366`), holds the fd, falls back to PEEKDATA
per-call (`2O9:272-284`). For writes, 2O9 uses `PTRACE_POKETEXT`
directly (`2O9:290-300`) instead of `/proc/PID/mem` writes —
equivalent for 1-byte bp swaps.

**Recommendation.** No code change for v1; the current design is
correct. The one gap (re-open `mem_fd` after `PTRACE_EVENT_EXEC`) only
matters once `PTRACE_O_TRACEEXEC` is added (#9).

### 4. Register access

gdb's regcache (`gdb/regcache.c:44` `struct regcache_descr`,
`gdb/regcache.c:211` ctor) is per-arch, per-thread, with per-register
status (`gdb/regcache.c:599` `REG_UNKNOWN`, `REG_VALID`,
`REG_UNAVAILABLE`). The status field matters for targets where some
registers are inaccessible (e.g. AVX-512 on a kernel that doesn't
expose XSTATE). 2O9's single `regs_valid` flag
(`2O9:98`) is fine for x86-64 with `PTRACE_GETREGS`.

2O9's register table (`2O9:175-200`) covers rax-r15, rip, eflags,
orig_rax, cs, ss, fs_base, gs_base. Compared to gdb's
`amd64_linux_gregset32_reg_offset` table at
`gdb/amd64-linux-nat.c:72-97`, 2O9 is missing: `ds`, `es`, `fs`, `gs`
(segment registers without `_base`; mostly zero in long mode, low
value). 2O9 is also missing DR0-DR7 (debug registers — needed for #8),
and all FP/SSE/AVX state.

gdb fetches FP/SSE/AVX via `PTRACE_GETREGSET` with `NT_X86_XSTATE`
(`gdb/amd64-linux-nat.c:54-55` overrides `fetch_registers`). 2O9 uses
`PTRACE_GETREGS` which only returns the GP register set. For most
debugging this is fine; if a user wants to inspect xmm0, they're out
of luck.

**Recommendation.** No urgent change. Optionally add `dr0`-`dr7` to
the register table for hardware breakpoint support (see #8). Skip
FP/SSE/AVX — the use cases that need it (numerical code, SIMD) are
rare in a system-package-debugging context, and the XSTATE format is
unpleasant. Skip gdb's per-register-status model — overkill for one
thread on one arch.

### 5. Backtrace

gdb's frame unwinder (`gdb/frame.c`) is a chain of "sniffers" tried in
priority order. For amd64 (`gdb/amd64-tdep.c:3620-3622`):
1. `amd64_sigtramp_frame_unwind` (signal trampoline frames)
2. `dwarf2_frame_unwind` (`gdb/dwarf2/frame.c:1298`) — DWARF CFI from
   `.debug_frame` or `.eh_frame`
3. `amd64_frame_unwind` (`gdb/amd64-tdep.c:3021`) — prologue analyzer
4. `amd64_epilogue_frame_unwind` and `amd64_epilogue_override_frame_unwind`
   for functions in their epilogue

The prologue analyzer `amd64_analyze_prologue` (`gdb/amd64-tdep.c:2724`)
decodes `endbr64; sub $X,%rsp; push %rbp; mov %rsp,%rbp; sub $Y,%rsp`
and the register-save area, recording where each callee-saved register
was spilled. This lets gdb unwind even `-fomit-frame-pointer` functions
IF they have a standard prologue. The DWARF CFI unwinder
(`gdb/dwarf2/frame.c:1298`, ~2200 lines) handles everything else,
including hand-written asm, signal frames, and stack-switching
coroutines. It reads `.eh_frame` (present in nearly all modern ELF
binaries because C++ exceptions need it).

2O9's `cmd_bt` (`2O9:1049-1088`) walks the rbp chain with the
"next_rbp > rbp" sanity check. This breaks on:
- `-fomit-frame-pointer` functions (the default at `-O2` on modern
  compilers, including the Linux kernel)
- Functions in their prologue (rbp not yet pushed)
- Hand-written asm without a frame chain
- Signal frames (the kernel pushes a `rt_sigframe`, breaking the rbp
  chain)

2O9 detects this gracefully (`2O9:1083-1086`: "frameless function?")
but produces no useful backtrace in those cases.

**Recommendation.** Two-tier approach:

1. **Cheap heuristic unwinder (~30 lines).** Scan the stack from `rsp`
   upward, reading 8-byte words. For each word, check whether it falls
   in the binary's executable segment range (from
   `s->analysis`'s PT_LOAD headers, which 2O9 already parses). If so,
   treat it as a candidate return address and print it. Cap at 64
   candidates. This is the "stack scan" technique used by frida, rr,
   and Linux kernel oops reports. Imperfect (will print stale stack
   values that happen to look like code addresses) but far better than
   nothing on stripped binaries. Add as `bt scan` command.

2. **Prologue-aware unwinder (~150 lines).** When the function at PC
   has a recognizable prologue (`55 48 89 e5` = `push %rbp; mov
   %rsp,%rbp`), use rbp-chain. When it doesn't, scan the function's
   first 64 bytes for `sub $X,%rsp` to find the frame size, then read
   the return address from `[rsp + frame_size]`. This is what
   `amd64_analyze_prologue` does at `gdb/amd64-tdep.c:2724-2752`.

Skip `.eh_frame` parsing for v1 — `gdb/dwarf2/frame.c` is 2224 lines
and depends on `gdb/dwarf2/read.c` (18558 lines). A minimal clean-room
EH_FRAME reader that handles only the `DW_CFA_def_cfa_offset`,
`DW_CFA_def_cfa_register`, `DW_CFA_offset` opcodes would be ~500 lines
and is worth doing eventually, but not in the first port.

### 6. Symbol resolution

gdb reads `.symtab` (full symbols) and `.dynsym` via
`elf_symtab_read` (`gdb/elfread.c:252`), with separate handling for
STT_FUNC, STT_OBJECT, STT_GNU_IFUNC, STT_TLS, mappable minimal symbols
at `gdb/elfread.c:345-653`. 2O9 already does this in
`debag_analyze`.

What 2O9 doesn't do:

- **C++ demangling.** gdb uses `libiberty/cp-demangle.c` (vendored,
  ~6000 lines, or `__cxa_demangle` from libstdc++). The demangled name
  is stored in `minimal_symbol` and shown in backtraces, breakpoint
  listings, and `info functions`. 2O9's `sym main` shows `_Z4mainv`
  for a C++ binary, not `main()`. This is high-leverage for any C++
  target (libalpm itself is C, but plugins or LD_PRELOAD'd helpers may
  not be).

- **`.debug_line` parsing.** gdb's `find_sal_for_pc`
  (`gdb/symtab.c:3164`) returns source file:line for an address. 2O9's
  `sym_for_addr` (`2O9:119`) returns just `name + offset`. A
  `bt` frame in 2O9 looks like `#0 0x401234 <foo+0x14>`; in gdb it
  looks like `#0 0x401234 in foo (argc=2) at src/foo.c:42`. The
  difference is enormous for actual debugging.

- **Inline frames.** gdb shows inline function calls as separate
  frames in `bt` (e.g. `#0 std::vector::push_back` then `#1
  std::sort`). 2O9 has no concept of inlining.

**Recommendation.** Do (a) first.

- **C++ demangling.** Use `__cxa_demangle` from `libstdc++_exp` (link
  with `-lstdc++_exp`) OR vendor `libiberty/cp-demangle.c` (it's
  GPL-3.0+libiberty exception, which is **incompatible** with 2O9's
  GPL-2.0-only). Safest path: dynamically `dlopen("libstdc++.so.6")`
  and look up `__cxa_demangle`. If absent, fall back to printing the
  mangled name. ~20 lines. Add to `sym_for_addr` and to `bt` output.

- **`.debug_line` parsing.** ~500 lines for a minimal DWARF line
  number program reader. Worth doing eventually but skip for v1.

- **Inline frames.** Skip; requires `.debug_info` parsing.

### 7. Signal handling

gdb's signal disposition is three tables of `unsigned char` indexed by
signal number (`gdb/infrun.c:332-334`):

```c
static unsigned char signal_stop[GDB_SIGNAL_LAST];    // should we stop?
static unsigned char signal_print[GDB_SIGNAL_LAST];   // should we print?
static unsigned char signal_program[GDB_SIGNAL_LAST]; // should we pass to program?
```

Plus a computed `signal_pass[]` (`gdb/infrun.c:344`) which is true
iff `!stop && !print && program && !catch`. The `handle` command
updates these via `signal_stop_update` / `signal_print_update` /
`signal_pass_update` (`gdb/infrun.c:9918-9945`). Defaults: SIGINT,
SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP stop+print+pass; SIGALRM,
SIGCHLD, SIGURG, SIGIO pass through without stopping.

The decision logic is in `handle_signal_stop` at `gdb/infrun.c:7356-7395`:
if the signal is in `signal_stop`, stop and wait for user input.
Otherwise, if `signal_print`, print and continue. If
`signal_program`, deliver on next resume; else swallow.

2O9 has none of this. `handle_stop` (`2O9:508-543`) always stops on
any signal. `cmd_dc` (`2O9:707`) always passes 0 (swallow) to
`PTRACE_CONT`.

**This is the single biggest correctness bug in 2O9's dynamic-db.**
For any program that uses signals for legitimate purposes — timers
(SIGALRM), child reaping (SIGCHLD), I/O (SIGIO), user-defined
(SIGUSR1/2) — `dc` after a signal stop silently eats the signal,
breaking the program. Even SIGSEGV: the user stops at the fault, types
`dc` expecting the default handler (death), but the signal is
swallowed, the instruction re-executes, faults again, infinite loop.

**Recommendation.** Port the 3-table model. ~80 lines.

- Add to `dyn_session_t`: `uint8_t sig_stop[NSIG]; uint8_t sig_print[NSIG]; uint8_t sig_pass[NSIG];`
- Initialize in `session_init`: stop+print+pass for SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGTRAP/SIGINT; pass-only for everything else.
- In `handle_stop` (`2O9:508`): if `!sig_stop[sig]`, print if `sig_print[sig]`, then auto-continue with `sig_pass[sig] ? sig : 0`. Don't return to the REPL.
- In `cmd_dc` (`2O9:707`): pass `sig_pass[s->last_sig] ? s->last_sig : 0` to `PTRACE_CONT`. Clear `s->last_sig` after.
- New command: `handle <signal> stop|nostop print|noprint pass|nopass`. ~20 lines.

### 8. Hardware breakpoints / watchpoints

gdb uses x86 debug registers DR0-DR3 (addresses), DR6 (status), DR7
(control). Each of DR0-DR3 can be a hardware execution breakpoint or a
data watchpoint (write or read/write). Four slots total, each watching
1/2/4/8 bytes. The implementation is in `gdb/nat/x86-dregs.c` (732
lines) and `gdb/nat/x86-linux-dregs.c` (175 lines). The Linux backend
pokes debug registers via `PTRACE_POKEUSER` at
`offsetof(struct user, u_debugreg[i])` (`gdb/nat/x86-linux-dregs.c:33`).

The state struct `x86_debug_reg_state` (`gdb/nat/x86-dregs.h:78-90`):
```c
struct x86_debug_reg_state {
    CORE_ADDR dr_mirror[DR_NADDR];     // 4 slot addresses
    unsigned dr_status_mirror, dr_control_mirror;
    int dr_ref_count[DR_NADDR];        // sharing by refcount
};
```

`x86_dr_insert_watchpoint` (`gdb/nat/x86-dregs.c:505`) finds a free
slot, sets the address, sets DR7 bits (length + read/write/execute),
and writes via `x86_dr_low.set_addr` / `x86_dr_low.set_control`. On
SIGTRAP, `x86_dr_stopped_by_watchpoint` (`gdb/nat/x86-dregs.c:682`)
checks DR6 to see if it was a debug-register trap (vs an INT3) and
returns the watched address via `x86_dr_stopped_data_address`.

**Why this matters.** Hardware watchpoints (`watch *0x1234`) stop the
program on the exact instruction that writes a watched memory
location. This is the single most powerful feature gdb has for
debugging memory corruption, use-after-free, and race conditions.
Software equivalents (single-step and check) are 1000x slower and
unusable in practice.

**License issue.** gdb's `nat/x86-dregs.c` is GPL-3.0. 2O9 is
GPL-2.0-only. **Cannot copy verbatim.** The `nat/` subdir is
specifically structured for reuse (gdbserver uses it), but the license
is incompatible.

**Recommendation.** Reimplement clean-room. The algorithm is
well-documented in the Intel SDM Vol 3, §17.2. ~200 lines of C:

- `static unsigned long dr_mirror[4]; static unsigned long dr7_mirror;`
- `hw_bp_insert(addr, len, type)`: find free slot, set `dr_mirror[i]`,
  set DR7 bits `(len<<2 | type) << (i*2)` for the L bits, `1 << (i*2)`
  for the LE bit, `1 << i` for the L bit.
- On `dc`, write DR0-DR3 and DR7 via `PTRACE_POKEUSER` at
  `offsetof(struct user, u_debugreg[i])`.
- In `handle_sigtrap` (`2O9:448`), check DR6 via `PTRACE_PEEKUSER` at
  `offsetof(struct user, u_debugreg[6])`. If bits 0-3 are set, it's a
  hardware trap; report the watched address.
- New commands: `hwb <addr>` (hardware breakpoint), `ww <addr> <len>`
  (write watchpoint), `rw <addr> <len>` (read watchpoint), `aw <addr>
  <len>` (access watchpoint).

HIGH leverage, MEDIUM effort. Top priority after #7 (signal handling).

### 9. Thread support

gdb's `linux_nat_ptrace_options` (`gdb/linux-nat.c:429-433`) sets:
```c
options |= (PTRACE_O_TRACESYSGOOD
            | PTRACE_O_TRACEVFORKDONE
            | PTRACE_O_TRACEVFORK
            | PTRACE_O_TRACEFORK
            | PTRACE_O_TRACEEXEC);
```
(Plus `PTRACE_O_TRACECLONE` is added elsewhere for thread tracking.)
On any of these events, the new child is auto-attached (stopped with
`PTRACE_EVENT_FORK` etc. encoded in the wait status). gdb then
maintains a `struct thread_info` list (`gdb/thread.c:343`) with
per-thread register caches, step-resume breakpoints, and pending
waitstatuses.

2O9 sets only `PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL`
(`2O9:1324`). If the tracee spawns a thread via `pthread_create`
(which calls `clone(CLONE_THREAD)`), the new thread runs **untraced**
— it hits no breakpoints, runs freely, and can crash the program
while 2O9 is single-stepping the main thread. If the tracee forks,
the child also runs untraced.

**Recommendation.** Two phases:

- **Phase 1 (one line, do now).** Add
  `PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
  PTRACE_O_TRACEEXEC` to the `PTRACE_SETOPTIONS` call at `2O9:1324`.
  This causes new children to be auto-stopped with
  `PTRACE_EVENT_FORK` etc. and to be ptrace-children of 2O9. Even if
  2O9 doesn't yet switch between them, this prevents them from
  running wild.

- **Phase 2 (~150 lines, defer).** Maintain `pid_t threads[MAX_THREADS]`
  in `dyn_session_t`. On `PTRACE_EVENT_FORK/CLONE`, wait for the new
  child, add to the list, stop it. Add commands: `info threads`
  (list), `thread N` (switch — sets the active pid for `dc`/`ds`/`dr`).
  Each thread needs its own register snapshot; `s->regs` becomes
  per-thread.

Skip gdb's per-thread step-resume bp coordination and
scheduler-locking mode — those are for non-stop debugging, well
beyond 2O9's scope.

### 10. Process lifecycle

gdb's `nat/fork-inferior.c:240-360` does, in the child after fork:

- `close_most_fds()` (`gdb/nat/fork-inferior.c:319`) — close all fds
  except stdin/stdout/stderr, so they don't leak to the inferior.
- `chdir(inferior_cwd)` if set (line 325) — 2O9 doesn't support
  `--cwd`; minor.
- `postfork_child_hook()` (line 333) — terminal setup.
- `traceme_fun()` (line 342) — the `PTRACE_TRACEME` call.
- `restore_original_signals_state()` (line 353) — reset all signal
  handlers to SIG_DFL, so the inferior isn't surprised by inherited
  dispositions.
- `execv` (later) — gdb supports both `execv` (no shell) and
  `execv` via `$SHELL -c` (for shell features like globs).

gdb's parent (`gdb/nat/fork-inferior.c:426-516`) handles the
post-exec wait loop, including `TARGET_WAITKIND_EXECD` (line 473) and
the "pending_execs" counter for shell-wrap cases.

2O9's `session_init` (`2O9:1264-1377`):

- `fork()` then in the child: `PTRACE_TRACEME`,
  `personality(ADDR_NO_RANDOMIZE)`, `execvp`. That's it.
- **Missing**: close fds, reset signal handlers.

**Bugs in 2O9:**

1. **fd leak.** If 2O9 is run from a script with many open fds (e.g.
   `209 debag --dynamic-db -- ./binary < input.txt > output.txt 3< some_fd`),
   the child inherits all of them. The child may behave differently
   (e.g. `select` on an unexpected fd). Fix: in the child, after
   `PTRACE_TRACEME`, call `close_range(3, ~0U, 0)` (Linux 5.9+) or
   fall back to `for (int fd = 3; fd < sysconf(_SC_OPEN_MAX); fd++)
   close(fd);`. ~5 lines.

2. **Signal disposition inheritance.** 2O9's REPL probably ignores
   SIGINT (so Ctrl-C in the REPL prints a new prompt instead of
   killing 2O9). The child inherits this. If the child expects
   default SIGINT behavior (death), it instead ignores it. Fix: in
   the child, after `PTRACE_TRACEME`, `signal(SIGINT, SIG_DFL);` for
   all signals 2O9 has overridden. ~5 lines.

3. **No exec event handling.** When the child `execve`s another
   binary (common: `bash` exec'ing `ls`), the new binary's symbols
   are different. 2O9's `s->analysis` still points at the old binary.
   Fix: on `PTRACE_EVENT_EXEC`, re-run `debag_analyze` on the new
   binary (read the path from `/proc/PID/exe`), recompute
   `load_offset`. Requires Phase 1 of #9 first.

Skip: setuid binary handling (gdb warns; the kernel drops privileges
when ptraced — edge case), terminal pty allocation (2O9 is
pipe-oriented), `LD_PRELOAD` injection (gdbserver's domain).

## Recommended port priority

Ranked by leverage / effort (highest first):

1. **Forward signals on `dc`** (#7). One-line fix at `2O9:707`.
   Without this, any signal-using program is un-debuggable. **Do
   today.**

2. **Signal disposition table + `handle` command** (#7). ~80 lines.
   Lets the user say "ignore SIGALRM", "stop on SIGUSR1". Required
   for non-trivial debugging.

3. **Don't delete `dso` temp bp on unrelated stops** (#2). ~10 lines.
   Fixes a real bug: stepping over a call that gets a signal loses
   the step-over target.

4. **Close fds and reset signals in the child** (#10). ~10 lines.
   Prevents surprising child behavior when 2O9 is run from scripts.

5. **C++ symbol demangling** (#6). ~20 lines (dlopen
   `__cxa_demangle`). Huge readability win for C++ targets.

6. **Hardware watchpoints via DR0-DR7** (#8). ~200 lines clean-room.
   The killer feature for memory-corruption debugging. Top
   "new feature" priority.

7. **Stack-scan backtrace fallback** (#5). ~30 lines. Salvages a
   useful backtrace from stripped/frameless binaries.

8. **Breakpoint `hit_count` and `ignore_count`** (#1). ~30 lines.
   Quality-of-life for breakpoint-heavy sessions.

9. **`PTRACE_O_TRACEFORK/CLONE/EXEC`** (#9, phase 1). One line.
   Prevents child threads from running wild.

10. **Breakpoint conditions** (#1). ~60 lines for a tiny
    register-comparison evaluator. Useful for "stop only when
    `rdi == 0`".

## What NOT to port

- **Full expression evaluator** (`gdb/eval.c`, `gdb/value.c`, ~10000
  lines). Conditions should accept `reg OP value` only.
- **`.debug_info` reader** (`gdb/dwarf2/read.c`, 18558 lines). Local
  variables, types, inlined frames all depend on it. 2O9's ELF
  symbol table is enough.
- **`.debug_line` reader**. ~500 lines for a minimal version; defer.
- **`.eh_frame` CFI unwinder** (`gdb/dwarf2/frame.c`, 2224 lines).
  Prologue-aware + stack-scan covers 90% for 10% of the code.
- **Displaced stepping** (`gdb/infrun.c:1591+`). Only needed for
  non-stop multi-threaded mode.
- **`struct target_ops`** (`gdb/target.c`, 4635 lines) and
  **`struct gdbarch`** (`gdb/gdbarch.c`, ~12000 lines generated).
  2O9 is ptrace-on-Linux-x86-64 forever; the abstraction buys
  nothing.
- **Python scripting** (`gdb/python/`), **reverse debugging**
  (`gdb/record-full.c`), **non-stop mode**, **gdbserver/remote
  protocol** (`gdb/remote.c`, ~15000 lines), **tui/CLI command
  parser** (`gdb/cli/`, `gdb/tui/`). All out of scope.
