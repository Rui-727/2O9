/* two9_init.h — 2O9 programmatic config entrypoint for libalpm
 *
 * See two9_init.c for implementation details. This is the public API
 * 2O9's CLI uses to construct a configured alpm_handle_t from a 2O9
 * manifest, without ever reading /etc/pacman.conf (MODIFICATIONS.md #3).
 */

#ifndef TWO9_INIT_H
#define TWO9_INIT_H

#include "alpm.h"

/* Build a fully-configured alpm_handle_t from a 2O9 manifest JSON.
 * The manifest is the output of evaluating 2O9.nix (see
 * src/declarative/gen.c). Configures root, dbpath, cachedirs,
 * architectures, sync DBs. Returns NULL on failure. */
alpm_handle_t *two9_alpm_init_from_manifest(const char *manifest_json);

/* Register the 2O9 install_backend and installed_set_loader callbacks
 * on an existing alpm_handle_t. After this call, libalpm's extraction
 * and local-DB-populate steps dispatch to 2O9's store adapter and
 * generation DB respectively. Pass NULL for either callback to disable
 * that dispatch (useful for testing). */
void two9_alpm_register_backends(alpm_handle_t *handle,
                                 char *(*install_backend)(alpm_handle_t *, alpm_pkg_t *, const char *),
                                 int (*installed_set_loader)(alpm_handle_t *, alpm_db_t *));

#endif /* TWO9_INIT_H */
