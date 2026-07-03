# Configuration Reference

2O9 has two config files, both Nix. `2O9.nix` is the declarative
config (Nix syntax, says what your system should look like).
`extra.nix` is the imperative side config (also Nix, holds
substituters, signing keys, and AUR build flags).

Per locked decision #7 in DESIGN.md: "One declarative config format:
Nix." There is no INI file anymore.

# Part 1: `2O9.nix`

A Nix expression that evaluates to an attrset describing what your
system should look like. One file, one format, one source of truth.

## Locations

| Scope | File | Notes |
|---|---|---|
| User | `~/.config/2O9/home.nix` | Per-user packages and settings |
| System | `/etc/2O9/2O9.nix` | System-wide. Wins on conflict |

Both files are evaluated on `209 apply` and merged. The system config
takes precedence on conflicts; package lists concatenate.

## The function form

Every `2O9.nix` must be a function taking at least `{ config, ... }:`:

```nix
{ config, ... }:
{
  packages = [ "vim" ];
  # Self-reference: config is the result of evaluating this function.
  # You can read other parts of the config from here.
  packages = packages ++ (if config.services.sshd.enable
                          then [ "openssh" ]
                          else []);
}
```

The `config` argument enables self-reference. The evaluator resolves
this via fixed-point recursion: `config` is the result of evaluating
the function itself, resolved lazily.

## Schema

### `packages` (list of strings)

Packages to install from the official Arch repos.

```nix
packages = [ "vim" "curl" "git" "htop" ];
```

### `aur.packages` (list of strings)

Packages to build from the AUR. Each is fetched via `git clone` from
`aur.archlinux.org`, built with `makepkg` (inside a chroot by default),
and the resulting `.pkg.tar.zst` is added to the store.

```nix
aur.packages = [ "google-chrome" "visual-studio-code-bin" ];
```

### `aur.build` (attrset)

Build optimization for AUR packages. Injected into makepkg's
environment.

```nix
aur.build = {
  # "native" = -march=native -O3 -pipe
  # "safe"   = -O2 -pipe
  # Or set explicitly:
  profile = "native";

  # Per-package overrides
  ffmpeg = {
    CFLAGS = "-march=native -O3 -pipe -fno-plt";
  };

  # Parallel build jobs (overrides makepkg.conf MAKEFLAGS)
  jobs = "auto";  # auto = nproc, or an integer like 8
};
```

### `pacman.options` (attrset)

Maps directly to pacman.conf options. Passed to lib2O9's
`alpm_option_set_*` API programmatically. There is no `pacman.conf`.

```nix
pacman.options = {
  SigLevel = "Required DatabaseOptional";
  ParallelDownloads = 5;
  IgnorePkg = [ "linux" ];  # packages to never upgrade
};
```

`SigLevel` is parsed into the libalpm bitmask and applied to the handle
and to every registered sync DB. Package `.sig` files are fetched
alongside the `.pkg.tar.zst` and verified via gpgme before extraction.
If `SigLevel` includes `Required` and the signature is missing or
invalid, the install is refused. Set `SigLevel = "Never"` to disable
signature verification entirely.

### `pacman.repos` (attrset of attrsets)

Repo definitions. Each repo gets registered via
`alpm_register_syncdb` (with the parsed `SigLevel`) plus
`alpm_db_add_server`.

```nix
pacman.repos = {
  core     = { server = "https://geo.mirror.pkgbuild.com/core/os/x86_64"; };
  extra    = { server = "https://geo.mirror.pkgbuild.com/extra/os/x86_64"; };
  multilib = { server = "https://geo.mirror.pkgbuild.com/multilib/os/x86_64"; };
};
```

The `$arch` and `$repo` variables in server URLs are expanded by
libalpm at download time.

### `services` (attrset of attrsets)

Services to enable or disable. Translates to `systemctl enable` /
`systemctl disable` during the activation phase.

```nix
services = {
  sshd.enable = true;
  NetworkManager.enable = true;
  cups.enable = false;  # explicitly disable
};
```

2O9 does not manage services itself, it just tells systemd what to
enable. After `209 apply`, a reboot is recommended for the full system
state to take effect.

## Imports

Configs can be split across multiple files using `import`:

```nix
# /etc/2O9/2O9.nix
{ config, ... }:
let
  packages = import ./packages.nix;
  services = import ./services.nix;
in
{
  packages = packages;
  services = services;
}
```

```nix
# /etc/2O9/packages.nix
[ "firefox" "neovim" "curl" ]
```

```nix
# /etc/2O9/services.nix
{ sshd.enable = true; }
```

## Self-reference

The `config` argument lets one part of the config read another:

```nix
{ config, ... }:
{
  services.sshd.enable = true;

  # Install openssh only if sshd is enabled
  packages = if config.services.sshd.enable
             then [ "openssh" ]
             else [];
}
```

This is resolved via fixed-point recursion. The evaluator iterates
until `config` stabilizes (up to 100 iterations).

## Evaluation

The evaluator is written from scratch in C as part of lib2O9. It is
not the C++ Nix evaluator. It's a small subset sufficient for `2O9.nix`.

Supported:

- Attribute sets (plain and `rec`)
- Lists, strings (with `${interpolation}`), integers, booleans, null
- `let ... in ...`, `if ... then ... else ...`, `with`, `assert`
- Lambda functions: `x: body`, `{ a, b ? default, ... }: body`
- Function application (including curried: `(x: y: x + y) 3 4`)
- `import` / `include` with relative paths
- 19 builtins: `map`, `filter`, `length`, `head`, `tail`,
  `attrNames`, `attrValues`, `hasAttr`, `getAttr`, `fromJSON`,
  `toJSON`, `trace`, etc.
- `builtins.<name>` dot-notation
- `inherit ident;` and `inherit (src) ident1 ident2;`
- All 9 binary operator precedence levels (`->`, `||`, `&&`, `==`,
  `!=`, `<`, `<=`, `>`, `>=`, `++`, `+`, `-`, `*`, `/`)

Not supported (not needed for `2O9.nix`):

- Derivations, fetchers, flakes
- Full nixpkgs evaluation

## Merge order

When both `home.nix` and `2O9.nix` exist:

```
built-in defaults
  → ~/.config/2O9/home.nix (user)
    → /etc/2O9/2O9.nix (system)  ← wins on conflict
      → CLI flags                  ← wins on everything
```

For list values (e.g. `packages`), the lists concatenate. Both user
and system packages get installed. For everything else, the system
config wins on conflict.

## Worked example

```nix
{ config, ... }:
let
  basePackages = [ "vim" "curl" "git" "htop" ];
  desktopPackages = [ "firefox" "alacritty" "dunst" ];
in
{
  packages = basePackages ++ desktopPackages;

  aur.packages = [ "google-chrome" ];
  aur.build.profile = "native";
  aur.build.jobs = "auto";

  pacman = {
    options = {
      SigLevel = "Required DatabaseOptional";
      ParallelDownloads = 10;
    };
    repos = {
      core     = { server = "https://geo.mirror.pkgbuild.com/core/os/x86_64"; };
      extra    = { server = "https://geo.mirror.pkgbuild.com/extra/os/x86_64"; };
      multilib = { server = "https://geo.mirror.pkgbuild.com/multilib/os/x86_64"; };
    };
  };

  services = {
    sshd.enable = true;
    NetworkManager.enable = true;
  };

  # Self-reference: install openssh only if sshd is enabled
  packages = packages
    ++ (if config.services.sshd.enable
        then [ "openssh" ]
        else []);
}
```

Run `209 apply` to make the system match this file.

---

# Part 2: `extra.nix`

An optional Nix file for stuff that doesn't belong in the declarative
config. Lives at `~/.config/2O9/extra.nix` (or `/etc/2O9/extra.nix`
for system-wide).

Format: a Nix attrset (or `{ config, ... }: { ... }` function form, but
plain attrset is recommended since extra.nix has no need for the
`config` self-reference that 2O9.nix uses). The evaluator handles both.
List values (`MFlags`, `GitFlags`, `URLs`) are Nix lists of strings.

```nix
{
  bin = {
    Makepkg = "makepkg";
    Git = "git";
    Gpg = "gpg";
    Sudo = "sudo";
    MFlags = [ "--skippgpcheck" "--nocheck" "-feA" ];
    GitFlags = [ "--depth" "1" ];
  };

  chroot = {
    Enabled = true;
    Dir = "/var/lib/2O9/chroot";
  };

  substituters = {
    URLs = [ "https://cache.example.com" ];
    PublicKey = "r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=";
    SigningKey = "/etc/2O9/secret-key";
    KeyName = "cache.example.com-1";
    AllowUnsigned = false;
  };
}
```

## Sections

### `bin`

Customize which binaries 2O9 invokes and what flags they get.

```nix
bin = {
  Makepkg = "makepkg";
  Git = "git";
  Gpg = "gpg";
  Sudo = "sudo";
  MFlags = [ "--skippgpcheck" "--nocheck" "-feA" ];
  GitFlags = [ "--depth" "1" ];
};
```

`bin.MFlags` replaces the default makepkg flags entirely. If you set
it, include `"-feA"` (or whatever you want) yourself. This matches
paru's MFlags semantics.

### `chroot`

AUR build isolation. On by default.

```nix
chroot = {
  Enabled = true;
  Dir = "/var/lib/2O9/chroot";
};
```

When enabled, AUR builds run inside an `arch-nspawn` chroot via
`makechrootpkg`. Requires the `devtools` package. If `mkarchroot` or
`arch-nspawn` is not on `$PATH`, the build fails with a clear error.

Set `Enabled = false` to run makepkg in your user environment. Not
recommended for untrusted PKGBUILDs.

### `substituters`

Binary cache configuration. Lets 2O9 pull packages from caches other
than the Arch mirror, and push to them.

```nix
substituters = {
  URLs = [ "https://cache.example.com" "s3://my-bucket" ];
  PublicKey = "r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=";
  SigningKey = "/etc/2O9/secret-key";
  KeyName = "cache.example.com-1";
  AllowUnsigned = false;
};
```

`URLs` is a Nix list of strings. Each URL is consulted in order on
install. `http://` and `https://` URLs use libcurl. `s3://` URLs shell
out to the `aws` CLI.

`PublicKey` is the base64 Ed25519 public key to verify narinfo
signatures against. Get it from `209 keygen` on the publishing machine.

`SigningKey` is the path to a file containing the 32-byte Ed25519
secret key. Used by `209 cache push` to sign narinfos. Only needs to
be present on publishing machines, not subscribers.

`KeyName` is the human-readable name embedded in the `Sig:` line of
each narinfo. Subscribers don't need this.

`AllowUnsigned = true` accepts narinfos with no signature or with a
signature from an unknown key. Use only for trusted local caches.
Default is `false`.

## Worked example: two machines sharing a cache

On machine A (the publisher):

```sh
209 keygen
# Output:
#   public key (add to extra.nix substituters.PublicKey): r634rsy7nIo/...
#   cache.example.com-1:r634rsy7nIo/...:TakyhFMCwVcOjdPUJurMrgEQeyuuGukyL+/wWYoCFQ8=
```

Save the second line to `/etc/2O9/secret-key` (mode 0600). Add to
`/etc/2O9/extra.nix`:

```nix
substituters = {
  URLs = [ "https://cache.example.com" ];
  SigningKey = "/etc/2O9/secret-key";
  KeyName = "cache.example.com-1";
};
```

Install a package and push it:

```sh
209 -S neovim
209 cache push /nix/store/<hash>-neovim-0.10.0
```

On machine B (the subscriber), add to `/etc/2O9/extra.nix`:

```nix
substituters = {
  URLs = [ "https://cache.example.com" ];
  PublicKey = "r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=";
  AllowUnsigned = false;
};
```

Now `209 -S neovim` on machine B will hit the cache first. If the
cache has it and the signature verifies, the package is downloaded as
a NAR and streamed into `/nix/store/` without ever touching the Arch
mirror.
