# Tutorial: Your first hour with 2O9

This is a linear walkthrough from a fresh clone to a working 2O9-managed
system. By the end you will have a config file, one Arch package installed,
one AUR package built, a couple of generations to roll between, and a
cleaned-up store. Each step assumes you finished the previous one.

Commands are prefixed with `$` (run as your user) or `#` (run as root,
typically via `sudo`). Copy the whole line, the prefix is just convention.

## 1. Install and build

2O9 ships as source. You need a C compiler, `make`, and a handful of dev
headers. On a stock Arch system the `base-devel` group plus a few extras
covers everything.

```sh
$ sudo pacman -S --needed base-devel libcurl libarchive openssl \
    libgpgme sqlite libxml2 libsodium
```

Clone the repo and build:

```sh
$ git clone https://github.com/Rui-727/2O9.git
$ cd 2O9
$ make
$ sudo make install
```

`make` builds the `209` binary plus three unit-test binaries. `make install`
copies `209` to `/usr/bin/209` (override with `make install PREFIX=/usr/local`).
Verify the binary is on your `$PATH` and prints a version:

```sh
$ 209 -V
209 0.5.0
```

If that prints a version, you are done with this step. If it fails, check
that all the dev packages are actually installed, the most common miss is
`libgpgme-dev` (called `gpgme` on Arch). You can build without `libsodium`
and 2O9 will fall back to OpenSSL Ed25519, but the other deps are required.

## 2. First config

Run `209 init` to create a starter config. It writes to
`~/.config/2O9/2O9.nix` and refuses to clobber an existing file.

```sh
$ 209 init
created: /home/you/.config/2O9/2O9.nix
```

Open the file in your editor. The starter has `vim`, `curl`, `git`, `htop`
listed under `packages`. Trim that down so we can watch 2O9 install exactly
what we ask for. Replace the `packages` list with a single entry, `cpufetch`,
which is small, builds fast, and was used during 2O9 testing. The top of the
file should look like this:

```nix
{ config, ... }:
{
  packages = [
    "cpufetch"
  ];

  aur.packages = [
    # "google-chrome"
  ];

  aur.build.profile = "safe";
  aur.build.jobs = "auto";

  pacman = {
    options = {
      SigLevel = "Required DatabaseOptional";
      ParallelDownloads = 5;
    };
    repos = {
      core     = { server = "https://geo.mirror.pkgbuild.com/core/os/x86_64"; };
      extra    = { server = "https://geo.mirror.pkgbuild.com/extra/os/x86_64"; };
      multilib = { server = "https://geo.mirror.pkgbuild.com/multilib/os/x86_64"; };
    };
  };
  # ... rest of file
}
```

Each field does one job. `packages` is the list of binary packages to pull
from the Arch repos. `aur.packages` is the list of AUR packages to build
from source. `aur.build.profile` sets the CFLAGS profile injected into
makepkg, `safe` means `-O2 -pipe`, `native` means `-march=native -O3 -pipe`.
`pacman.options` maps to pacman.conf options, with `SigLevel` controlling
signature verification. `pacman.repos` lists the sync DBs and their mirror
URLs.

## 3. First sync

Before 2O9 can install anything, it needs the repo databases. `209 sync`
(also `209 -Sy`) downloads the `core`, `extra`, and `multilib` DB files from
the mirrors listed in `pacman.repos`. It uses lib2O9's `alpm_db_update`
under the hood, the same code pacman uses, so signature verification,
delta downloads, and parallel fetches all work.

```sh
$ 209 sync
209: syncing core...
209: syncing extra...
209: syncing multilib...
```

The sync DBs land in `~/.local/state/2O9/sync/` as `core.db`,
`extra.db`, and `multilib.db`. They are standard pacman sync DBs, plain
tarballs of parsed package metadata. `209 sync` is idempotent: running it
again only re-downloads a DB if its modification time changed on the
mirror.

You only need to sync when you want to see new packages or new versions.
`209 apply` does not sync for you. A common workflow is `209 -Sy && sudo
209 apply`, mirroring `pacman -Syu`.

## 4. First apply

Now make the system match the config. `209 apply` needs root because it
writes to `/nix/store/` and `/var/lib/2O9/`. Run it via `sudo`. 2O9
respects `SUDO_USER`, so it will still read your `~/.config/2O9/2O9.nix`
and write to your `~/.local/state/2O9/` generation DB, not root's.

```sh
$ sudo 209 apply
209: evaluating 2O9.nix...
209: reconciling against generation 0
209: install plan:
    + cpufetch-1.07-1
209: downloading cpufetch-1.07-1-x86_64.pkg.tar.zst...
209: verifying signature...
209: extracting to /nix/store/0v4v8nxq2cfk7kbhcr5d4s5d9lqzrqyr-cpufetch-1.07-1/
209: committing generation 1
209: rebuilding symlink farm
209: done
```

What just happened, step by step. 2O9 evaluated `2O9.nix` to a JSON
manifest, diffed it against the current generation (generation 0, the
empty default), and produced an install plan: one package to add. It then
downloaded the package from the configured Arch mirror, verified the
detached `.sig` against the pacman keyring via gpgme, extracted the
tarball into a temp dir under `/nix/store/.tmp/`, and `rename()`'d the
result to its final content-addressed path.

The final path is `/nix/store/<base32-hash>-cpufetch-1.07-1/`. The hash
is computed from the NAR serialisation of the extracted tree, so two
machines that build the same package get the same path. 2O9 committed a
new generation (ID 1) recording that this path is the live root for
`cpufetch`, then rebuilt the symlink farm so `~/.local/bin/cpufetch`
points into the store.

Verify it works:

```sh
$ cpufetch
# prints CPU info in a table
$ ls -l ~/.local/bin/cpufetch
lrwxrwxrwx 1 you you 65 Aug 1 12:00 /home/you/.local/bin/cpufetch -> /nix/store/0v4v8...cpufetch-1.07-1/bin/cpufetch
```

## 5. First rollback

Now let's roll a generation back and forth. List what you have:

```sh
$ 209 generations
  ID  Packages  Pinned  Changes
  ──  ────────  ──────  ───────
    1        1  no     +1 -0 ~0  ← current
```

You have one generation, ID 1, marked current. Edit your config to remove
`cpufetch` from the `packages` list:

```nix
  packages = [
    # "cpufetch"
  ];
```

Apply again:

```sh
$ sudo 209 apply
209: install plan:
    - cpufetch-1.07-1
209: committing generation 2
209: rebuilding symlink farm
```

Notice it did not delete the store path. The symlink farm just no longer
links `~/.local/bin/cpufetch`. The store path is still there, kept alive
by generation 1's root set, so a rollback can bring it back instantly.

Confirm the binary is gone, then list generations:

```sh
$ cpufetch
bash: cpufetch: command not found
$ 209 generations
  ID  Packages  Pinned  Changes
  ──  ────────  ──────  ───────
    1        1  no     +1 -0 ~0
    2        0  no     +0 -1 ~0  ← current
```

Now roll back to generation 1:

```sh
$ sudo 209 1 rollback
209: switching current generation 2 -> 1
209: rebuilding symlink farm
```

Verify `cpufetch` works again:

```sh
$ cpufetch
# prints CPU info
$ 209 generations
  ID  Packages  Pinned  Changes
  ──  ────────  ──────  ───────
    1        1  no     +1 -0 ~0  ← current
    2        0  no     +0 -1 ~0
```

For a full system rollback you would reboot here. 2O9 just swaps the
profile symlink and rebuilds the symlink farm. Already-running processes
keep their old file descriptors. A reboot is what gets systemd to start
the generation's enabled units and what gets your shell to see the new
binaries on its next exec.

## 6. First AUR build

So far we only used binary packages from the Arch repos. Now build one
from the AUR. Add `yt-dlp` to `aur.packages`:

```nix
  aur.packages = [
    "yt-dlp"
  ];
```

Then apply:

```sh
$ sudo 209 apply
209: AUR build plan:
    yt-dlp (1 dep)
209: cloning yt-dlp from aur.archlinux.org...
209: reviewing PKGBUILD (diff against previous build)
209: creating chroot at /var/lib/2O9/chroot
209: importing PGP key 0x... (if needed)
209: running makechrootpkg -r /var/lib/2O9/chroot
209: built yt-dlp-2024.08.06-1-x86_64.pkg.tar.zst
209: extracting to /nix/store/abc123-yt-dlp-2024.08.06-1/
209: committing generation 3
209: rebuilding symlink farm
```

The AUR build pipeline is: resolve deps recursively, clone the PKGBUILD
repo from `aur.archlinux.org`, run `aur review` to show you a diff of the
PKGBUILD against the last build (or against an empty tree if first build),
create a clean chroot at `/var/lib/2O9/chroot` via `mkarchroot`, run
`makechrootpkg` inside it which calls `makepkg` with the CFLAGS from
`aur.build.profile`, parse the resulting `.pkg.tar.zst`'s `.PKGINFO` for
the final name and version, add it to the store.

PGP keys listed in `validpgpkeys` in the PKGBUILD are auto-imported via
`gpg --recv-keys`. 2O9 prompts you with `[y/N]` before importing, so you
get a chance to abort if a key looks wrong. The chroot build needs the
`devtools` package installed. If it is missing, 2O9 fails with a clear
"install devtools" message. You can disable the chroot per-build with
`--no-chroot`, but that runs makepkg in your user environment, which is
not recommended for untrusted PKGBUILDs.

Verify:

```sh
$ yt-dlp --version
2024.08.06
```

## 7. First garbage collection

After rolling back and forth a few times, your store has paths that no
generation references anymore. `209 gc` deletes those. It walks the
references graph (SQLite at `~/.local/state/2O9/store.sqlite`) and
computes the closure of all generations' root paths, plus any pinned
generations' root paths. Anything outside that closure is dead and gets
deleted.

```sh
$ sudo 209 gc
209: computing live set...
209: 47 live paths
209: 3 dead paths:
    /nix/store/xyz-some-old-version/
    /nix/store/abc-yt-dlp-2024.07.01-1/
    /nix/store/def-cpufetch-1.06-1/
209: deleting...
209: freed 124.3 MB
```

If you want to protect a generation from GC, pin it:

```sh
$ sudo 209 1 pin
209: pinned generation 1
```

Pinned generations have a `.pinned` marker file in their generation
directory. GC skips them when computing what's safe to delete. Use this
for known-good rollback points you want to keep around indefinitely.

After GC, run `209 optimise` to hardlink-dedup the surviving store
paths. It walks every regular file, hashes it with SHA-256, and links
identical files into `/nix/store/.links/<sha256>`. Two packages shipping
the same 50 MB locale file collapse to 50 MB on disk instead of 100 MB.

```sh
$ sudo 209 optimise
209: scanning /nix/store/...
209: hashed 12483 files
209: deduped 412 files (saved 89.2 MB)
```

You can also do both in one pass:

```sh
$ sudo 209 gc --optimise
```

## 8. First substituter (optional)

If you have two Arch machines, you can set up one as a build-and-publish
machine and have the other pull from its cache. This saves bandwidth and
guarantees the two machines run byte-identical binaries.

On machine A (the publisher), generate an Ed25519 keypair:

```sh
$ 209 keygen
public key (add to extra.nix substituters.PublicKey on subscribers):
  r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=

cache.example.com-1:r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=:TakyhFMCwVcOjdPUJurMrgEQeyuuGukyL+/wWYoCFQ8=
```

Save the second line (the `name:public:secret` triple) to a file with
mode 0600. Add to `/etc/2O9/extra.nix`:

```nix
substituters = {
  URLs = [ "https://cache.example.com" ];
  SigningKey = "/etc/2O9/secret-key";
  KeyName = "cache.example.com-1";
};
```

Push a path and its closure:

```sh
# 209 cache push /nix/store/0v4v8nxq2cfk7kbhcr5d4s5d9lqzrqyr-cpufetch-1.07-1
209: computing closure...
209: pushing 1 path
209:   /nix/store/0v4v8...-cpufetch-1.07-1/
209:     narinfo: signed with cache.example.com-1
209:     nar: 1.2 MB uploaded
```

On machine B (the subscriber), add the public key to its
`/etc/2O9/extra.nix`:

```nix
substituters = {
  URLs = [ "https://cache.example.com" ];
  PublicKey = "r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=";
  AllowUnsigned = false;
};
```

Now when machine B runs `209 -S cpufetch`, 2O9 first asks
`https://cache.example.com/0v4v8....narinfo`. If found and the signature
verifies against the configured `PublicKey`, it downloads the NAR,
decompresses it, streams it into the store path, and never touches the
Arch mirror. If the cache does not have the path, or the signature fails,
2O9 falls through to the mirror as if no cache were configured.

You are done. You now have a working 2O9 setup: a config, a binary
package, an AUR build, multiple generations to roll between, a clean
store, and optionally a binary cache. The [Cookbook](./COOKBOOK.md) has
recipes for specific tasks, and the [Use cases](./USE_CASES.md) doc has
longer writeups of how 2O9 fits real workflows.
