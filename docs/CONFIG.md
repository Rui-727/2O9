# Configuration Reference

2O9 has two config files per scope, both Nix. `<scope>.nix` is the
declarative config (Nix syntax, says what your system or user should
look like). `<scope>.extra.nix` is the imperative side config (also
Nix, holds binary-cache subs, signing keys, and AUR build flags).

Per locked decision #7 in DESIGN.md: "One declarative config format:
Nix." There is no INI file anymore.

# Part 1: locations and multi-user

All config lives under `/nix/config/`. The directory is root:root 0755.
Four kinds of file:

| File | Owner | Mode | Purpose |
|---|---|---|---|
| `/nix/config/2O9.nix` | root:root | 0644 | System-wide declarative (packages, services, repos). Wins on conflict |
| `/nix/config/extra.nix` | root:root | 0644 | System-wide runtime (bin paths, subs, chroot) |
| `/nix/config/<user>.nix` | `<user>:<user>` | 0644 | Per-user declarative |
| `/nix/config/<user>.extra.nix` | `<user>:<user>` | 0644 | Per-user runtime |

`<user>` is the Unix username. When running under `sudo`, `SUDO_USER`
is used so `sudo 209 apply` reads the original user's config.

## Multi-user

`209 init --all` (run as root) scans `/etc/passwd` for every user with
uid >= 1000 and a home dir under `/home/*`, excluding root and nobody.
For each detected user, it creates `/nix/config/<user>.nix` and
`/nix/config/<user>.extra.nix`, chowned to that user (mode 0644). This
is the recommended setup for shared machines: each user edits their own
config, `sudo 209 apply` reconciles them all with the system config.

## `2O9.nix` schema

A Nix expression that evaluates to an attrset describing what your
system or user should look like. One file, one format, one source of
truth.

## The function form

Every `2O9.nix` must be a function taking at least `{ config, ... }:`:

```nix
{ config, ... }:
let
  basePackages = [ "vim" ];
in
{
  # Self-reference: config is the result of evaluating this function.
  # You can read other parts of the config from here.
  packages = basePackages ++ (if config.services.sshd.enable
                              then [ "openssh" ]
                              else []);
}
```

The `config` argument enables self-reference. The evaluator resolves
this via fixed-point recursion: `config` is the result of evaluating
the function itself, resolved lazily. Use `let` to bind a name to your
base package list so the `packages` attribute can refer to it without
infinite recursion.

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

### `snapshots` (attrset of attrsets)

Paths to snapshot on a schedule. Each key is a path, each value is an
attrset with `auto` and `keep`. Snapshots are NAR-hashed copies stored
in `/nix/store/<base32>-snap-<sanitized-name>/`, content-addressed like
packages. A SQLite DB at `~/.local/state/2O9/snapshots.sqlite` (or
`/var/lib/2O9/snapshots.sqlite` for system scope) records the snapshot
history per path with parent links.

Path resolution rules depend on which config file declares them:

- In `2O9.nix` (system config): paths MUST be absolute. A relative
  path is rejected with an error.
- In `<user>.nix` (user config): paths are relative to that user's
  home dir. `Documents` resolves to `/home/<user>/Documents`. Absolute
  paths and paths starting with `~` are rejected.

```nix
# /nix/config/2O9.nix - system paths must be absolute
snapshots = {
  "/var/lib/postgres" = {
    auto = "daily";     # "hourly", "daily", "weekly", "manual"
    keep = 7;           # keep last 7, 0 = keep forever
  };
};
```

```nix
# /nix/config/<user>.nix - user paths are relative to home
snapshots = {
  "Documents" = {
    auto = "daily";
    keep = 7;
  };
  "projects" = {
    auto = "manual";    # only taken via `209 snapshot take`
    keep = 0;
  };
};
```

`auto` selects the systemd timer schedule. `manual` means no timer is
installed; snapshots are only taken when you run `209 snapshot take
<path>`. The other values (`hourly`, `daily`, `weekly`) install a
systemd timer that runs `209 snapshot take <path>` on that calendar.
System paths run as root; user paths run as the declaring user.

`keep` is the retention count. After each `209 apply`, snapshots beyond
the keep count are pruned (oldest first). `0` means keep forever.

A path declared in `snapshots` is "managed". `209 snapshot take <path>`
refuses to snapshot unmanaged paths. Take, list, restore, diff, and rm
all live under `209 snapshot <subcommand>`; see the manpage for the
full command reference.

## Imports

Configs can be split across multiple files using `import`:

```nix
# /nix/config/2O9.nix
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
# /nix/config/packages.nix
[ "firefox" "neovim" "curl" ]
```

```nix
# /nix/config/services.nix
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

## Config inclusion model

2O9 evaluates only `/nix/config/2O9.nix`. That file is the single entry
point. User configs (`<user>.nix`) do nothing on their own. They take
effect only if `2O9.nix` imports them via standard Nix `import`:

```nix
# /nix/config/2O9.nix
let myuser = import ./myuser.nix; in
{ config, ... }:
{
  packages = [ "vim" ] ++ myuser.packages or [];
  services = myuser.services or {};
}
```

This gives the sysadmin explicit control over what is active. A user
can create `<user>.nix` but it sits idle until the sysadmin adds an
`import` for it in `2O9.nix`.

Same model for `extra.nix`: only `/nix/config/extra.nix` is loaded.
User side configs (`<user>.extra.nix`) take effect only if `extra.nix`
imports them:

```nix
# /nix/config/extra.nix
let myuser = import ./myuser.extra.nix; in
{
  bin = { Makepkg = "makepkg"; };
  subs = myuser.subs or {};
}
```

There is no automatic merge. If you want user packages concatenated
with system packages, write that in `2O9.nix` using `++`:

```nix
packages = [ "vim" ] ++ myuser.packages or [];
```

If you want the system to override a user setting, write that too. The
merge logic lives in your config, not in 2O9's C code.

## Worked example

```nix
{ config, ... }:
let
  basePackages = [ "vim" "curl" "git" "htop" ];
  desktopPackages = [ "firefox" "alacritty" "dunst" ];
in
{
  packages = basePackages ++ desktopPackages
    ++ (if config.services.sshd.enable
        then [ "openssh" ]
        else []);

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

  snapshots = {
    "/var/lib/postgres" = {
      auto = "daily";
      keep = 7;
    };
  };
}
```

Run `209 apply` to make the system match this file. The `let` block
binds `basePackages` and `desktopPackages` so the `packages` attribute
can concatenate them plus conditionally add `openssh` based on
`config.services.sshd.enable`, all without infinite recursion.

# Part 2: `extra.nix`

An optional Nix file for stuff that doesn't belong in the declarative
config. Lives at `/nix/config/<user>.extra.nix` (or `/nix/config/extra.nix`
for system-wide).

Format: a Nix attrset (or `{ config, ... }: { ... }` function form, but
plain attrset is recommended since extra.nix has no need for the
`config` self-reference that 2O9.nix uses). The evaluator handles both.
List values (`MFlags`, `GitFlags`, `URLs`, `PublicKeys`) are Nix lists
of strings.

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

  subs = {
    personal = {
      URLs = [ "https://cache.example.com" ];
      PublicKeys = [ "r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=" ];
      SigningKey = "/etc/2O9/secret-key";
      KeyName = "cache.example.com-1";
      AllowUnsigned = false;
    };
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

### `subs`

Named binary-cache substituters. Each sub is an attrset keyed by a
bare identifier (e.g. `personal`) or a quoted string (e.g.
`"personal-cache"`). String keys are needed when the key contains
non-identifier characters; bare identifiers work for simple names.

```nix
subs = {
  personal = {
    URLs = [ "https://cache.example.com" "s3://my-bucket" ];
    PublicKeys = [ "key1base64==" "key2base64==" ];
    SigningKey = "/etc/2O9/personal-secret-key";
    KeyName = "personal-1";
    AllowUnsigned = false;
  };
  friend = {
    URLs = [ "https://friend.example.org/cache" ];
    PublicKeys = [ "friendkeybase64==" ];
    AllowUnsigned = false;
  };
};
```

`URLs` is a Nix list of strings. `http://` and `https://` URLs use
libcurl. `s3://` URLs shell out to the `aws` CLI.

`PublicKeys` is a Nix list of base64 Ed25519 public keys. A narinfo is
accepted if ANY of the listed keys verifies its signature. This lets
you rotate keys or accept packages signed by multiple publishers.

`SigningKey` is the path to a file containing the 32-byte Ed25519
secret key. Used by `209 cache push` to sign narinfos. Only needs to
be present on publishing machines, not subscribers.

`KeyName` is the human-readable name embedded in the `Sig:` line of
each narinfo. Subscribers don't need this.

`AllowUnsigned = true` accepts narinfos with no signature or with a
signature from an unknown key. Use only for trusted local caches.
Default is `false`.

#### Cache index (`index.json`)

Every time `209 share <path>` or `209 cache push <path>` uploads to a
cache, it also appends an entry to the cache's `index.json` at the
cache root. The index lists every share, package, and snapshot pushed
to that cache:

```json
{
  "version": 1,
  "updated_at": 1735689600,
  "items": [
    {
      "hash": "abc123share",
      "name": "myapp-backup",
      "type": "share",
      "nar_size": 13000000,
      "nar_hash": "sha256:...",
      "pushed_at": 1735689600,
      "signed_by": "personal-1"
    }
  ]
}
```

The cache server (HTTP or S3) just serves files. 2O9 maintains the
index by fetching the current one, appending the new entry, and
re-uploading. If two publishers race, last-write-wins (acceptable for
now; the NAR and narinfo files themselves are unaffected).

`209 subs` and `209 subs <name>` fetch `index.json` from each URL in
the sub and merge the items (deduped by hash) to display the sub's
contents. A sub with multiple URLs shows the union of all URLs'
indexes.

#### Backward compat: `substituters`

The pre-v2 flat `substituters` block is still parsed as a single sub
named `legacy`. Its singular `PublicKey` field (a string) is folded
into the `PublicKeys` list as a single entry. A deprecation warning is
printed. To migrate, rename `substituters` to `subs.legacy` and
convert `PublicKey` (string) to `PublicKeys` (list of strings).

## Worked example: two machines sharing a cache

On machine A (the publisher):

```sh
209 keygen
# Output:
#   public key (add to extra.nix subs.<name>.PublicKeys): r634rsy7nIo/...
#   cache.example.com-1:r634rsy7nIo/...:TakyhFMCwVcOjdPUJurMrgEQeyuuGukyL+/wWYoCFQ8=
```

Save the second line to `/etc/2O9/secret-key` (mode 0600). Add to
`/nix/config/extra.nix`:

```nix
subs = {
  personal = {
    URLs = [ "https://cache.example.com" ];
    SigningKey = "/etc/2O9/secret-key";
    KeyName = "cache.example.com-1";
  };
};
```

Install a package and push it:

```sh
209 -S neovim
209 cache push /nix/store/<hash>-neovim-0.10.0
```

On machine B (the subscriber), add to `/nix/config/extra.nix`:

```nix
subs = {
  personal = {
    URLs = [ "https://cache.example.com" ];
    PublicKeys = [ "r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=" ];
    AllowUnsigned = false;
  };
};
```

Now `209 -S neovim` on machine B will hit the cache first. If the
cache has it and the signature verifies, the package is downloaded as
a NAR and streamed into `/nix/store/` without ever touching the Arch
mirror.
