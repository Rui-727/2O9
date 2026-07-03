# Use Cases

Longer writeups of how 2O9 fits specific real-world scenarios. Each one
covers the problem, why 2O9 helps, what the setup looks like, what the
day-to-day looks like, and the gotchas to watch for. These are not
tutorials, they assume you have read the [Tutorial](./TUTORIAL.md) and
know the basic commands.

## 1. Developer workstation

You are a developer with two or three Arch machines: a desktop, a laptop,
maybe a work machine. You want the same tools on all of them, you want
to know exactly what is installed, and you want to be able to roll back
a broken upgrade without thinking too hard. You do not want to install
NixOS because Arch works fine and you would rather not repartition.

2O9 gives you content-addressed packages and generations on top of Arch.
Your `~/.config/2O9/home.nix` lives in a git repo alongside your
dotfiles. On a new machine, you clone the dotfiles repo, symlink
`home.nix` into place, run `209 sync && sudo 209 apply`, and you have
the same set of packages as every other machine.

Day to day, you edit `home.nix` to add or remove a package, then `209
apply`. If an upgrade breaks something, you `209 generations`, find the
last good one, `209 <N> rollback`, and reboot. The old kernel or driver
is still in `/nix/store/`, untouched by GC until you explicitly reap
it. You can keep multiple pinned generations around for "before the
kernel upgrade", "before the graphics driver upgrade", and "current" and
flip between them freely.

The gotcha: 2O9 manages userspace packages and `systemctl enable`, not
the whole OS. The kernel itself is just another package, but the
bootloader config is not declarative. If you upgrade the kernel and the
new one panics, rolling back the generation brings the old kernel
package back, but you still need to reboot into it. 2O9 does not touch
GRUB or systemd-boot. Pin the pre-upgrade generation before rebooting
into a new kernel so GC does not delete it under you.

## 2. Multi-machine Arch fleet

You run three or more Arch machines: a desktop, a build server, a media
box, maybe a VPS. Bandwidth is the bottleneck: every machine downloading
the same 200 MB package update from the mirror is wasteful, and you want
guaranteed byte-identical binaries across the fleet.

Pick one machine as the publisher. It has the build tools, the signing
key, and a public HTTPS endpoint. Configure `/etc/2O9/2O9.conf` on it
with `[substituters] URLs = https://cache.example.com` plus the signing
key. Run `209 sync && sudo 209 apply` there first, then `209 cache push
<nix-store-path>` for the closure of the apply. Or, run a cron job that
pushes the current generation's closure every night.

On every other machine, configure `[substituters] URLs =
https://cache.example.com` with the publisher's public key. The next
`209 -Su` on those machines hits the cache first. If the path is there
and the Ed25519 signature verifies, the NAR is downloaded and streamed
into the store. The Arch mirror is only consulted for paths the cache
does not have.

Day to day, the sysadmin edits `2O9.nix` on the publisher, applies it,
pushes the new closure to the cache, then runs `209 -Su` on the
subscribers. Bandwidth across the WAN is one copy of each new package,
not N copies. All machines run the same binaries because they share the
same content-addressed paths. The gotcha: this only works for packages
that build identically. AUR packages with `profile = "native"` will
produce different hashes on machines with different CPUs. Use
`aur.build.profile = "safe"` on the publisher if you want AUR packages
to be shareable. Configure `aur.build.jobs = "auto"` is fine, that does
not affect the output hash.

## 3. AUR power user

You build a lot of AUR packages. With `paru` or `yay`, every time you
`-Syu`, every AUR package rebuilds from scratch even if nothing
changed. The build takes 20 minutes for `ffmpeg-full`, you do it weekly,
and after a year you are tired of it.

2O9's chroot builds and content-addressed store means rebuilding only
happens when the inputs actually change. The store path is
`/nix/store/<hash>-<name>-<version>/` where the hash is computed from
the NAR serialisation of the extracted tree. If the PKGBUILD, sources,
CFLAGS, and makepkg version are all the same, the hash is the same, and
2O9 just reuses the existing path.

Day to day, you list your AUR packages in `aur.packages` in `2O9.nix`.
On `209 -Su`, 2O9 clones each PKGBUILD, runs `aur review` to diff
against the last build, and only calls `makechrootpkg` if the source or
PKGBUILD actually changed. Even if the version bumps, if the package
binary is byte-identical (rare but possible for pure-data packages), 2O9
reuses the path. Your `ffmpeg-full` build takes 20 minutes once, then
zero minutes for every subsequent `209 -Su` until upstream changes
something.

The gotcha: PGP key handling is automatic but interactive. If a
PKGBUILD's `validpgpkeys` lists a key you do not have, 2O9 pauses and
asks `import key 0xABC...? [y/N]`. In a cron-driven `-Su`, this hangs.
Either import the key manually once and it stays in your keyring, or set
up a keyring pre-populated with the common AUR maintainer keys. Also
worth knowing: chroot builds via `makechrootpkg` need `devtools`
installed, and the chroot itself at `/var/lib/2O9/chroot` takes about
500 MB. It is reused across builds, so the cost is paid once.

## 4. Rollback-friendly system

You upgrade your kernel or your graphics driver, reboot, and the system
does not come up properly. With plain pacman, you would be digging
through `/var/cache/pacman/pkg/` to find the old `.pkg.tar.zst`, hoping
you did not clean the cache, then `pacman -U` it, then reboot and pray.
It works, but it is stressful and slow.

With 2O9, the workflow is: from a TTY or a recovery shell, log in,
`209 generations`, find the ID of the last working generation, `sudo
209 <N> rollback`, reboot. The whole thing takes 30 seconds. The old
kernel and driver packages are still in `/nix/store/` because GC
preserves the closure of every generation, not just the current one.

The setup is: before any risky upgrade, run `209 generations` to see
the current ID, then `sudo 209 <N> pin` so GC cannot reap that
generation's store paths even if you forget about it for a month. Apply
the upgrade. If it works, great, you can unpin in a week when you are
sure. If it does not, `sudo 209 <pinned-id> rollback` and reboot.

Day to day, this is invisible. You `209 -Su` like normal. The rollback
optionality is just there when you need it. The gotcha: rolling back
does not roll back `/etc/` config files. If an upgrade installed a new
`/etc/foo.conf` and you edited it, rolling back the generation will
re-symlink the old version's `/etc/foo.conf` into the store, but any
edits you made to the file (the real one, not the symlink) survive. This
is usually what you want, but it can surprise you. Also, kernel
rollbacks require the bootloader to still know about the old kernel. 2O9
does not manage the bootloader, so you need to either keep the old
kernel in your bootloader entries manually or use a bootloader like
`systemd-boot` that auto-detects kernels in `/boot/`.

## 5. Air-gapped machine

You have a machine with no internet. It might be a secure workstation,
might be a build machine behind a corporate firewall, might be a
machine in a lab. You want packages on it. Walking them over by hand
with a USB stick and `pacman -U` works but is tedious and you lose
dependency resolution.

Set up a normal Arch machine elsewhere as the publisher, with a binary
cache configured. Push the closure you want to a `file://` URL pointing
at a directory on a USB stick:

```ini
[substituters]
URLs = file:///mnt/usb/cache
SigningKey = /etc/2O9/secret-key
KeyName = airgap-1
```

```sh
$ sudo 209 cache push /nix/store/<hash>-neovim-0.10.0
```

Then walk the USB stick over to the air-gapped machine. Configure it
with:

```ini
[substituters]
URLs = file:///mnt/usb/cache
PublicKey = <from publisher's 209 keygen>
AllowUnsigned = no
```

Run `209 -S neovim` on the air-gapped machine. 2O9 reads the narinfo
from the USB stick, verifies the Ed25519 signature, streams the NAR into
the store. No internet needed. For a system that needs to be kept
completely offline, this is the only safe way to install packages: the
USB stick is a single trust root, signed by a key you control, and
nothing on the air-gapped machine ever trusts the internet.

The gotcha: the USB stick needs the full closure, not just the package
you want. `209 cache push` does this for you (it walks the refs graph),
but if you forget and only copy the top-level `.narinfo` and `.nar.xz`,
the install will fail with "missing reference". Also, every package on
the air-gapped machine needs to have been pushed first. There is no
fallback to the mirror, because there is no mirror. Plan ahead.

## 6. CI build worker

You run CI for a project. Each job needs a known-good set of packages
installed: a specific compiler version, a specific `cmake`, a specific
`ninja`. You want jobs to start from a clean state, but you also do not
want to spend three minutes reinstalling packages on every job.

Snapshot the desired state once: install the toolchain, run `209 lock
--export ci-toolchain.lock`, commit the lockfile to the repo. At the
start of each CI job, the worker runs `209 lock --import
ci-toolchain.lock`. 2O9 checks which paths are already in the store
(from the previous job), fetches only the missing ones from the
configured cache, verifies every NAR hash matches the lockfile, and
commits a new generation. The job runs against exactly the toolchain the
lockfile specifies, every time, on every worker.

The setup: workers share a binary cache. The cache is populated by a
nightly job that runs `209 -Su` on a reference machine and `209 cache
push` on the resulting closure. Workers never need to download from the
Arch mirror, so the mirror's rate limits do not matter. A worker that
gets re-imaged can be back to running jobs in minutes: install 2O9,
configure the cache URL and public key, run `209 lock --import`, done.

The gotcha: the lockfile is large (one line per store path, often
thousands of lines). Committing it to the repo is fine, but review the
diff carefully when you regenerate it, you want to know when a path
changed. Also, the import is atomic per generation, not per path: if the
worker crashes mid-import, the partial generation is not committed and
the next run starts fresh. This is good. The bad part is that already-
fetched paths are kept, so the next run is faster, but you should still
monitor disk usage on workers. Run `209 gc` weekly to keep things tidy.

## 7. Untrusted software review

You found a weird binary. Maybe a coworker sent it to you. Maybe you
downloaded a `.pkg.tar.zst` from a sketchy forum. Maybe an AUR package
has a new maintainer and you want to be careful. You want to run it, but
you do not want it to touch your real filesystem or phone home.

2O9 has two tools for this. Trakker is the slow, thorough one: ptrace
on every syscall, full JSON trace of file I/O, network connections,
process forks, mmap activity. Debag is the fast one: seccomp-bpf for
the fast path (most syscalls allowed directly, nanosecond overhead),
ptrace only for syscalls that need argument inspection (execve,
connect, open, mount). Debag also does static analysis: it scans the
ELF symbol table to figure out what syscalls the binary probably uses,
then builds a seccomp filter from that.

For a quick look at what a binary does:

```sh
$ 209 debag --static-scan -- ./suspicious-binary
```

Debag prints the syscall list and the seccomp filter it would apply,
without actually running the binary. If the list includes `ptrace`,
`mount`, `keyctl`, or `bpf`, that is suspicious. For an AUR package
build, wrap makepkg:

```sh
$ 209 trakker --no-net --redirect-writes /tmp/aur-build -- \
    makechrootpkg -r /var/lib/2O9/chroot
```

Trakker blocks all network, redirects all writes into `/tmp/aur-build`,
and produces a JSON trace. If the PKGBUILD tries to `curl` something
during build, it fails. If it writes to `~/.ssh/authorized_keys`, the
write lands at `/tmp/aur-build/home/you/.ssh/authorized_keys` instead,
and you can read the JSON trace to see exactly what it tried to do.

The gotcha: trakker's `--redirect-writes` only catches writes that go
through the `open`/`openat` syscall family. A binary that opens a file
descriptor via `memfd_create` and then writes to it will not be
redirected. Debag's static scan only sees symbols, not runtime behavior,
so dynamically-loaded code (e.g. `dlopen`) is invisible to it. For
truly untrusted software, run inside a VM as well. Trakker and Debag
are first-line defenses, not a complete sandboxing solution.
