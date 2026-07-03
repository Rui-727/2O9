# Migration Guide

If you are coming to 2O9 from another tool, this doc maps your existing
mental model onto 2O9's. Each section covers what is the same, what is
different, and a command mapping table.

## From pacman

You already know pacman. 2O9 uses libalpm (copied into the tree as
lib2O9 and modified), so the solver, the sync DB format, the signature
verification, the repo structure, and the package format are all the
same. The Arch repos work as-is. You do not need new mirrors, new keys,
or new packages. What is different is where files land and how the
installed set is tracked.

Under pacman, a package's files land at their declared paths (`/usr/bin/foo`,
`/etc/foo.conf`, etc.) and the installed set is tracked in
`/var/lib/pacman/local/`. Under 2O9, a package's files land in
`/nix/store/<base32-hash>-<name>-<version>/` and the installed set is
tracked in the generation DB at `~/.local/state/2O9/` (or
`/var/lib/2O9/`). The symlink farm at `~/.local/bin/` and `~/.local/lib/`
points into the store. This means two versions of the same package can
coexist, no file conflicts, and rollback is a symlink swap.

What is the same: the repos, the sync DBs, the package tarball format,
the signature keys, the keyring, the dependency solver, the conflict
detection, the upgrade path, the search, the package info, the file
listing. Pacman flags (`-S`, `-R`, `-Q`, `-Sy`, `-Su`, `-Ss`, `-Si`,
`-Qi`, `-Ql`, `-Qm`) all work in 2O9.

What is different: no `/var/lib/pacman/local/`, no `pacman.conf` (it is
`2O9.nix`), no `pacman -Sc` (use `209 gc`), no `pacman -U` (use `209
import` for a local `.pkg.tar.zst`), and the binary cache protocol is
2O9's own NAR-based one, not pacman's.

| pacman | 2O9 |
|---|---|
| `pacman -Syu` | `209 sync && sudo 209 apply` (or `sudo 209 -Su`) |
| `pacman -S <pkg>` | `209 <pkg> install` (or `209 -S <pkg>`) |
| `pacman -R <pkg>` | `209 <pkg> remove` (or `209 -R <pkg>`) |
| `pacman -Ss <term>` | `209 <term> search` (or `209 -Ss <term>`) |
| `pacman -Si <pkg>` | `209 <pkg> info` (or `209 -Si <pkg>`) |
| `pacman -Q` | `209 -Q` |
| `pacman -Qs <term>` | `209 -Qs <term>` |
| `pacman -Qi <pkg>` | `209 -Qi <pkg>` |
| `pacman -Ql <pkg>` | `209 -Ql <pkg>` |
| `pacman -Qm` | `209 -Qm` |
| `pacman -Sc` | `209 gc` |
| `pacman -Scc` | `209 gc` (closures of pinned gens still kept) |
| `pacman -U <file>` | `209 import <file>` |
| `pacman.conf` | `2O9.nix` + `2O9.conf` |
| `/var/lib/pacman/local/` | `~/.local/state/2O9/` (or `/var/lib/2O9/`) |
| file paths `/usr/bin/foo` | `/nix/store/<hash>-foo-<ver>/bin/foo` (symlinked from `~/.local/bin/foo`) |

## From paru

You use paru as your AUR helper. 2O9's AUR workflow is a C port of
paru's, so the pipeline is the same: AUR RPC for search and info, git
clone of the PKGBUILD repo, PKGBUILD review diff, recursive dependency
resolution, makepkg, store add. The big differences are: chroot builds
are on by default, there is no `paru.conf` (use `2O9.conf`), MFlags live
in `2O9.conf`, and PGP key handling is automatic.

Under paru, the default is to run makepkg in your user environment. You
can opt into chroot builds via `paru.conf`, but it is off by default and
you have to install `devtools` and configure the chroot path. Under
2O9, chroot is on by default. The chroot lives at
`/var/lib/2O9/chroot` and is created automatically via `mkarchroot` on
the first AUR build. If `devtools` is not installed, 2O9 fails with a
clear "install devtools" message. To disable chroot for one build, pass
`--no-chroot`. To disable globally, set `[chroot] Enabled = no` in
`2O9.conf`.

What is the same: AUR search, AUR info, PKGBUILD review diff, recursive
dep resolution, `makepkg` invocation, `validpgpkeys` handling, MFlags
pass-through, GitFlags for the clone.

What is different: no `paru.conf`, no `paru -P` (stats), no `paru -G`
(getpkgbuild), no `paru -C` (clean), no paru's repo management. 2O9
puts AUR packages in the same content-addressed store as binary
packages, so they participate in GC, dedup, and binary cache
substitution uniformly.

| paru | 2O9 |
|---|---|
| `paru -S <pkg>` (AUR) | `209 <pkg> aur build` then add to `aur.packages` |
| `paru -Ss <term>` | `209 <term> aur search` |
| `paru -Si <pkg>` | `209 <pkg> aur info` |
| `paru -G <pkg>` | (no equivalent; clone happens automatically during build) |
| `paru -Gp <pkg>` | `209 <pkg> aur review` |
| `paru.conf` `[bin] MFlags` | `2O9.conf` `[bin] MFlags` |
| `paru.conf` `[chroot] Enabled` | `2O9.conf` `[chroot] Enabled` |
| `paru --noconfirm` | add `--noconfirm` to `MFlags` in `2O9.conf` |
| `paru --skipreview` | (always runs review; if you want to skip, pipe to `/dev/null`) |
| review diff against last build | same, automatic during `209 apply` |

The gotcha: 2O9's MFlags replace the default `-feA` entirely. If you
set `MFlags = --noconfirm`, you lose `-feA` and makepkg will prompt for
everything else. Include `-feA` (or whatever flags you want) explicitly
in `MFlags`. This matches paru's MFlags semantics but surprises users
coming from yay, where MFlags are appended.

## From Nix

You use Nix the package manager, not NixOS. You are comfortable with
`/nix/store/`, content-addressed paths, NAR format, profiles, and
generations. 2O9 will feel familiar. The store is the same idea, the
paths look the same, the symlink farm works the same way, generations
are the same concept. The big differences are: no derivations, no
nixpkgs, no flakes, packages come from Arch repos not nixpkgs, and the
evaluator is a C subset, not the full C++ nix.

What is the same: `/nix/store/<base32-hash>-<name>-<version>/` path
format, content-addressed paths, NAR serialisation (2O9 uses a
compatible variant), references graph, closure-aware GC, hardlink
dedup via `/nix/store/.links/`, profiles as symlinks, generations,
binary cache substitution with narinfo files, Ed25519-signed narinfos.

What is different: no derivations. A store path in 2O9 is just an
extracted `.pkg.tar.zst` from the Arch mirror. There is no `.drv` file,
no `nix-store --realise`, no build-time dependency graph in the store.
The dependency graph lives in the SQLite refs graph at
`~/.local/state/2O9/store.sqlite`, populated from `.PKGINFO`'s `depend
=` lines. Packages come from Arch repos, not nixpkgs, so the package
set is Arch's, not nixpkgs's. The evaluator is a C subset, not the full
C++ nix: no derivations, no `builtins.derivation`, no fetchers, no
flakes. Just enough to evaluate `2O9.nix`.

| Nix | 2O9 |
|---|---|
| `nix build` | (no equivalent; packages are pre-built by Arch) |
| `nix-env -iA nixpkgs.foo` | `209 foo install` |
| `nix-env -e foo` | `209 foo remove` |
| `nix-env -u` | `209 -Su` |
| `nix-collect-garbage -d` | `209 gc` |
| `nix-store --optimise` | `209 optimise` |
| `nix profile history` | `209 generations` |
| `nix profile rollback` | `209 <N> rollback` |
| `nix copy --to file:///cache` | `209 cache push <path>` |
| `nix copy --from file:///cache` | `209 cache pull <path>` |
| `nix-store -q --roots <path>` | `209 why <pkg>` |
| `nix-store -q --referrers <path>` | `209 why <pkg>` |
| `nix path-info` | (no direct equivalent; inspect via `209 -Qi`) |
| `default.nix` / `flake.nix` | `2O9.nix` |
| `~/.nix-profile/bin/` | `~/.local/bin/` |
| `/nix/var/nix/profiles/per-user/` | `~/.local/state/2O9/profile` |

The gotcha: 2O9's NAR format is a C reimplementation, not byte-for-byte
identical to Nix's. The serialisation is the same shape (canonical tree,
sorted directory entries, length-prefixed strings) but 2O9 uses
big-endian length prefixes without padding, where Nix uses a slightly
different framing. Cross-compatibility with real Nix binary caches
would need a compat flag. For now, 2O9 caches are for 2O9, and Nix
caches are for Nix. They do not interoperate.

## From NixOS

You use NixOS. You are wondering if 2O9 is a NixOS replacement. The
short answer is no. The longer answer is: 2O9 is a package manager
with generations on Arch. NixOS is a complete Linux distribution with
declarative system configuration, bootloader management, service
management as a DAG, user declaration, PAM, NSS, initrd, kernel
arguments, and a build farm of 100,000+ packages. 2O9 has none of
those.

What 2O9 does have that overlaps with NixOS: `/nix/store/` with
content-addressed paths, generations, rollback, a Nix-syntax config
file, and binary cache substitution. If what you wanted from NixOS was
"generations and rollback on a per-package basis", 2O9 gives you that
on top of Arch. If what you wanted was "declare my whole system in one
file and have the bootloader, initrd, kernel, users, services, and PAM
all derived from that declaration", 2O9 does not do that. Phase 4 of
the 2O9 roadmap (declarative system) is deferred.

If you want NixOS, use NixOS. It is good at what it does. If you want
Nix-style package management on Arch, with pacman's repos as the
package set, 2O9 is the right tool. The two are not in competition,
they are different points on a tradeoff curve. NixOS trades complexity
and package count for full declarativeness. 2O9 trades full
declarativeness for Arch's package set and simplicity.

| NixOS | 2O9 |
|---|---|
| `nixos-rebuild switch` | `sudo 209 apply` (then reboot for full effect) |
| `nixos-rebuild rollback` | `sudo 209 <N> rollback` (then reboot) |
| `nixos-rebuild list-generations` | `209 generations` |
| `/etc/nixos/configuration.nix` | `~/.config/2O9/home.nix` + `/etc/2O9/2O9.nix` |
| `nix.gc.automatic` | cron `209 gc` |
| `nix.optimise.automatic` | cron `209 optimise` |
| `services.openssh.enable = true;` | `services.sshd.enable = true;` |
| `boot.loader.grub.devices` | (not managed; configure grub yourself) |
| `users.users.alice` | (not managed; configure users yourself) |
| `systemd.services.foo` | (not managed; use `services.foo.enable`) |
| `environment.systemPackages` | `packages` |
| `nixpkgs.config.allowUnfree` | (no equivalent; Arch repos do not distinguish) |

The gotcha: do not try to use 2O9 and Nix on the same machine. Both
want `/nix/store/`, both want `~/.local/bin/` or `~/.nix-profile/bin/`
in `$PATH`, both have their own NAR format and their own narinfo
signing. They will fight. Pick one. If you want both package sets
(NixOS's and Arch's), run NixOS as the host and use a container or VM
for Arch, or vice versa. 2O9 is designed for Arch users who want
generations, not for NixOS users who want Arch packages.
