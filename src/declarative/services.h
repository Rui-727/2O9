/* services.h - declarative systemd service management (Phase 4)
 *
 * Applies the `services` attrset from a 2O9 manifest to systemd.
 * Services can declare `requires` dependencies on each other; 2O9
 * topologically sorts them by those edges and activates them in order.
 * Services with `execStart` get a generated unit file at
 * /etc/systemd/system/<name>.service. Services without `execStart`
 * are treated as pre-existing system services (sshd, NetworkManager)
 * and just enabled/disabled.
 *
 * See docs/PHASE4_SERVICES.md for the config schema and examples.
 */

#ifndef TWO9_SERVICES_H
#define TWO9_SERVICES_H

/* Apply service declarations from the manifest JSON.
 *
 * - Topologically sorts services by their `requires` dependencies
 *   (Kahn's algorithm). Cycles and references to undeclared services
 *   are fatal errors.
 * - Generates unit files for services with `execStart` set, written
 *   to /etc/systemd/system/<name>.service when content differs from
 *   the existing file.
 * - Enables/disables services via `systemctl enable|disable`.
 * - Starts services that are enabled and not running. Restarts running
 *   services when `restartOnChange` is true and the unit file changed.
 * - For services in the previous manifest but not the current one:
 *   disable, stop if running, remove the generated unit file.
 * - Runs `systemctl daemon-reload` once if any unit file was written
 *   or removed.
 *
 * manifest_json:      the evaluated 2O9.nix JSON (from nix_eval_file).
 * prev_manifest_json: the previous generation's manifest JSON, or NULL.
 * dry_run:            if 1, print what would happen but do not execute.
 *
 * Returns 0 on success, non-zero on error. Errors during individual
 * service operations are logged but do not abort the whole apply; a
 * cycle, a missing `requires` target, an invalid service name, or
 * non-root invocation is fatal and returns non-zero immediately.
 *
 * Requires root (getuid() == 0) for systemctl and writes under
 * /etc/systemd/system/. Returns non-zero with a clear message if
 * invoked as a non-root user.
 */
int services_apply(const char *manifest_json, const char *prev_manifest_json,
                   int dry_run);

#endif /* TWO9_SERVICES_H */
