# phase-4 branch

This branch holds Phase 4 work: declarative system management. It is
NOT merged into main. The two branches diverge here.

## What Phase 4 covers

Phase 4 turns 2O9 from a package manager with generations into
something closer to NixOS: the config declares the whole system, not
just packages.

Scope:

- **Services as a DAG.** Today `services.sshd.enable = true` translates
  to `systemctl enable sshd`. Phase 4 adds a service model with
  dependencies, composition, and live reload via shepherd-style
  activation. A service can declare `requires = [ "network" ]` and 2O9
  orders activation accordingly.

- **Users and groups.** Declare users in `2O9.nix`:
  ```nix
  users.rui = {
    uid = 1000;
    groups = [ "wheel" "video" ];
    shell = "/bin/zsh";
    home = "/home/rui";
  };
  ```
  2O9 creates the user and group on `209 apply`, with idempotent
  updates. This replaces the manual `useradd` step.

- **PAM and NSS.** Declare PAM service configs and NSS modules in
  `2O9.nix`. 2O9 writes the files to `/etc/pam.d/` and `/etc/nsswitch.conf`
  on apply. This is what makes a 2O9-managed system actually log in.

- **Bootloader (including grub).** Declare the bootloader in `2O9.nix`:
  ```nix
  boot.loader.grub = {
    enable = true;
    device = "/dev/sda";
    extraEntries = ''
      menuentry "Windows" { ... }
    '';
  };
  ```
  2O9 generates `grub.cfg` with one entry per 2O9 generation, runs
  `grub-install` and `grub-mkconfig` on apply. Rolling back a
  generation updates the grub menu. This also covers systemd-boot for
  UEFI systems.

- **Kernel and initrd.** Declare `boot.kernel = "linux-lts"` and 2O9
  installs that kernel, regenerates the initrd, and updates the
  bootloader entry. Kernel modules are declared in
  `boot.kernelModules`. Initrd modules in `boot.initrdModules`.

- **Profile hooks as derivations.** Today the activation phase runs
  `gtk-update-icon-cache`, `update-desktop-database`,
  `update-ca-certificates`, etc. as imperative side effects on the live
  filesystem. Phase 4 turns these into per-profile store paths. Roll
  back a generation and the cache files roll back with it.

- **File systems and swap.** Declare `fileSystems` and `swapDevices`
  in `2O9.nix`. 2O9 writes `/etc/fstab` on apply.

## What Phase 4 does NOT cover

- **A custom init system.** 2O9 does not replace systemd. It tells
  systemd what to enable and gets out of the way. Phase 4 adds service
  dependency ordering for the activation phase, but systemd is still
  PID 1.

- **Cross-machine build delegation.** No nix-daemon equivalent. 2O9 is
  single-machine.

- **A TUI.** The CLI stays flat. If you want a TUI, write a separate
  `209-tui` binary that shells out.

## Branch policy

This branch is for Phase 4 work only. It does not get merged into
main. When Phase 4 is done, it stays on this branch (or gets its own
release branch). Main continues at 0.1.0 as a package manager.

If you want the package manager without the system management, use
main. If you want the full declarative system, use phase-4.

## Current state

### Done

- **Users and groups.** `src/declarative/users.c` (530 LOC). Declares
  users and groups in `2O9.nix`, creates/updates them on `209 apply`
  via `useradd`/`usermod`/`groupadd`/`groupmod`/`gpasswd`. Handles
  supplementary groups, hashed passwords, system users, home creation,
  and removal of users/groups no longer in the config (with
  confirmation). See `docs/PHASE4_USERS.md`.

- **File systems and swap.** `src/declarative/fstab.c` (476 LOC).
  Declares `fileSystems` and `swapDevices` in `2O9.nix`, generates
  `/etc/fstab` on apply. Creates swap files, backs up the old fstab,
  runs `swapon -a` and `mount -a`. See `docs/PHASE4_FSTAB.md`.

- **Bootloader (grub + systemd-boot).** `src/declarative/bootloader.c`
  (480 LOC). Declares `boot.loader.grub` or `boot.loader.systemd-boot`
  in `2O9.nix`. Generates grub.cfg or systemd-boot entries with one
  menu entry per 2O9 generation. Runs `grub-install` or `bootctl
  install`. Regenerates initrd via `mkinitcpio`. See
  `docs/PHASE4_BOOTLOADER.md`.

- **Services as a DAG.** `src/declarative/services.c` (627 LOC).
  Declares services with `requires`, `after`, `before` dependencies.
  Topologically sorts via Kahn's algorithm with cycle detection.
  Generates systemd unit files for services with `execStart`. Enables,
  starts, restarts (if `restartOnChange`), disables, and stops based
  on the diff against the previous generation. See
  `docs/PHASE4_SERVICES.md`.

- **PAM and NSS.** `src/declarative/pam_nss.c` (391 LOC). Declares
  PAM service configs in `security.pam` (both string and structured
  forms) and NSS sources in `security.nss`. Writes `/etc/pam.d/<svc>`
  and `/etc/nsswitch.conf` on apply, with backups. See
  `docs/PHASE4_PAM_NSS.md`.

- **Profile hooks as derivations.** `src/declarative/profile_hooks.c`
  (500 LOC). Declares `profileHooks` in `2O9.nix`. Built-in hooks
  (gtk-icon-cache, desktop-database, font-cache, ldconfig) run their
  system command, copy the output to a store path, and NAR-hash it.
  Custom hooks run via `sh -c` with a clean env and `OUT` set. Hook
  outputs land at `/nix/store/<hash>-hook-<name>/` and are recorded in
  the generation manifest for rollback. See
  `docs/PHASE4_PROFILE_HOOKS.md`.

All six modules are wired into `cmd_apply` and run after the
activation phase, in this order: users, fstab, bootloader, services,
PAM/NSS, profile hooks.

### Not started

Nothing. All items from the original plan are implemented.
