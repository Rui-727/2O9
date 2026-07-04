# 209(1) - 2O9 package manager

## SYNOPSIS

`209` [options] `<subject>` `<verb>`
`209` [options] `<command>`

## DESCRIPTION

**2O9** is a unified package manager for Arch Linux that puts files in
`/nix/store/`. It combines three things into one tool:

1. **pacman's engine** (libalpm), modified in-tree as part of lib2O9
2. **paru's AUR workflow**, rewritten in C
3. **A real `/nix/store`** with content-addressed paths, atomic
   generations, and a references graph, driven by a declarative
   Nix-syntax configuration (`2O9.nix`)

Plus **Trakker** (ptrace sandbox) and **Debag** (seccomp + ptrace hybrid
sandbox).

The binary is `209` (numeric). The project name is `2O9` (letter O).

## COMMANDS

### Pacman-compatible flags

2O9 supports pacman's common operation flags so muscle memory transfers.
Each maps to the equivalent 2O9 command.

`209 -S` `<pkg>` `[...]`
: Install package(s). Same as `209 <pkg> install`.

`209 -Sy`
: Refresh repo databases. Same as `209 sync`.

`209 -Su`
: Upgrade all packages. Evaluates `2O9.nix`, computes what's outdated
  in the current generation, downloads new versions from configured
  caches or the Arch mirror, extracts, commits a new generation.
  Requires root (writes to `/nix/store/`).

`209 -Ss` `<term>`
: Search repos. Same as `209 <term> search`.

`209 -Si` `<pkg>`
: Show package info. Same as `209 <pkg> info`.

`209 -R` `<pkg>` `[...]`
: Remove package(s). Same as `209 <pkg> remove`.

`209 -Q`
: List all installed packages (name + version, one per line).

`209 -Qs` `<term>`
: Search installed packages by substring.

`209 -Qi` `<pkg>`
: Show info for an installed package.

`209 -Ql` `<pkg>`
: List files in an installed package (from its store path).

`209 -Qm`
: List foreign (AUR) packages, installed packages with `origin: "aur"`.

### 2O9 commands

`209 apply`
: Evaluate `2O9.nix`, reconcile against the current generation, commit
  a new generation if the manifest changed.

`209 generations`
: List all generations. The current one is marked with `← current`.
  Shows a change summary (`+N -N ~N`) per generation from the diff.

`209 sync`
: Download repo databases. Same as `-Sy`. When a `2O9.nix` config
  exists, uses lib2O9 (`alpm_db_update`). Otherwise falls back to
  direct libcurl download of default Arch mirrors.

`209 gc` [`--optimise`]
: Garbage-collect store paths not reachable from any generation's root
  set. Walks the references graph (SQLite) to compute the closure, so
  dependencies of installed packages are preserved even if no
  generation explicitly references them. With `--optimise`, also runs
  hardlink dedup on the surviving store paths.

`209 optimise`
: Walk `/nix/store/`, SHA-256 every regular file, hardlink identical
  files into `/nix/store/.links/<sha256>`. Saves disk when multiple
  packages ship the same file (locale data, license text, etc).

`209 news`
: Fetch and display the latest Arch Linux news from the RSS feed.

`209 init` [`--system`]
: Create a starter `2O9.nix` in `~/.config/2O9/` (or `/etc/2O9/` with
  `--system`). Refuses to overwrite an existing file.

### SOV patterns (Subject-Object-Verb)

`209 <pkg> install`
: Install a package temporarily. It lands in the current generation
  but is not declared in `2O9.nix`. The next `209 apply` will flag it
  for removal unless you add it to the config first.

`209 <pkg> remove`
: Remove a package. Commits a new generation without it and rebuilds
  the symlink farm.

`209 <pkg> info`
: Show info about an installed package. Falls back to AUR info if the
  package isn't installed locally.

`209 <term> search`
: Search installed packages by substring. Falls back to AUR search if
  no local matches.

`209 <pkg> aur build`
: Build a package from the AUR. Resolves deps, clones the PKGBUILD,
  imports PGP keys if `validpgpkeys` is set, runs makepkg inside a
  chroot (on by default, use `--no-chroot` to disable), installs the
  result into `/nix/store/`.

`209 <term> aur search`
: Search the AUR.

`209 <pkg> aur info`
: Show AUR package info.

`209 <pkg> aur review`
: Clone the PKGBUILD and show a diff for review before building.

`209 trakker` [`--no-net`] [`--no-write`] [`--redirect-writes` `<dir>`] [`--allow-net` `port=<port>`] [`--`] `<cmd>` `[args...]`
: Run `<cmd>` inside the Trakker sandbox. The command is resolved via
  `$PATH`, so bare names work (`209 trakker ls -la`). Records file I/O,
  network connections, and process activity as a JSON trace. Use `--`
  to separate trakker flags from the command's own flags.

  Examples:
  - `209 trakker ls -la`
  - `209 trakker --no-net -- curl https://example.com`
  - `209 trakker --no-write --redirect-writes /tmp/trakker -- makepkg -f`

`209 debag` [`--static-scan`] [`--static-db`] [`--no-net`] [`--fast-mode`] [`--`] `<cmd>` `[args...]`
: Run `<cmd>` inside the Debag hybrid sandbox. Uses seccomp-bpf for
  the fast path (most syscalls allowed directly) and ptrace only for
  syscalls that need argument inspection. About 90% native speed. With
  `--static-scan`, scans the ELF symbol table first to build a tighter
  seccomp filter.

`209 debag --static-db --` `<binary>`
: Drop into an interactive, rizin-style read-only REPL for inspecting
  an ELF binary. No sandbox, no exec; just static analysis. Commands
  (longest-prefix match, so `iS` is distinct from `iSS`):

  | Command | Action |
  |---|---|
  | `i` | Print binary info (path, arch, bits, endian, type, entry) |
  | `iS` | List sections (index, name, vaddr, size, type, flags) |
  | `iSS` | List segments / program headers (type, vaddr, offset, filesz, memsz, flags) |
  | `is` | List symbols (name, vaddr, size, type, bind) |
  | `ie` | List entry points (`entry0`) |
  | `iz` | List printable strings (>= 4 chars, ASCII + UTF-16LE) in `.rodata` / `.data` |
  | `iz ascii` | List only ASCII strings in `.rodata` / `.data` |
  | `iz utf16` | List only UTF-16LE strings in `.rodata` / `.data` |
  | `izz` | List strings in ALL sections (drops the SHF_ALLOC filter; includes `.text`, `.comment`, `.note.*`, etc., with section name in output) |
  | `izz ascii` / `izz utf16` | Same filters as `iz`, applied to the whole-binary scan |
  | `ii` | List imports (undefined dynamic symbols, with GOT slot addresses) |
  | `px <addr> <len>` | Hex dump at virtual address (sparse-collapse: 3+ identical rows collapse to first, `...`, last) |
  | `pxw <addr> <len>` | Hex dump as 32-bit little-endian words (sparse-collapse) |
  | `pxq <addr> <len>` | Hex dump as 64-bit little-endian words (sparse-collapse) |
  | `pxr [addr] [len]` | Pointer-chase dump: 8-byte words, zeros suppressed, non-zero words annotated `-> symname` / `-> symname+0xN` / `-> (no symbol)` |
  | `ps <addr> <len>` | Print string at address |
  | `s <addr>` | Seek to address |
  | `s <section>` | Seek to section start (by name) |
  | `s <symbol>` | Seek to symbol |
  | `s <import>` | Seek to import's GOT slot (resolved via the dynamic relocation table) |
  | `s entry0` | Seek to entry point |
  | `sh` | Show seek history stack (oldest to newest, current marked) |
  | `u` | Undo seek (pop seek history; prints `no seek history` if empty) |
  | `U` | Redo seek (pop redo stack; prints `no seek redo history` if empty) |
  | `pd <n>` | Disassemble `<n>` instructions at current seek (annotates jumps with `; -> <sym>`/`; -> 0x<tgt>` and ` (out)` when the target is outside the window; annotates PLT calls with `; <name>@plt` and direct calls with `; <symname>`) |
  | `pdd <addr> <n>` | Disassemble `<n>` instructions at `<addr>` (same annotation as `pd`) |
  | `af [addr]` | Analyze function: walk forward from current seek (or `<addr>`) one instruction at a time until a `ret`, an `int3` (0xCC) padding byte, an out-of-range jump (backward past start, or forward more than 4 KB), or 4096 instructions. Stores the range so `pdf` can use it. Prints `function at 0xSTART-0xEND (size 0xSIZE, N instructions)`. Basic linear walk; does not handle jump tables, tail calls, or conditional branches |
  | `pdf` | Disassemble the current function (the range last computed by `af`). Prints a hint if `af` hasn't been run |
  | `axt <addr\|sym> [max]` | Find xrefs to an address (or symbol name, resolved first). Scans every executable section for instructions whose immediate or rip-relative effective address matches, then scans data sections for pointer values (qword on 64-bit, dword on 32-bit) that match. Default cap 50 results; `<max>` overrides. One-shot scan, not a precomputed xref DB |
  | `?` | Show command table |
  | `q` / `quit` / `exit` | Quit |

  Addresses accept decimal, `0x`-hex, `entry0`, a section name, a
  symbol name, or an import name (resolved to its GOT slot via the
  dynamic relocation table). Disassembly (`pd` / `pdd` / `pdf` / `af`)
  and `axt` require libcapstone; without it, those commands print a
  hint and return. The prompt shows the current seek: `0x0000000000401000> `.

  `$`-tokens (resolved before symbol names so a symbol named `s` or `e`
  can't shadow them):
  - `$$` — current seek (the REPL's offset). `s $$` is a no-op; `px $$ 16`
    prints 16 bytes at the current seek.
  - `$s` — binary size (file bytes, via `fstat`). `s $s` seeks to one
    past the last byte.
  - `$e` — entry point address. `axt $e` finds xrefs to the entry point;
    `pd $e` disassembles starting at the entry point (seek is unchanged).

  Seek history: every successful `s <addr>` (where `addr` differs from
  the current seek) pushes the prior position onto a 32-entry history
  stack. `u` pops and restores it; `U` re-applies a position popped by
  `u`. Any new `s` clears the redo stack (rizin semantics). `sh` prints
  the stack with `<= current` next to the live position and `(redo)`
  next to positions available via `U`.

  PLT/GOT resolution: `ii` walks the dynamic relocation table
  (`DT_JMPREL` for PLT, `DT_REL`/`DT_RELA` for the regular GOT) and
  prints each import with its GOT slot address and relocation type
  (`R_X86_64_JUMP_SLOT`, `R_X86_64_GLOB_DAT`, `R_X86_64_COPY`). For
  an import with a GOT slot, `s <import_name>` seeks to that slot, so
  `px <import_name> 8` reads the 8-byte function pointer. `pxr` walks
  8-byte words at the current seek and annotates each non-zero word
  with `-> <symname>` if the value falls inside a defined symbol's
  `[vaddr, vaddr+size)` range, or `-> <symname>+0xN` for an in-range
  non-exact match; all-zero words are suppressed. Useful for dumping
  `.got.plt`, `.data.rel.ro`, and vtables.

  Disassembly annotations: `pd`/`pdd` annotate each instruction with
  a trailing `; ...` comment. Jump instructions (`jmp`, `je`, `jne`,
  `jz`, ...) with an immediate `0x<target>` operand get `; ->
  <symname>` (or `; -> <symname>+0xN`, or `; -> 0x<target>` if no
  symbol contains the target); jumps whose target is outside the
  disassembly window are marked ` (out)`. Calls get `; <name>@plt`
  if the target is a PLT entry (the GOT slot is resolved by reading
  the PLT's `jmp [rip+disp32]` and looking it up in the reloc
  table, handling both standard `.plt` and CET `.plt.sec` layouts),
  or `; <symname>` (or `; <symname>+0xN`) for direct calls to a
  known symbol. Direct calls to unknown targets are not annotated.
  Indirect jumps/calls (e.g. `call qword ptr [rip + 0x...]`) are not
  annotated. The simpler text-arrow form is used in place of rizin's
  multi-line ASCII art reflines.

  Sparse hex dump: `px`/`pxw`/`pxq` collapse runs of 3 or more
  byte-identical rows (all-zero `.bss`, repeated fill patterns) into
  the first row, a `...` line, and the last row (with its real
  address). Two identical rows are printed verbatim; only 3+ trigger
  the collapse. Mirrors rizin's `checkSparse` loop.

  String detection (`iz` / `izz`): both ASCII and UTF-16LE strings are
  detected. ASCII strings are runs of `>= 4` printable bytes (0x20..0x7e).
  UTF-16LE strings are runs of `>= 4` two-byte pairs where the low byte
  is printable and the high byte is 0 (e.g. `48 00 65 00 6c 00 6c 00`
  for "Hell"). Output format: `0xADDR  ascii  "string"` or
  `0xADDR  utf16  "string"` (for `iz`); `0xADDR  <section>  ascii
  "string"` (for `izz`, with the section name added so the user can
  tell where strings in unusual places like `.text` and `.comment`
  came from). For non-ALLOC sections (`.comment`, `.strtab`, `.shstrtab`,
  etc.) the address column is the file offset, since these sections
  have no virtual address. `iz ascii` / `iz utf16` filter to one
  encoding; same for `izz`. Mirrors a tiny slice of rizin's
  `librz/util/str_search.c` `process_one_string`.

  Xref scan (`axt`): a one-shot scan, not a precomputed xref database.
  Each invocation disassembles every executable section (`SHF_EXECINSTR`:
  `.text`, `.init`, `.fini`, `.plt`) with Capstone in detail mode and
  checks each instruction's operands. For x86, an `X86_OP_IMM` operand
  equal to `<addr>` is a direct reference (call/jmp target, `mov reg,
  imm`, `cmp reg, imm`, etc.); an `X86_OP_MEM` operand with
  `base = X86_REG_RIP` is a rip-relative reference whose effective
  address is `insn.address + insn.size + disp` (this is how `lea rax,
  [rip + 0xfd8]` references a string in `.rodata` without ever
  containing the literal address). For non-x86 arches (or memory
  operands we don't model), the `op_str` is scanned for the literal
  hex of `<addr>` with a word-boundary check. After the instruction
  pass, data sections (`SHF_ALLOC && !SHF_EXECINSTR`: `.data`,
  `.rodata`, `.data.rel.ro`, `.got`, `.dynsym`, ...) are scanned for
  pointer values (8-byte LE on 64-bit, 4-byte on 32-bit) equal to
  `<addr>`. Output is capped at 50 results by default; `axt <addr>
  <max>` overrides. Mirrors the read-only variant of rizin's
  `axt` (`librz/arch/xrefs.c:164`).

  Function walk (`af` / `pdf`): `af` walks forward from the current
  seek (or `af <addr>`) one instruction at a time until a `ret`, an
  `int3` (0xCC) padding byte on x86, an out-of-range jump (target
  before `start`, or more than 4 KB past the current position), or
  4096 instructions. The range `[start, end)` is stored in the REPL
  state and `pdf` disassembles exactly that range. This is a basic
  linear walk, not rizin's full recursive-descent function detection
  (`librz/arch/fcn.c:1672`); it does not handle jump tables, tail
  calls, or conditional branches (the walk follows the fall-through
  and ignores the taken branch). For typical compiler-emitted
  functions with a single `ret` at the end, the result is correct.

`209 debag --dynamic-db --` `<cmd>` `[args...]`
: Drop into an interactive gdb-style live debugger REPL on `<cmd>`.
  Forks the target under `PTRACE_TRACEME`, stops it at the entry point,
  and presents a `(209-db)` prompt. Software breakpoints via the INT3
  (0xCC) trick, `/proc/PID/mem` for memory access, rbp-chain walk for
  backtraces, a minimal x86-64 instruction-length decoder for
  step-over (libcapstone used if available), and hardware
  watchpoints via the x86 debug registers (DR0-DR7). x86-64 only;
  single-threaded.

  Commands (empty line repeats the last command, numbers are hex):

  | Command | Action |
  |---|---|
  | `db <addr>` | Set breakpoint at address |
  | `db <sym>` | Set breakpoint at symbol name (resolved via static analysis) |
  | `db` | List all breakpoints |
  | `db- <addr>` | Remove breakpoint |
  | `dc` | Continue execution |
  | `ds` | Single step (one instruction) |
  | `dso` | Step over (step past a call instruction) |
  | `dr` | Print all GP registers + rip + eflags |
  | `dr <reg>` | Print single register |
  | `dr <reg>=<val>` | Set register (hex) |
  | `watch <addr\|sym> [len]` | Set hardware write watchpoint (default len=1, max 4 slots) |
  | `watchw <addr\|sym> [len]` | Set hardware write watchpoint (alias for `watch`) |
  | `watchr <addr\|sym> [len]` | Set hardware read/write watchpoint |
  | `watcha <addr\|sym> [len]` | Set hardware access (read or write) watchpoint (= `watchr` on x86) |
  | `watchx <addr\|sym>` | Set hardware execute breakpoint (like `db` but uses DR0-DR7; works on read-only memory) |
  | `watch` | List active hardware watchpoints (DR0-DR3, max 4) |
  | `watch- <slot\|addr>` | Remove one hardware watchpoint (slot 0-3 or its watched address) |
  | `watch- *` | Remove all hardware watchpoints |
  | `px <addr\|reg> <len>` | Hex dump of memory at address or in register |
  | `ps <addr\|reg> <len>` | Print string at memory |
  | `bt` | Backtrace (rbp chain, max 64 frames) |
  | `sym <addr>` | Find nearest symbol at or before address |
  | `info` | Print process status (pid, state, rip, bp count, hw watchpoint count) |
  | `handle` | List signal dispositions (stop/print/pass per signal) |
  | `handle <sig> <stop\|nostop> <print\|noprint> <pass\|nopass>` | Set signal disposition (e.g. `handle SIGINT nostop noprint pass` to let SIGINT through without breaking) |
  | `help` \| `?` | List commands |
  | `q` \| `quit` \| `exit` | Kill child and quit |

  Hardware watchpoints: `watch <addr|sym> [len]` arms one of the four
  x86 debug-register slots (DR0-DR3). DR7 is programmed with the
  slot's length (1, 2, 4, or 8 bytes; default 1) and access mode
  (write, read/write, or execute). DR6 reports which slot(s) fired on
  a SIGTRAP; debag reads it on every stop, prints `Hardware watchpoint
  N (DRN) triggered at 0x<addr>...`, then clears DR6 (Linux does not
  auto-clear it). Watched ranges must be naturally aligned (a 4-byte
  watch must be 4-byte aligned, etc.) or the kernel rejects the
  POKEUSER; debag pre-checks alignment and prints
  `address 0x... not aligned to length N`. On modern x86 with DR7
  LE+GE set (which debag always sets), data watchpoints use
  trap-after semantics: the faulting instruction has already executed
  and rip points at the next instruction, so `dc` does not re-trigger
  the watchpoint. Execute watchpoints (`watchx`) use trap-before: rip
  points AT the wp address; the next `dc`/`ds`/`dso` sets RF (Resume
  Flag) in EFLAGS so the CPU ignores the wp for one instruction, then
  re-arms automatically. Symbols (both STT_FUNC and STT_OBJECT) are
  resolved by name, so `watch x` watches a `volatile int x` and
  `watchx main` sets an execute bp at `main` (useful for code in
  read-only memory where INT3 insertion would fail). The watched
  address can be unmapped; the watchpoint only fires when the CPU
  actually accesses it. Hardware watchpoints are per-process and not
  inherited across fork (debag does not trace forks). This is
  hardware-assisted: zero runtime overhead, exact instruction
  pinpointing, the killer feature for memory-corruption debugging.

  Signal handling: by default, fatal signals (SIGSEGV, SIGBUS, SIGFPE,
  SIGILL, SIGTRAP, SIGINT) stop and print, and are forwarded to the
  child on the next `dc`. Benign signals that programs typically handle
  internally (SIGALRM, SIGCHLD, SIGUSR1, SIGUSR2, SIGIO, SIGURG,
  SIGWINCH, SIGPIPE, SIGVTALRM, SIGPROF) pass through without stopping
  so timers, child reaping, and window-resize handling are not
  disrupted. Use `handle` to change any signal's disposition.

  Known limitations: frameless functions (compiled with
  `-fomit-frame-pointer`) break the rbp-chain backtrace walk; the walk
  terminates gracefully via the `next_rbp > rbp` sanity check. Stripped
  binaries with no useful `.symtab`/`.dynsym` produce `<unknown>` for
  `sym` and `bt`. Step-over (`dso`) on exotic call encodings may
  miscount instruction length and fall back to a single step.

`209 <n> rollback`
: Roll back to generation #n. Repoints the current-generation symlink
  and rebuilds the symlink farm. A reboot is recommended for the full
  system state to take effect.

`209 <n> pin`
: Pin generation #n so garbage collection won't reap its store paths.

### Binary cache commands

`209 keygen`
: Generate a new Ed25519 keypair for signing narinfos. Prints the
  public key (to put in `extra.nix` on subscriber machines) and a
  `KeyName:PublicKey:SecretKey` triple (to put on the publishing
  machine).

`209 cache push` `<store-path>`
: Upload a store path and its full closure (computed from the refs
  graph) to all configured substituters. For each path: compute the
  NAR hash, build a narinfo, sign it with the configured Ed25519 key,
  upload both. Idempotent.

`209 cache pull` `<store-path>`
: Explicitly fetch a store path from configured substituters. Normally
  not needed: the install path consults substituters automatically
  before falling through to the Arch mirror. Useful for debugging or
  for pre-populating a machine.

### Multi-subject

`209 <pkg1> <pkg2> ... <verb>`
: Apply the verb to all subjects. For example:
  `209 vim curl install` installs both.

## OPTIONS

`-V`, `--version`
: Print version and exit.

`-h`, `--help`
: Print help and exit.

## CONFIGURATION

Two config files, different jobs.

`2O9.nix` is the declarative config. Nix syntax. Says what your system
should look like. Two scopes:

- **User:** `~/.config/2O9/home.nix`
- **System:** `/etc/2O9/2O9.nix`

Both are evaluated and merged per `DESIGN.md` section 7: global wins on
conflict, packages concatenate. See [`docs/CONFIG.md`](./CONFIG.md) for
the full schema reference.

`extra.nix` is the imperative side config. Also Nix syntax (per
locked decision #7: "One declarative config format: Nix"). Holds
stuff that doesn't belong in the declarative config: substituter URLs,
signing keys, AUR build flags, chroot settings. Lives at
`~/.config/2O9/extra.nix` (or `/etc/2O9/extra.nix` for system-wide).
See [`docs/CONFIG.md`](./CONFIG.md) for the schema.

## FILES

`/etc/2O9/2O9.nix`
: System-wide declarative config.

`/etc/2O9/extra.nix`
: System-wide imperative side config (substituters, signing keys).

`~/.config/2O9/home.nix`
: Per-user declarative config (overlaid on the system config).

`~/.config/2O9/extra.nix`
: Per-user imperative side config.

`/var/lib/2O9/`
: System generation DB. One subdirectory per generation, each
  containing a `manifest.json` and a `diff.json`.

`/var/lib/2O9/store.sqlite`
: System SQLite store DB. Tables: `valid_paths`, `refs`. Used by GC
  to compute the closure of live paths.

`~/.local/state/2O9/`
: Per-user generation DB and store DB (same layout as the system DB).

`/nix/store/`
: The store. Package files live here as
  `/nix/store/<base32-hash>-<name>-<version>/`. The hash is computed
  from the NAR serialisation of the extracted tree.

`/nix/store/.links/`
: Hardlink pool used by `209 optimise`. Files are linked here by their
  SHA-256 hash, then store paths hardlink back to the pool.

`/nix/store/.tmp/`
: Staging directory for atomic extraction. 2O9 extracts here, then
  `rename()`s to the final store path. Hidden from normal store scans.

`~/.local/bin/`, `~/.local/lib/`
: Symlink farm targets. The shell's `$PATH` should include
  `~/.local/bin`.

## ENVIRONMENT

`HOME`
: Determines the user config and state directories.

`SUDO_USER`
: When 2O9 runs as root via sudo, this is used to resolve the original
  user's home directory. Ensures that `209 apply` (running as root)
  reads the same sync DB that `209 sync` (running as the user) wrote.

`TWO09_DEBUG`
: If set, prints extra debug output during install, apply, and store
  operations.

`TWO09_TEST_MODE`
: If set, uses fake store paths and skips real downloads. Used by the
  test suite.

`TWO09_PKG_PATH`
: If set, uses the given `.pkg.tar.zst` instead of downloading. Useful
  for testing extraction against a known package.

`LIB2O9_DEPS_PREFIX`
: Build-time only. Points the Makefile at a user-local install of
  libarchive-dev / libgpgme-dev / libsqlite3-dev headers when
  system-wide install isn't available.

## SEE ALSO

- [`README.md`](../README.md) - overview and quick start
- [`DESIGN.md`](../DESIGN.md) - full architecture
- [`docs/CONFIG.md`](./CONFIG.md) - `2O9.nix` and `extra.nix` schema reference
- [`lib/2O9/nix/README.md`](../lib/2O9/nix/README.md) - Nix evaluator
- [`lib/2O9/alpm/MODIFICATIONS.md`](../lib/2O9/alpm/MODIFICATIONS.md) - libalpm mods

## AUTHOR

Rui-727 <https://github.com/Rui-727>

## LICENSE

GPL-2.0-only. Inherited from pacman, the engine we link against.
