# 2O9.nix — Configuration Reference

`2O9.nix` is the declarative config file. It's a Nix expression that
evaluates to an attrset describing what your system should look like.
One file, one format, one source of truth.

## Locations

| Scope | File | Notes |
|---|---|---|
| User | `~/.config/2O9/home.nix` | Per-user packages and settings |
| System | `/etc/2O9/2O9.nix` | System-wide; wins on conflict |

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
this via fixed-point recursion — `config` is the result of evaluating
the function itself, resolved lazily.

## Schema

### `packages` (list of strings)

Packages to install from the official Arch repos.

```nix
packages = [ "vim" "curl" "git" "htop" ];
```

### `aur.packages` (list of strings)

Packages to build from the AUR. Each is fetched via `git clone` from
`aur.archlinux.org`, built with `makepkg`, and the resulting
`.pkg.tar.zst` is added to the store.

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
`alpm_option_set_*` API programmatically — there is no `pacman.conf`.

```nix
pacman.options = {
  SigLevel = "Required DatabaseOptional";
  ParallelDownloads = 5;
  IgnorePkg = [ "linux" ];  # packages to never upgrade
};
```

### `pacman.repos` (attrset of attrsets)

Repo definitions. Each repo gets registered via
`alpm_register_syncdb` + `alpm_db_add_server`.

```nix
pacman.repos = {
  core     = { server = "https://mirror.example.com/core/os/x86_64"; };
  extra    = { server = "https://mirror.example.com/extra/os/x86_64"; };
  multilib = { server = "https://mirror.example.com/multilib/os/x86_64"; };
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

2O9 does not manage services itself — it just tells systemd what to
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
not the C++ Nix evaluator — it's a small subset sufficient for `2O9.nix`.

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

For list values (e.g. `packages`), the lists concatenate — both user
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
      core     = { server = "https://mirror.example.com/core/os/x86_64"; };
      extra    = { server = "https://mirror.example.com/extra/os/x86_64"; };
      multilib = { server = "https://mirror.example.com/multilib/os/x86_64"; };
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
