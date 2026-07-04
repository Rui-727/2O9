# Cookbook

Recipes for common tasks. Each one is self-contained: the problem, the
commands or config to solve it, and a sentence or two on why it works.
Read top to bottom or jump to the recipe you need.

Commands are prefixed with `$` (run as your user) or `#` (run as root).
Most mutating operations (`apply`, `gc`, `optimise`, `rollback`) require
root because they write to `/nix/store/` or the generation DB.

## 1. Install a single package temporarily

You want to try `ripgrep` without committing to keeping it. Do not edit
`2O9.nix` yet, just install it imperatively:

```sh
$ sudo 209 ripgrep install
```

The package lands in the current generation. The next `209 apply` will
flag it for removal because it is not in the config. If you want to keep
it, add `ripgrep` to `packages` in `2O9.nix` before the next apply. This
matches `pacman -S ripgrep` behavior, just with a generation recorded.

## 2. Install a package permanently

Edit `/nix/config/<user>.nix` (or `/nix/config/2O9.nix` for system-wide)
and add the package to the `packages` list:

```nix
  packages = [
    "vim"
    "ripgrep"
  ];
```

Then apply:

```sh
$ sudo 209 apply
```

2O9 evaluates the config, diffs against the current generation, sees
`ripgrep` is new, installs it, and commits a new generation. From now on,
every `209 apply` will keep `ripgrep` installed. To remove it, delete the
line and apply again.

## 3. Remove a package

Either delete it from `packages` in `2O9.nix` and run `209 apply`, or
remove it imperatively without touching the config:

```sh
$ sudo 209 htop remove
```

Both commit a new generation without the package. The first form is
permanent (the next apply will keep it removed). The second form is
temporary (if `htop` is still in the config, the next apply will
reinstall it). The store path itself is not deleted, only the symlink
farm entry, so a rollback can bring it back instantly.

## 4. Upgrade all packages

```sh
$ 209 sync
$ sudo 209 apply
```

`209 apply` always reconciles against the latest sync DBs, so an upgrade
is just "sync then apply". `209 -Su` is the same thing in one flag. New
versions of packages already in your config get downloaded, extracted to
fresh store paths, and a new generation is committed. Old versions stay
in the store until GC reaps them.

## 5. Search for a package in repos

```sh
$ 209 ffmpeg search
extra/ffmpeg 7.0.2-1
    Complete, cross-platform solution to record, convert and stream audio and video
extra/ffmpeg4 4.4.4-1
    Old version of ffmpeg kept around for compatibility
```

This searches the sync DBs in `~/.local/state/2O9/sync/`. If no local
match is found, 2O9 falls back to AUR search. The `pacman -Ss ffmpeg`
flag does the same thing.

## 6. Search for a package in AUR

```sh
$ 209 ffmpeg aur search
aur/ffmpeg-full 7.0.2-1 (+234 5.67%)
    Complete solution to record, convert and stream audio and video (full build)
aur/ffmpeg-decklink 7.0.2-1 (+12 0.30%)
    FFmpeg with Blackmagic DeckLink support
```

This hits `https://aur.archlinux.org/rpc/?v=5&type=search&arg=ffmpeg`
via libcurl and prints results. The numbers in parens are AUR vote count
and popularity. To build one of these, see recipe 7.

## 7. Build an AUR package with custom CFLAGS

Add the package to `aur.packages` in `2O9.nix`, and override CFLAGS per
package under `aur.build`:

```nix
  aur.packages = [ "ffmpeg-full" ];

  aur.build = {
    profile = "native";        # default: -march=native -O3 -pipe
    jobs = "auto";             # auto = nproc

    ffmpeg-full = {
      CFLAGS = "-march=native -O3 -pipe -fno-plt";
    };
  };
```

Then apply:

```sh
$ sudo 209 apply
```

2O9 clones the PKGBUILD, imports PGP keys from `validpgpkeys`, builds in
a chroot via `makechrootpkg` with your CFLAGS injected into makepkg's
environment. The per-package `CFLAGS` overrides the global `profile`.
The resulting `.pkg.tar.zst` is added to the store as
`/nix/store/<hash>-ffmpeg-full-7.0.2-1/`.

## 8. Review a PKGBUILD before building

```sh
$ 209 yt-dlp aur review
209: cloning yt-dlp from aur.archlinux.org...
209: diff against /home/you/.cache/2O9/aur/yt-dlp/last-build
--- PKGBUILD (last-build)
+++ PKGBUILD (current)
@@ -3,7 +3,7 @@
 pkgname=yt-dlp
-pkgver=2024.07.01
+pkgver=2024.08.06
 pkgrel=1
 source=("yt-dlp-${pkgver}.tar.gz::https://github.com/yt-dlp/yt-dlp/archive/refs/tags/${pkgver}.tar.gz")
 sha256sums=('abc...')
```

2O9 clones the PKGBUILD repo, diffs it against the last build (or
against an empty tree on first build), and shows you the result in your
`$PAGER`. If anything looks wrong (suspicious `curl | bash`, weird
`source=`, changed `validpgpkeys`), abort and do not build. The actual
`209 apply` build step also runs review automatically, but running it
ahead of time lets you decide before you commit.

## 9. List what changed between two generations

```sh
$ 209 diff 3 5
=== generation 3 -> 5 ===
added:
  + neovim-0.10.0-1
  + ripgrep-14.1.0-1
removed:
  - vim-9.1.0-1
changed:
  ~ htop: 3.3.0-1 -> 3.4.0-1
```

Reads `manifest.json` and `diff.json` for each generation, prints the
added/removed/changed sets. Useful for "what did I do last week" or for
deciding whether a rollback is safe.

## 10. Find out why a package is installed

```sh
$ 209 why openssl
openssl is installed because:
  - it is in /nix/config/2O9.nix: packages
  - curl depends on it (closure)
  - git depends on it (closure)
```

2O9 walks the generation index and the references graph to find every
root that reaches the named package. If the only entries are "depends on
it (closure)", the package is a transitive dep, not declared directly,
and you can remove it by removing its parent. If the only entry is "in
2O9.nix", you can remove it by editing the config.

## 11. Pin a generation so GC doesn't delete it

```sh
$ sudo 209 1 pin
209: pinned generation 1
```

This creates a `.pinned` marker file in the generation's directory. GC
treats pinned generations as live roots, so their store paths survive
even after a newer generation makes them redundant. To unpin, remove the
file:

```sh
# rm /var/lib/2O9/generations/1/.pinned
# (or for user scope: rm ~/.local/state/2O9/generations/1/.pinned)
```

Use this for known-good rollback points, like the generation before a
kernel upgrade you might need to undo.

## 12. Roll back to a previous generation

```sh
$ 209 generations
  ID  Packages  Pinned  Changes
  ──  ────────  ──────  ───────
    1       42  yes    +42 -0 ~0
    2       43  no     +1 -0 ~0
    3       41  no     +0 -2 ~0  ← current
$ sudo 209 1 rollback
209: switching current generation 3 -> 1
209: rebuilding symlink farm
```

2O9 repoints the current-generation symlink to generation 1 and
rebuilds the symlink farm. The store is untouched. For a full system
rollback, reboot afterward so systemd starts the generation's enabled
units and your shell sees the new binaries.

## 13. Clean up disk: GC plus dedup

```sh
$ sudo 209 gc --optimise
209: computing live set...
209: 312 live paths
209: 14 dead paths
209: deleting...
209: freed 487.1 MB
209: scanning /nix/store/...
209: hashed 31845 files
209: deduped 1247 files (saved 312.4 MB)
```

`gc` walks the refs graph and deletes anything not reachable from any
(pinned or current) generation. `--optimise` then runs hardlink dedup:
walks every surviving regular file, SHA-256s it, and links identical
files into `/nix/store/.links/<sha256>`. Two packages shipping the same
file collapse to one inode. You can also run the two steps separately
with `209 gc` then `209 optimise`.

## 14. Set up a binary cache between two machines

On the publisher (machine A):

```sh
# 209 keygen > /tmp/keygen.out
# head -1 /tmp/keygen.out
r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=
# tail -1 /tmp/keygen.out > /etc/2O9/secret-key
# chmod 0600 /etc/2O9/secret-key
```

Add to `/nix/config/extra.nix` on machine A:

```nix
subs = {
  personal = {
    URLs = [ "https://cache.example.com" ];
    SigningKey = "/etc/2O9/secret-key";
    KeyName = "cache.example.com-1";
  };
};
```

Push a path and its closure:

```sh
# 209 cache push /nix/store/0v4v8...-cpufetch-1.07-1
```

On the subscriber (machine B), add to `/nix/config/extra.nix`:

```nix
subs = {
  personal = {
    URLs = [ "https://cache.example.com" ];
    PublicKeys = [ "r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=" ];
    AllowUnsigned = false;
  };
};
```

Now `209 -S cpufetch` on machine B fetches the NAR from
`https://cache.example.com/0v4v8....narinfo`, verifies the Ed25519
signature against any of the listed `PublicKeys`, streams it into the
store. The Arch mirror is only consulted if the cache is missing the
path or the signature fails.

## 15. Run an untrusted binary in a sandbox

```sh
$ 209 trakker --no-net --redirect-writes /tmp/sandbox -- ./suspicious-binary
```

Trakker runs the command under ptrace, intercepts every syscall, and
logs file I/O, network connections, and process forks as JSON. With
`--no-net`, all `connect`, `sendto`, `recvfrom` calls fail with `EACCES`.
With `--redirect-writes /tmp/sandbox`, any write to `/etc/foo` gets
silently redirected to `/tmp/sandbox/etc/foo` instead. The binary thinks
it modified your system. Your real system is untouched. The trace JSON
ends up in your working directory.

## 16. Build an AUR package in a sandbox

```sh
$ 209 debag --no-net --redirect-writes /tmp/aur-build -- \
    makechrootpkg -r /var/lib/2O9/chroot
```

Debag is the fast sandbox: seccomp-bpf for the fast path (most syscalls
allowed directly, nanosecond overhead), ptrace only for syscalls that
need argument inspection. The target runs at about 90% native speed
instead of 20% with pure ptrace. Wrap any makepkg invocation to catch
writes before they hit your filesystem. Combine with `--no-net` to force
makepkg to use only local sources (will fail if the PKGBUILD needs
network), or with `--allow-net port=443` to allow only HTTPS.

## 17. Block network for a build

```sh
$ 209 trakker --no-net -- makepkg -f
```

Or with debag (faster):

```sh
$ 209 debag --no-net -- makepkg -f
```

`--no-net` blocks all network syscalls (`connect`, `sendto`, `recvfrom`,
`socket` for `AF_INET`/`AF_INET6`). Any network call fails with `EACCES`.
This forces makepkg to fail loudly if a PKGBUILD tries to phone home
during build. If a build genuinely needs network (e.g. to download
sources), `--allow-net port=443` lets only port 443 through.

## 18. Configure a different mirror

Edit `pacman.repos` in `2O9.nix`:

```nix
  pacman.repos = {
    core     = { server = "https://mirror.umd.edu/archlinux/core/os/x86_64"; };
    extra    = { server = "https://mirror.umd.edu/archlinux/extra/os/x86_64"; };
    multilib = { server = "https://mirror.umd.edu/archlinux/multilib/os/x86_64"; };
  };
```

Then re-sync:

```sh
$ 209 sync
```

Any URL libalpm accepts works. The `$arch` and `$repo` variables are
expanded at download time, so you can also write
`"https://mirror.umd.edu/archlinux/$repo/os/$arch"` and let libalpm
substitute. After changing mirrors, your existing sync DBs are
overwritten with the new mirror's copies.

## 19. Disable signature verification (for testing only)

In `2O9.nix`:

```nix
  pacman.options.SigLevel = "Never";
```

Or for one install, set the env var (if you have patched the code to
honor it, which stock 2O9 does not, so the config option is the
supported path). With `SigLevel = "Never"`, 2O9 skips fetching `.sig`
files and skips `alpm_pkg_load` verification. This is dangerous: any
MITM can swap in a malicious package. Use it only on an air-gapped
machine with hand-verified packages, or for testing a broken mirror that
has not published sigs yet. Switch back to `Required DatabaseOptional`
as soon as you are done.

## 20. Reproduce an exact system state on another machine

On machine A:

```sh
$ 209 lock --export system.lock
209: exported 312 paths from generation 7
```

Copy `system.lock` to machine B. On machine B:

```sh
$ 209 lock --import system.lock
209: reading lockfile...
209: 312 paths to match
209: 287 already present
209: 25 to fetch
209:   fetching /nix/store/abc...-neovim-0.10.0/...
209: committing generation 1
```

The lockfile records every store path in the source generation, with
hashes. The import fetches missing paths from configured substituters or
the Arch mirror, verifies each NAR hash matches what the lockfile
expects, and commits a new generation. The two machines now have
byte-identical user packages. This is the closest thing 2O9 has to
NixOS-style reproducibility without going full NixOS.
