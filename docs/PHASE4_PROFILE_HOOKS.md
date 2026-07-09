# Phase 4: Profile Hooks

2O9 Phase 4 turns the imperative cache-rebuild step from the activation
phase into per-generation store paths. `209 apply` runs each enabled
hook in a temp directory, NAR-hashes the output, and moves it to
`/nix/store/<hash>-hook-<name>/`. The output is recorded in the
generation's `hooks.json`. Rolling back the generation drops the hook
output. Unreferenced hook outputs are collected by the next `209 gc`.

Before Phase 4, `209 apply` ran `gtk-update-icon-cache`,
`update-desktop-database`, `fc-cache`, and `ldconfig` directly on the
live filesystem. Cache files lived outside the store. Rolling back a
generation rolled back the packages but left the cache files from the
newer generation in place. Phase 4 fixes this by producing each cache
file as a store path tied to the generation.

## Config schema

Declare hooks under `profileHooks`. Built-in hooks are toggled with
`true` or `false`. Custom hooks are attrsets with a `command` string
and an optional `dest` path.

```nix
{
  profileHooks = {
    gtk-icon-cache = true;       # runs gtk-update-icon-cache
    desktop-database = true;     # runs update-desktop-database
    font-cache = true;           # runs fc-cache
    ldconfig = false;            # off by default

    "custom-ca-certs" = {
      command = ''
        trust extract --filter=ca-anchors --purpose=server-auth \
          --format=java-cacerts $out/cacerts.jks
      '';
      dest = "/etc/ssl/certs/java/cacerts.jks";
    };
  };
}
```

If `profileHooks` is absent from `2O9.nix`, 2O9 runs the three default
built-ins (`gtk-icon-cache`, `desktop-database`, `font-cache`) and
leaves `ldconfig` off. This preserves the pre-Phase-4 behavior for
configs that do not declare `profileHooks`.

If `profileHooks` is present, only the hooks you list run. An empty
`profileHooks = {};` runs nothing.

## Built-in hooks

| Name | Binary | Output | Destination |
|---|---|---|---|
| `gtk-icon-cache` | `gtk-update-icon-cache` | `icon-theme.cache` | `/usr/share/icons/hicolor/icon-theme.cache` |
| `desktop-database` | `update-desktop-database` | `mimeinfo.cache` | `/usr/share/applications/mimeinfo.cache` |
| `font-cache` | `fc-cache` | files under `/var/cache/fontconfig` | `/var/cache/fontconfig` |
| `ldconfig` | `ldconfig` | `ld.so.cache` | `/etc/ld.so.cache` |

Each built-in runs the named binary, then copies the generated cache
file or files into the hook's temp directory. The temp directory is
NAR-hashed and moved to `/nix/store/<hash>-hook-<name>/`.

`ldconfig` is off by default because the 2O9 store does not need it.
Turn it on if you drop shared libraries into `/usr/lib/` outside the
store and want the linker cache rebuilt per generation.

If a built-in binary is not installed, 2O9 skips that hook silently.
If a built-in runs but produces no cache file, 2O9 skips it without
creating a store path.

## Custom hooks

A custom hook is an attrset with a `command` string and an optional
`dest` path.

```nix
{
  profileHooks = {
    "custom-ca-certs" = {
      command = ''
        trust extract --filter=ca-anchors --purpose=server-auth \
          --format=java-cacerts $out/cacerts.jks
      '';
      dest = "/etc/ssl/certs/java/cacerts.jks";
    };
  };
}
```

2O9 runs the `command` via `sh -c` with `$OUT` and `$out` set to the
temp directory. The script writes its output under `$OUT/`. After the
script exits, 2O9 NAR-hashes the temp directory and moves it to
`/nix/store/<hash>-hook-<name>/`.

The custom hook environment is clean. Only `OUT`, `out`, and
`PATH=/usr/bin:/bin` are set. The parent process environment (`HOME`,
`USER`, `LANG`, and the rest) does not leak into the hook. If your
hook needs more, set it inside the command.

`dest` is recorded in `hooks.json` for the symlink farm. The symlink
farm integration is wired up by the parent; for now, 2O9 records the
value and prints what would be symlinked.

## What `209 apply` does

For each enabled hook:

1. Create `/nix/store/.tmp/hook-<name>-<pid>/`.
2. Set `OUT` and `out` to the temp directory path.
3. Run the hook (built-in command or `sh -c <command>`).
4. NAR-hash the temp directory.
5. Compute `/nix/store/<base32>-hook-<name>/` from the NAR hash.
6. `rename()` the temp directory to the final store path (atomic on
   the same filesystem).
7. Append `{name, store_path, nar_hash, dest}` to the generation's
   `hooks.json`.

If a hook command exits non-zero, 2O9 prints a warning and continues
with the next hook. The apply does not abort.

If a hook produces an empty output directory, 2O9 skips it without
creating a store path.

If a hook binary is missing (exit 127), 2O9 skips that hook silently.

If a hook's output NAR-hash matches an existing store path, 2O9 drops
the new temp directory and reuses the existing path. Same content
always lands at the same store path.

## Rollback

Each generation's `hooks.json` lists the hook outputs for that
generation. Rolling back to an earlier generation restores the earlier
hook outputs. Hook outputs that belonged only to the rolled-back-from
generation become unreferenced and are collected by the next `209 gc`.

## Permissions

`profile_hooks_apply` requires root. It writes to `/nix/store/` and
may run system utilities (`ldconfig`, `update-desktop-database`, etc.)
that need root. Run `209 apply` with `sudo`.

## Examples

### Defaults (no `profileHooks` block)

```nix
{ /* ... */ }
```

2O9 runs `gtk-icon-cache`, `desktop-database`, and `font-cache`. No
`ldconfig`.

### All four built-ins on

```nix
{
  profileHooks = {
    gtk-icon-cache = true;
    desktop-database = true;
    font-cache = true;
    ldconfig = true;
  };
}
```

### Skip icon cache (no GTK on the system)

```nix
{
  profileHooks = {
    gtk-icon-cache = false;
    desktop-database = true;
    font-cache = true;
  };
}
```

### Custom hook only

```nix
{
  profileHooks = {
    "regenerate-mime" = {
      command = "update-mime-database $out/mime";
      dest = "/usr/share/mime";
    };
  };
}
```

Built-ins do not run because `profileHooks` is present and only lists
the custom hook.

### Custom hook with no `dest`

```nix
{
  profileHooks = {
    "build-info" = {
      command = ''
        echo "built by 209 at $(date)" > $out/build-info.txt
      '';
    };
  };
}
```

`dest` is null in `hooks.json`. The output lives in the store at
`/nix/store/<hash>-hook-build-info/` but is not symlinked anywhere by
the default symlink farm. Read it from the store path directly.
