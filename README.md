# 2O9

Its either 2O9 or Pacman. your choice! its not my rights to judge people.

2O9 is a package manager for Arch Linux that puts files in `/nix/store/`
instead of `/`. The binary is `209`. It takes three tools that normally
don't talk to each other (pacman, paru, and Nix) and makes them one:

1. **pacman's engine** (libalpm), copied into the tree and modified
   directly as **lib2O9**. All the good parts: dependency resolution,
   repo sync, database parsing, hooks. The solver reads from the
   generation DB instead of `/var/lib/pacman/local/`, and the install
   backend dispatches to the store adapter. See
   `lib/2O9/alpm/MODIFICATIONS.md` for what we changed.
2. **paru's AUR workflow**, rewritten in C. AUR RPC, PKGBUILD clone
   and review, recursive dependency resolution, makepkg orchestration,
   chroot builds, PGP key import.
3. **A real `/nix/store`** with content-addressed paths
   (`/nix/store/<base32-hash>-<name>-<version>/`), atomic generations,
   a references graph, hardlink dedup, and binary cache substitution.
   Everything is driven by a Nix-syntax config file (`2O9.nix`) that
   declares what your system should look like. The Nix evaluator is
   **written from scratch in C** as part of lib2O9, not a vendored copy
   of the C++ nix source. It does the function form (`{ config, ... }:
   ...`) with fixed-point recursion for self-reference, plus `import`
   for splitting configs across files.

There's also **Trakker**, a ptrace sandbox for running untrusted
commands with syscall tracing, network blocking, and write redirection.
And **Debag**, a hybrid sandbox that uses seccomp for the fast path and
ptrace only for syscalls that need argument inspection, so the target
runs at about 90% native speed instead of 20%.

Rollback is a symlink swap plus a reboot. No boot-time rollback
machinery, no daemon, no service manager. You switch generations,
reboot, and systemd starts whatever's enabled. That's it.

## The package repo is always Arch Linux

The repository is always an Arch Linux mirror, configured in `2O9.nix`
under `pacman.repos`. No custom repos, no alternative sources for the
binary packages themselves. We just put the files in `/nix/store/`
instead of `/`. You can configure one or more 2O9 binary caches (HTTP
or S3) to share built packages between machines without re-downloading
from the mirror.

## Naming

| | Form | Where |
|---|---|---|
| Binary / command | `209` (numeric) | what you type |
| Project / branding | `2O9` (letter O) | docs, repo name, on-disk paths (`/nix/config/`, `/var/lib/2O9/`) |
| Merged internal library | `lib2O9` | static `lib2O9.a`, modified libalpm plus own C Nix evaluator |

## Build

```sh
make                            # builds 209 + 3 test binaries
make install                    # installs 209 to /usr/bin
make install PREFIX=/usr/local  # custom prefix
make clean                      # remove build artifacts
```

`make` builds four targets:

| Target | What it is |
|---|---|
| `209` | The CLI binary: SOV dispatch, store adapter, generation DB, AUR pipeline, Trakker, Debag, Nix evaluator |
| `test-aur-rpc` | AUR RPC client unit tests (libcurl + cJSON) |
| `test-nix-lexer` | Nix lexer unit tests |
| `test-nix-eval` | Nix evaluator unit tests (49 tests) |

Build dependencies: a C compiler, `make`, `libcurl-dev`,
`libarchive-dev`, `openssl-dev`, `libgpgme-dev`, `libsqlite3-dev`, and
`libxml2-dev`. Optional: `libsodium-dev` for Ed25519 cache signatures
(falls back to OpenSSL Ed25519 if not installed).

lib2O9 (the modified libalpm plus own C Nix evaluator plus cJSON) is
built into `lib2O9.a` and linked into the `209` binary. The Makefile's
`LIB2O9_DEPS_PREFIX` variable lets you point at a user-local install
of the dev headers (e.g. `~/local/`) when system-wide install isn't
available.

Running tests:

```sh
make test           # unit tests (nix-eval, nix-lexer, aur-rpc) plus integration tests
make test-nix-eval  # just the Nix evaluator unit tests (49 tests)
```

## Quick start

First, create a config:

```sh
209 init                    # creates /nix/config/<user>.nix + <user>.extra.nix
sudo 209 init --system     # creates /nix/config/2O9.nix + extra.nix (system-wide)
sudo 209 init --all        # creates configs for every detected user in /etc/passwd
```

Edit the config to list what you want, then apply it:

```sh
209 -Sy                     # download repo databases (same as: 209 sync)
sudo 209 apply              # make the system match your config
```

Day to day. Pacman flags work too:

```sh
209 -S neovim               # install (same as: 209 neovim install)
209 -R htop                 # remove (same as: 209 htop remove)
209 -Q                      # list all installed packages
209 -Qs vim                 # search installed packages
209 -Qi neovim              # show package info
209 -Ql neovim              # list files in a package
209 -Qm                     # list foreign (AUR) packages
209 -Ss ffmpeg              # search repos
```

Build from the AUR:

```sh
209 yt-dlp aur build        # build from AUR (chroot + makepkg + store add)
209 ffmpeg aur search       # search AUR
209 ffmpeg aur review       # review PKGBUILD diff before building
```

Generations and rollback:

```sh
209 generations             # list all generations
209 3 rollback              # go back to generation 3
209 3 pin                   # protect it from GC
```

Garbage collection and dedup:

```sh
209 gc                      # delete unreferenced store paths (closure-aware)
209 gc --optimise           # GC, then hardlink-dedup the remaining store
209 optimise                # dedup only
```

Binary cache (share packages between machines):

```sh
209 keygen                  # generate an Ed25519 keypair, print to stdout
209 cache push /nix/store/<hash>-neovim-0.10.0   # upload a path + closure
209 cache pull /nix/store/<hash>-neovim-0.10.0   # explicitly fetch from cache
# normal install path also consults subs automatically
```

Snapshots (content-addressed copies of declared paths):

```sh
# Declare in 2O9.nix:
#   snapshots."/var/lib/postgres" = { auto = "daily"; keep = 7; };
209 snapshot take /var/lib/postgres       # take a snapshot now
209 snapshot list /var/lib/postgres       # list snapshots of a path
209 snapshot diff 1 2                     # diff two snapshots of the same path
209 snapshot restore 1                    # restore (auto-snapshots current state first)
209 snapshot rm 1                         # remove from the DB (store path GC'd later)
```

Paths declared under `snapshots` in `2O9.nix` (absolute) or `<user>.nix`
(relative to home) are "managed". `209 snapshot take` only works on
managed paths. `auto = "hourly"|"daily"|"weekly"` installs a systemd
timer; `auto = "manual"` means snapshots only via the CLI. See
[`docs/CONFIG.md`](./docs/CONFIG.md) for the schema.

NAR file sharing (share any file or folder by hash):

```sh
209 share /home/me/dotfiles        # hash, copy to /nix/store/, push, print nar://<hash>
209 share ls                       # list local shares
209 get nar://<hash> /tmp/restore  # fetch a share by URI and extract to /tmp/restore
209 subs                           # interactive picker: browse subs and their contents
209 subs <name>                    # print one sub's details and contents
```

Trakker (sandbox, command resolved via `$PATH`):

```sh
209 trakker ls -la
209 trakker --no-net -- curl https://example.com
209 trakker --no-write --redirect-writes /tmp/trakker -- makepkg -f
```

Testing the pipeline without a real `/nix/store`:

```sh
# End-to-end test with a fake store path
TWO09_TEST_MODE=1 209 sl install

# Or feed it a real .pkg.tar.zst
TWO09_PKG_PATH=/var/cache/pacman/pkg/neovim-0.10.0-1-x86_64.pkg.tar.zst \
  209 neovim install
```

## Architecture

Six pieces, layered. The CLI dispatches into either the declarative
engine or the AUR helper; both produce transactions that flow through
lib2O9 (the modified libalpm plus Nix evaluator) into the store adapter,
which writes to `/nix/store` and rebuilds the symlink farm. Trakker
sits beside all of it as an on-demand sandbox. The full diagram is in
[`DESIGN.md`](./DESIGN.md) section 4.

1. **CLI**: the entrypoint. SOV dispatch (`209 <subject> <verb>`) for
   package ops, leading-form for trakker (`209 trakker ls -la`).
2. **Declarative Engine**: turns `2O9.nix` into a transaction.
   Evaluate, reconcile against the current generation, produce an
   install/remove plan.
3. **AUR Helper**: paru ported to C. RPC, PKGBUILD clone, review diff,
   recursive dep resolution, chroot build, PGP key import, makepkg,
   into the store.
4. **Trakker**: ptrace sandbox. Records what a command does, blocks
   what you tell it to.
5. **lib2O9**: modified libalpm (solver reads from the generation DB,
   install backend dispatches to the store adapter, transaction is
   wired through `alpm_trans_init`/`prepare`) plus our own C Nix
   evaluator. One static library.
6. **Store Adapter**: puts packages in `/nix/store/` with content-
   addressed paths, registers them in the SQLite refs graph, then
   symlinks them into `~/.local/bin`, `~/.local/lib`, and `/etc/`.

## Configuration

There is no `config.toml`, no `paru.conf`, no `pacman.conf`. One file
format for everything: Nix. All config lives under `/nix/config/`:

| File | Owner | Scope |
|---|---|---|
| `/nix/config/2O9.nix` | root:root | system-wide declarative (packages, services, repos) |
| `/nix/config/extra.nix` | root:root | system-wide runtime (bin paths, subs, chroot) |
| `/nix/config/<user>.nix` | `<user>:<user>` 0644 | per-user declarative |
| `/nix/config/<user>.extra.nix` | `<user>:<user>` 0644 | per-user runtime |

`<user>` is the Unix username (from `getpwuid(getuid())`, or `SUDO_USER`
when running under sudo). When running as root, `209 init --all`
scans `/etc/passwd` for every user with uid >= 1000 and a `/home/*`
home dir (excluding root and nobody) and creates a config pair for each,
chowned to that user.

2O9 evaluates only `/nix/config/2O9.nix`. That file is the single entry
point. User configs (`<user>.nix`) take effect only if `2O9.nix`
imports them via standard Nix `import`:

```nix
# /nix/config/2O9.nix
let myuser = import ./myuser.nix; in
{ config, ... }:
{
  packages = [ "vim" ] ++ myuser.packages or [];
}
```

Same for `extra.nix`: only `/nix/config/extra.nix` is loaded. User side
configs (`<user>.extra.nix`) take effect only if `extra.nix` imports
them. This gives the sysadmin explicit control over what is active.

`~/.local/bin` is in `$PATH`. That is the visibility mechanism: binaries
from the store are symlinked there. Libraries go to `~/.local/lib/`.
Config files stay at their real paths: `/etc/` is `/etc/`, never
`~/.local/etc/`.

The `extra.nix` file is for stuff that doesn't belong in the declarative
config: substituter URLs, signing keys, AUR build flags. Per locked
decision #7 in DESIGN.md ("One declarative config format: Nix"), there
is no INI file. See [`docs/CONFIG.md`](./docs/CONFIG.md) for the full
schema.

If 2O9 detects a pre-v2 config layout (`~/.config/2O9/`, `/etc/2O9/`),
it prints the exact `mv` commands to migrate and exits 1. No
auto-migration. See [`docs/MIGRATION.md`](./docs/MIGRATION.md).

Services are managed with `systemctl enable`/`disable`. There is no
2O9 service manager. After `209 apply` changes which systemd units are
visible in the store, the user reboots (or manually runs `systemctl
daemon-reload` and starts/stops the relevant units) for the new state
to take full effect. This is simple and correct: systemd starts
exactly what is enabled.

## Commands

2O9 supports two CLI styles. Pacman flags (`-S`, `-R`, `-Q`) for muscle
memory, and 2O9's own Subject-Object-Verb order for the new stuff.
Both work; pick whichever feels right.

### Pacman-compatible flags

| Flag | Meaning | 2O9 equivalent |
|---|---|---|
| `209 -S <pkg>...` | Install | `209 <pkg> install` |
| `209 -Sy` | Refresh repo DBs | `209 sync` |
| `209 -Su` | Upgrade all | `209 apply` |
| `209 -Ss <term>` | Search repos | `209 <term> search` |
| `209 -Si <pkg>` | Package info | `209 <pkg> info` |
| `209 -R <pkg>...` | Remove | `209 <pkg> remove` |
| `209 -Q` | List all installed | (none) |
| `209 -Qs <term>` | Search installed | `209 <term> search` |
| `209 -Qi <pkg>` | Installed info | `209 <pkg> info` |
| `209 -Ql <pkg>` | List files in package | (none) |
| `209 -Qm` | List foreign (AUR) packages | (none) |

### 2O9 commands

| Command | Meaning | Origin |
|---|---|---|
| `209 <pkg> install` | Install a package temporarily (not declared in `2O9.nix`) | pacman |
| `209 <pkg> remove` | Remove a package. New generation without it, rebuild symlink farm | pacman |
| `209 <pkg> info` | Show package info | pacman |
| `209 <term> search` | Search repos | pacman |
| `209 <pkg> aur build` | Build from AUR (chroot by default) | paru |
| `209 <term> aur search` | Search AUR | paru |
| `209 <pkg> aur review` | Review PKGBUILD diff | paru |
| `209 trakker [flags] [--] <cmd> [args...]` | Run a command in the sandbox (PATH-resolved) | new |
| `209 debag [flags] [--] <cmd> [args...]` | Run a command in the seccomp+ptrace hybrid sandbox | new |
| `209 apply` | Apply declarative config (`2O9.nix`) | new |
| `209 <n> rollback` | Roll back to generation #n | new |
| `209 <n> pin` | Pin a generation (protect from GC) | new |
| `209 generations` | List generations | new |
| `209 gc [--optimise]` | Garbage-collect unreferenced store paths | new |
| `209 optimise` | Hardlink-dedup the store | new |
| `209 sync` | Sync repo databases | pacman |
| `209 news` | Show Arch Linux news | paru |
| `209 cache push <path>` | Upload a store path and its closure to configured caches | new |
| `209 cache pull <path>` | Explicitly fetch a store path from configured caches | new |
| `209 keygen` | Generate an Ed25519 keypair for signing narinfos | new |
| `209 snapshot take <path>` | Take a snapshot of a declared path (NAR-hashed, content-addressed) | new |
| `209 snapshot list [path]` | List snapshots (all or filtered by path) | new |
| `209 snapshot restore <id>` | Restore a snapshot by ID (auto-snapshots current state first) | new |
| `209 snapshot diff <id1> <id2>` | Diff two snapshots of the same path | new |
| `209 snapshot rm <id>` | Remove a snapshot from the DB (store path GC'd later) | new |
| `209 share <path>` | Share a file or folder as a NAR blob, push to subs | new |
| `209 share ls` | List local shares | new |
| `209 share rm <hash>` | Remove a share from the store | new |
| `209 get <uri> [dest]` | Fetch a share by `nar://<hash>` URI and extract | new |
| `209 get pkg <name>` | Explicitly fetch a package from caches | new |
| `209 subs` | Interactive subscription picker (TUI) | new |
| `209 subs <name>` | Print one sub's details and contents | new |
| `209 subs add/rm <name>` | Add or remove a sub from config | new |

**Special subjects**: `apply`, `generations`, `sync`, `news`, `gc`,
`optimise` are zero-argument commands. They have no subject, only a
verb. They operate on the system as a whole, not on a named thing.

**Multi-subject**: `209 nginx firefox install` installs both. The verb
comes last, applied to everything before it.

## Trakker and Debag

**Trakker** is a ptrace sandbox. It runs a command, records everything
it does, and optionally restricts it. The command is resolved via
`$PATH`, so bare names work:

```sh
209 trakker ls -la
209 trakker --no-net -- curl https://example.com
209 trakker --no-write --redirect-writes /tmp/trakker -- makepkg -f
```

Use `--` to separate trakker's flags from the command's own args.
Trakker uses ptrace to intercept syscalls and records file I/O,
network connections, process forks/execs, and mmap activity. The trace
comes out as JSON.

**Debag** is the fast version. It uses seccomp-bpf for the fast path
(most syscalls are allowed directly in the kernel, nanosecond overhead)
and ptrace only for the dangerous ones that need argument inspection
(execve, connect, open, mount). The target runs at about 90% native
speed instead of 20% with pure ptrace. Debag also does static analysis:
it scans the ELF binary's symbol table to figure out what syscalls it
will probably use, then builds a seccomp filter from that.

```sh
209 debag --static-scan -- /bin/ls    # see what a binary does before running it
209 debag --no-net -- curl URL        # run with network blocked
209 debag --fast-mode -- ls -la       # seccomp only, fastest
209 debag --static-db -- /bin/ls      # interactive rizin-style ELF REPL
209 debag --dynamic-db -- /bin/ls     # interactive gdb-style live debugger
```

Debag also has an interactive static-analysis REPL, `--static-db`,
modelled on rizin. It drops you at a `0xADDR>` prompt where you can
inspect the parsed ELF: list sections (`iS`), segments (`iSS`), symbols
(`is`), imports (`ii`, with GOT slot addresses resolved from the
dynamic relocation table), strings (`iz`); hex-dump any virtual address
(`px`, `pxw`, `pxq`, with sparse-collapse of long zero runs);
pointer-chase dump for `.got`/vtables (`pxr`, annotates each non-zero
word with `-> symname`); seek to sections / symbols / imports / `entry0`
(`s`) and undo/redo seeks (`u`, `U`, `sh`); and disassemble with
libcapstone (`pd`, `pdd`) where jumps get `; -> <sym>` / `; -> 0x<tgt>
(out)` annotations and calls get `; <name>@plt` (PLT) or `; <symname>`
(direct). Type `?` inside the REPL for the full command table.

For live debugging, `--dynamic-db` forks the target under `ptrace`,
stops it at the entry point, and drops you at a `(209-db)` prompt
modelled on gdb. Software breakpoints via the INT3 (0xCC) trick
(`db <addr|sym>`), continue (`dc`), single-step (`ds`), step over a
call (`dso`), inspect registers (`dr`), hex-dump memory (`px`), print
strings (`ps`), walk the rbp chain for a backtrace (`bt`), resolve
nearest symbol (`sym <addr>`), set hardware watchpoints via the
x86 debug registers (`watch <addr|sym> [len]`, `watchw`, `watchr`,
`watcha`, `watchx <addr|sym>` for execute breakpoints, `watch-` to
remove), and manage signal dispositions (`handle`). Hardware
watchpoints stop the program on the exact instruction that writes or
reads a watched memory location, with zero runtime overhead
(hardware-assisted, not single-step-and-check). Four slots (DR0-DR3),
lengths 1/2/4/8 bytes, write / read-write / execute modes; `watch x`
works on a `volatile int x` (STT_OBJECT symbols are resolved by
name). Signals that last stopped the child are forwarded on `dc`
gated by a per-signal `stop`/`print`/`pass` table: SIGSEGV/SIGBUS/
SIGFPE/SIGILL/SIGTRAP/SIGINT stop+print+pass by default, while
SIGALRM/SIGCHLD/SIGUSR1/SIGUSR2/SIGIO/SIGURG/SIGWINCH/SIGPIPE pass
through without breaking so timers and child reaping keep working.
Empty line repeats the last command; `q` kills the child and quits.
x86-64 only; single-threaded.

Restriction flags (both Trakker and Debag):

| Flag | Effect |
|---|---|
| `--no-net` | Block all network access |
| `--no-write` | Block all file writes |
| `--redirect-writes <dir>` | Redirect writes into `<dir>` instead of their real paths |
| `--allow-net port=443` | Allow only the listed port(s) (repeatable) |

## How the store works

Two big ideas that make 2O9 worth using over plain pacman.

**Content-addressed paths.** Every store path looks like
`/nix/store/<base32-hash>-<name>-<version>/`. The hash is computed
from the NAR serialisation of the extracted tree (a canonical byte
stream, SHA-256, compressed to 20 bytes, Nix-base32-encoded). Two
builds of the same package produce the same path. Tampering is
detectable. Cross-machine sharing works.

**References graph.** A SQLite DB at `~/.local/state/2O9/store.sqlite`
records which store paths depend on which others. GC walks the closure
of all generations' root paths and deletes only what's truly
unreachable. Deps don't get reaped when their parent is still alive.

**Hardlink dedup** (`209 optimise`) walks the store, SHA-256s every
regular file, and hardlinks identical files into
`/nix/store/.links/<sha256>`. Two packages shipping the same 50 MB
locale file cost 50 MB on disk instead of 100 MB.

**Binary cache substitution**. Configure one or more named subs in
`/nix/config/<user>.extra.nix` (or `/nix/config/extra.nix` for
system-wide):

```nix
subs = {
  personal = {
    URLs = [ "https://cache.example.com" "s3://backup-bucket" ];
    PublicKeys = [ "<from 209 keygen>" ];
    AllowUnsigned = false;
    SigningKey = "/etc/2O9/personal-secret-key";
    KeyName = "personal-1";
  };
  friend = {
    URLs = [ "https://friend.example.org/cache" ];
    PublicKeys = [ "<friend's pubkey>" ];
    AllowUnsigned = false;
  };
};
```

A narinfo is accepted if any of the listed `PublicKeys` verifies its
signature. `209 cache push` pushes to every sub that has a `SigningKey`,
signed with that sub's own key. `209 cache pull` (and install-time
substitution) pulls from every sub in config order. The old flat
`substituters` block still parses as a single sub named `legacy` with a
`PublicKey` (singular) field; a deprecation warning is printed.

**NAR file sharing**. `209 share <path>` NAR-hashes any file or
folder, copies it into `/nix/store/<hash>-share-<basename>/`, pushes
it to every sub that has a `SigningKey`, and prints `nar://<hash>`.
The receiver runs `209 get nar://<hash> <dest>` to fetch and extract.
Each push also appends to the sub's `index.json`, so `209 subs` can
list shares, packages, and snapshots side by side. Use it for
dotfiles, ad-hoc file transfer between machines, or anything that
needs content addressing without going through a PKGBUILD.

## Documentation

- [Tutorial](./docs/TUTORIAL.md): getting started, first hour of use
- [Cookbook](./docs/COOKBOOK.md): common tasks as recipes
- [Use cases](./docs/USE_CASES.md): real-world scenarios
- [Migration guide](./docs/MIGRATION.md): from pacman, paru, Nix, NixOS
- [Manpage](./docs/MANPAGE.md): command reference
- [Config reference](./docs/CONFIG.md): `2O9.nix` and `extra.nix` schema
- [Design document](./DESIGN.md): full architecture

## Status

What works and what's left.

- **Phase 0 (Foundation): DONE.** Repo, Makefile, builds clean.

- **Phase 1 (Store adapter + lib2O9): DONE.** Store adapter, symlink
  farm, and generation DB all work. All three modifications to vendored
  libalpm are applied (see
  [`MODIFICATIONS.md`](./lib/2O9/alpm/MODIFICATIONS.md)):
  1. Install backend dispatches to the store adapter instead of
     extracting to `/`.
  2. Solver reads the installed set from the generation DB instead of
     `/var/lib/pacman/local/`.
  3. Config is programmatic from a JSON manifest, no `pacman.conf`.

  lib2O9 is built and linked (2.4 MB static library, 216 alpm symbols).
  `209 sync` calls the real `alpm_db_update()`. The libalpm transaction
  is wired through `alpm_trans_init` + `alpm_trans_prepare`, so the
  solver, conflict detection, and cycle detection are all live.
  Package signatures are verified via gpgme when `SigLevel` is set.

- **Phase 2 (AUR helper): DONE.** AUR RPC, PKGBUILD clone, review,
  recursive dep resolution, makepkg. Chroot builds via `mkarchroot` +
  `arch-nspawn` + `makechrootpkg` (on by default). PGP key auto-import
  for `validpgpkeys`. MFlags pass-through from `extra.nix`.

- **Phase 3 (Declarative engine): DONE.** The Nix evaluator handles
  everything `2O9.nix` needs: attrsets, lists, strings, let, if/else,
  lambdas (curried and formal), imports, fixed-point recursion for
  `{ config, ... }`, `inherit`, all 9 binary operator precedence
  levels, 19 builtins. 49/49 tests pass. `209 apply` evaluates
  `/nix/config/2O9.nix`. User configs take effect only if `2O9.nix`
  imports them via standard Nix `import`.

- **Phase 4 (Trakker + Debag): DONE.** ptrace sandbox with all four
  restriction flags, JSON trace output. Debag hybrid sandbox with
  seccomp fast path and ptrace slow path.

- **Phase 5 (Polish): DONE, except packaging.** Everything listed below
  ships and runs. The 9-step activation phase is implemented in
  `src/declarative/activation.c`. The convenience commands all work:
  `209 init`, `209 news`, `209 doctor`, `209 wiki`, `209 fuzz`,
  `209 bundle`, `209 import`, `209 diff`, `209 why`, `209 lock`,
  `209 -Su`. Pacman flags (`-S`, `-R`, `-Q`, `-Qs`, `-Qi`, `-Ql`,
  `-Qm`) all work. Color output is wired (bold package names, green
  for success, red for errors, cyan for hints, dim for secondary
  info). 10 integration tests in `test/` pass. The only thing left
  is distro packaging (a `PKGBUILD` for self-hosting) and more test
  coverage. Neither blocks usage.

- **Roadmap phases 0 through 3 (transaction wiring, AUR isolation,
  content addressing, binary cache substitution): DONE.** See the
  commit history for `phase 0:`, `phase 1:`, `phase 2:`, `phase 3:`
  commits. The store is content-addressed, GC is closure-aware,
  AUR builds run in a chroot, and packages can be shared between
  machines via Ed25519-signed narinfos.

- **Phase 4 (Declarative system): DEFERRED.** Whole-OS reconfigure
  (bootloader including grub, initrd, kernel, services as a DAG,
  users, PAM, NSS, profile hooks as derivations). Tracked in the
  project TODO as future work. 2O9 today is a package manager with
  generations, not a NixOS competitor.

## License

GPL-2.0-only. See [`LICENSE`](./LICENSE).
