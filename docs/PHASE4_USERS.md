# Phase 4: Users and Groups

2O9 Phase 4 declares system users and groups in `2O9.nix`. `209 apply`
creates, updates, and removes them to match the declaration.

## Schema

Declare users under `users` and groups under `groups`. Both are
attrsets keyed by name.

```nix
{
  users = {
    rui = {
      uid = 1000;
      group = "rui";
      groups = [ "wheel" "video" "audio" ];
      shell = "/bin/zsh";
      home = "/home/rui";
      isNormalUser = true;
      hashedPassword = "$6$rounds=5000$...";  # from `mkpasswd -m sha-512`
    };
    deploy = {
      uid = 1001;
      group = "deploy";
      shell = "/sbin/nologin";
      home = "/var/lib/deploy";
      isSystemUser = true;
    };
  };

  groups = {
    rui = { gid = 1000; };
    deploy = { gid = 1001; };
    developers = {
      gid = 2000;
      members = [ "rui" "deploy" ];
    };
  };
}
```

## User fields

| Field | Type | Default | Notes |
|---|---|---|---|
| `uid` | int | system-assigned | Omitted from `useradd`/`usermod` if not set |
| `group` | string | same as user name | Primary group |
| `groups` | list of string | `[]` | Supplementary groups |
| `shell` | string | `/bin/bash` for normal users, `/sbin/nologin` for system users | |
| `home` | string | `/home/<name>` for normal users, omitted for system users | |
| `isNormalUser` | bool | false | Sets `createHome = true` and the bash default shell |
| `isSystemUser` | bool | false | No home creation |
| `createHome` | bool | true for normal users, false for system users | Overrides the default |
| `hashedPassword` | string | (none) | SHA-512 hash from `mkpasswd -m sha-512` |
| `password` | string | (none) | Plaintext. Prints a warning on apply |
| `description` | string | (none) | Sets the GECOS field via `useradd -c` |

## Group fields

| Field | Type | Default | Notes |
|---|---|---|---|
| `gid` | int | system-assigned | |
| `members` | list of string | `[]` | Users added to this group via `gpasswd -a` |

## What `209 apply` does

Groups apply first, then users, then passwords, then supplementary
group memberships.

For each declared group:

- New group runs `groupadd -g <gid> <name>`.
- Existing group with a different gid runs `groupmod -g <gid> <name>`.
- Existing group with the right gid is skipped.

For each declared user:

- New user runs `useradd` with the declared fields.
- Existing user runs `usermod` for fields that changed.
- Supplementary groups go through `gpasswd -a` for new memberships and
  `gpasswd -d` for groups the user is no longer in.

A user's primary `group` is auto-created with `gid = uid` if it is not
declared in the `groups` block. This covers the common pattern where
user `rui` gets group `rui` with gid 1000.

`hashedPassword` runs through `chpasswd -e`. `password` runs through
`chpasswd` and prints a warning that plaintext is a bad idea. Generate
a hash with `mkpasswd -m sha-512`.

Group `members` is supplementary to per-user `groups`. A user ends up
in a group if either one lists them.

## Removal

Users and groups that existed in the previous generation's manifest but
not the current one are removed after a `[y/N]` prompt. If the user's
home directory still has files, a second prompt asks whether to lose
them. Answer `y` and 2O9 runs `userdel -r`; otherwise it runs `userdel`
and leaves the home directory alone.

Removal is skipped in `--dry-run` mode. The warning that lists what
would be removed still prints.

## Permissions

`209 apply` runs as root. `users_apply` checks `getuid() == 0` and
errors out with `209: users_apply requires root. Run with sudo.` if
invoked as a regular user.

## Examples

### Single user with their own group

```nix
{
  users.rui = {
    uid = 1000;
    isNormalUser = true;
    shell = "/bin/zsh";
    hashedPassword = "$6$rounds=5000$...";
  };
  # Group `rui` is auto-created with gid 1000.
}
```

### Deploy user with no login

```nix
{
  users.deploy = {
    uid = 920;
    group = "deploy";
    home = "/var/lib/deploy";
    shell = "/sbin/nologin";
    isSystemUser = true;
  };
  groups.deploy.gid = 920;
}
```

### Shared group with multiple members

```nix
{
  groups.developers = {
    gid = 2000;
    members = [ "rui" "deploy" ];
  };
}
```

Both users land in `developers` via `gpasswd -a`.

### Removing a user

Delete the entry from `2O9.nix`. On the next `209 apply`, 2O9 prints:

```
users: would remove user 'deploy' (was in previous generation, not in current)
  remove user? [y/N]
```

Answer `y` to confirm. Answer `n` (or press enter) to keep the user.
