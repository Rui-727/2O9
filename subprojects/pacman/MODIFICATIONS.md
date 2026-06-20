# 2O9 Modifications to vendored pacman

This is pacman v6.0.0, copied into the 2O9 tree and modified directly.
Every change is marked with `/* 2O9: <reason> */` in the source.

## Modification targets

1. **Install backend**: dispatch to store adapter instead of libalpm's extractor
2. **Installed-set query**: solver reads from generation DB, not /var/lib/pacman/local/
3. **Config entrypoint**: configured programmatically, never from pacman.conf

## Change log

No modifications yet. Phase 1 will produce the first changes.
