/* symlinks.h — 2O9 symlink farm builder
 *
 * The symlink farm is how packages become visible to users. After a
 * package is added to the store, its binaries are symlinked into
 * ~/.local/bin/, its libraries into ~/.local/lib/, and its config
 * files into /etc/ (which needs root).
 *
 * From DESIGN.md §7:
 *   binaries:   ~/.local/bin/   →  store  (per user, no root needed)
 *   libraries:  ~/.local/lib/   →  store  (per user, no root needed)
 *   config:     /etc/           →  store  (system, root needed)
 *   system bins: /usr/bin/      →  store  (system, root needed)
 *
 * No daemon. Symlinks written directly on 209 apply / 209 install.
 */

#ifndef TWO9_SYMLINKS_H
#define TWO9_SYMLINKS_H

#include "store.h"
#include "gen.h"

/* Build the symlink farm for a generation.
 *
 * This creates/updates symlinks in ~/.local/bin/, ~/.local/lib/,
 * and /etc/ based on the packages in the generation.
 *
 * If prev_gen is provided, only the diff between the two generations
 * is applied (remove old symlinks, add new ones).
 *
 * Returns 0 on success, -1 on error.
 */
int symlink_farm_build(gen_db_t *db, gen_t *gen, gen_t *prev_gen);

/* Tear down the symlink farm for a generation (remove all symlinks).
 * Used before rollback. Returns 0 on success. */
int symlink_farm_teardown(gen_t *gen);

/* Create a single symlink, creating parent directories as needed.
 * Returns 0 on success. */
int symlink_create(const char *target, const char *link_path);

/* Remove a symlink (or any file). Returns 0 on success. */
int symlink_remove(const char *link_path);

#endif /* TWO9_SYMLINKS_H */
