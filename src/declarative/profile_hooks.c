/* profile_hooks.c - 2O9 profile hooks as per-generation store paths (Phase 4)
 *
 * Replaces the imperative cache-rebuild step in activation.c (step 8 of
 * the activation phase) with a derivations-style flow. Each enabled
 * hook runs in a temp directory under /nix/store/.tmp/, 2O9 NAR-hashes
 * the output, moves it to /nix/store/<base32-hash>-hook-<name>/, and
 * records the path in the generation's hooks.json. Rolling back the
 * generation drops the hook output. Unreferenced hook outputs are
 * collected by the next `209 gc` like any other orphan store path.
 *
 * Built-in hooks:
 *   gtk-icon-cache    gtk-update-icon-cache, output copied to $OUT
 *   desktop-database  update-desktop-database, output copied to $OUT
 *   font-cache        fc-cache, /var/cache/fontconfig files copied to $OUT
 *   ldconfig          ldconfig -f $OUT/ld.so.cache (writes to $OUT)
 *
 * Custom hooks run `sh -c <command>` with $OUT (and $out) set to the
 * temp directory and a clean environment. The command writes its output
 * there; 2O9 NAR-hashes the directory and links it into the store.
 *
 * Permissions: root only. The hook writes to /nix/store/ and may invoke
 * system utilities that need root.
 *
 * See docs/PHASE4_PROFILE_HOOKS.md for the config schema and examples.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>

#include "profile_hooks.h"
#include "cJSON.h"
#include "nar.h"

#define STORE_TMP_DIR  "/nix/store/.tmp"
#define STORE_ROOT     "/nix/store"

/* ── Color helpers (self-contained, like the other Phase 4 modules) ── */

static int want_color(void)
{
        static int v = -1;
        if (v < 0) v = isatty(STDERR_FILENO);
        return v;
}
static const char *C_RESET(void)  { return want_color() ? "\033[0m"  : ""; }
static const char *C_GREEN(void)  { return want_color() ? "\033[32m" : ""; }
static const char *C_YELLOW(void) { return want_color() ? "\033[33m" : ""; }
static const char *C_DIM(void)    { return want_color() ? "\033[2m"  : ""; }

/* ── Path helpers ──────────────────────────────────────────────────── */

/* mkdir -p. Returns 0 on success or if path already exists. */
static int mkdir_p(const char *path)
{
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", path);
        size_t len = strlen(tmp);
        if (len == 0) return -1;
        if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
        for (char *p = tmp + 1; *p; p++) {
                if (*p == '/') {
                        *p = '\0';
                        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
                        *p = '/';
                }
        }
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
        return 0;
}

/* Recursively remove a directory tree. Best-effort: ignores errors on
 * individual entries so a stuck file doesn't leak the parent dir. */
static void rmtree(const char *path)
{
        DIR *d = opendir(path);
        if (!d) { unlink(path); return; }
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0)
                        continue;
                char child[PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
                struct stat st;
                if (lstat(child, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) rmtree(child);
                        else unlink(child);
                }
        }
        closedir(d);
        rmdir(path);
}

/* Copy a single regular file. Creates parent dirs of dst. Returns 0 on
 * success, -1 on error. */
static int copy_file(const char *src, const char *dst)
{
        FILE *in = fopen(src, "rb");
        if (!in) return -1;
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", dst);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = '\0'; mkdir_p(parent); }
        FILE *out = fopen(dst, "wb");
        if (!out) { fclose(in); return -1; }
        char buf[65536];
        size_t n;
        int rc = 0;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
        }
        if (ferror(in)) rc = -1;
        fclose(in);
        fclose(out);
        return rc;
}

/* Copy all regular files from src_dir into dst_dir (non-recursive).
 * Returns the count copied, or -1 if src_dir can't be opened. */
static int copy_dir_files(const char *src_dir, const char *dst_dir)
{
        DIR *d = opendir(src_dir);
        if (!d) return -1;
        mkdir_p(dst_dir);
        int count = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0)
                        continue;
                char src[PATH_MAX], dst[PATH_MAX];
                snprintf(src, sizeof(src), "%s/%s", src_dir, ent->d_name);
                snprintf(dst, sizeof(dst), "%s/%s", dst_dir, ent->d_name);
                struct stat st;
                if (lstat(src, &st) != 0) continue;
                if (S_ISREG(st.st_mode)) {
                        if (copy_file(src, dst) == 0) count++;
                }
        }
        closedir(d);
        return count;
}

/* Returns 1 if the directory has no entries other than . and .. */
static int dir_is_empty(const char *path)
{
        DIR *d = opendir(path);
        if (!d) return 1;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0)
                        continue;
                closedir(d);
                return 0;
        }
        closedir(d);
        return 1;
}

/* Sanitize a hook name for use in a path component. Replaces any char
 * outside [A-Za-z0-9._-] with '_'. */
static void sanitize_name(char *dest, size_t n, const char *src)
{
        if (n == 0) return;
        size_t i = 0;
        for (; src[i] && i + 1 < n; i++) {
                char c = src[i];
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
                        dest[i] = c;
                else
                        dest[i] = '_';
        }
        dest[i] = '\0';
}

/* ── Subprocess execution ──────────────────────────────────────────── */

/* fork + execvp with the parent environment preserved. Returns 0 on
 * success, 127 if the binary was not found, the exit code otherwise,
 * or -1 on fork/wait failure. */
static int run_cmd_inherit_env(const char *argv[])
{
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return -1; }
        if (pid == 0) {
                execvp(argv[0], (char *const *)argv);
                _exit(127);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return -1; }
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return -1;
}

/* fork + sh -c with a clean environment plus $OUT, $out, and $PATH.
 * The parent environment is not disturbed (clearenv runs in the child
 * only). Returns 0/127/exit/-1. */
static int run_sh_clean_env(const char *command, const char *out_val)
{
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return -1; }
        if (pid == 0) {
                clearenv();
                setenv("OUT", out_val, 1);
                setenv("out", out_val, 1);
                setenv("PATH", "/usr/bin:/bin", 1);
                const char *argv[] = {"sh", "-c", command, NULL};
                execvp("sh", (char *const *)argv);
                _exit(127);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return -1; }
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return -1;
}

/* ── Built-in hook runners ─────────────────────────────────────────── */

/* Each returns 0 on success, 127 if the binary is missing, non-zero on
 * failure. The temp directory $out_dir is populated on success. */

static int run_gtk_icon_cache(const char *out_dir)
{
        const char *argv[] = {"gtk-update-icon-cache", "-f",
                              "/usr/share/icons/hicolor", NULL};
        int rc = run_cmd_inherit_env(argv);
        if (rc == 127) return 127;
        if (rc != 0) return rc;

        char src[PATH_MAX], dst[PATH_MAX];
        snprintf(src, sizeof(src), "%s/icon-theme.cache",
                 "/usr/share/icons/hicolor");
        snprintf(dst, sizeof(dst), "%s/icon-theme.cache", out_dir);
        if (copy_file(src, dst) != 0) return -1;
        return 0;
}

static int run_desktop_database(const char *out_dir)
{
        const char *argv[] = {"update-desktop-database", "-q",
                              "/usr/share/applications", NULL};
        int rc = run_cmd_inherit_env(argv);
        if (rc == 127) return 127;
        if (rc != 0) return rc;

        char dst_dir[PATH_MAX];
        snprintf(dst_dir, sizeof(dst_dir), "%s/share/applications", out_dir);
        mkdir_p(dst_dir);

        char src[PATH_MAX], dst[PATH_MAX];
        snprintf(src, sizeof(src), "%s/mimeinfo.cache",
                 "/usr/share/applications");
        snprintf(dst, sizeof(dst), "%s/share/applications/mimeinfo.cache",
                 out_dir);
        if (copy_file(src, dst) != 0) return -1;
        return 0;
}

static int run_font_cache(const char *out_dir)
{
        const char *argv[] = {"fc-cache", "-f", NULL};
        int rc = run_cmd_inherit_env(argv);
        if (rc == 127) return 127;
        if (rc != 0) return rc;

        char dst_dir[PATH_MAX];
        snprintf(dst_dir, sizeof(dst_dir), "%s/fontconfig", out_dir);
        int n = copy_dir_files("/var/cache/fontconfig", dst_dir);
        if (n <= 0) return -1;
        return 0;
}

static int run_ldconfig(const char *out_dir)
{
        char dst[PATH_MAX];
        snprintf(dst, sizeof(dst), "%s/ld.so.cache", out_dir);
        const char *argv[] = {"ldconfig", "-f", dst, NULL};
        int rc = run_cmd_inherit_env(argv);
        if (rc == 127) return 127;
        return rc;
}

/* ── Hook output finalization ──────────────────────────────────────── */

/* NAR-hash the temp dir, compute the store path, rename atomically,
 * and append a {name, store_path, nar_hash, dest} object to hooks_arr.
 * Returns 0 on success, 1 if the hook was skipped (empty output), -1
 * on error. dry_run prints what would happen but does not execute. */
static int finalize_hook_output(const char *temp_dir, const char *name,
                                const char *dest, cJSON *hooks_arr,
                                int dry_run)
{
        /* Empty output: skip silently. */
        if (dir_is_empty(temp_dir)) {
                rmtree(temp_dir);
                if (dry_run)
                        fprintf(stderr, "  %s[dry-run]%s hook %s produced no "
                                "output, skipping\n",
                                C_DIM(), C_RESET(), name);
                return 1;
        }

        /* NAR-hash the temp dir. */
        char nar_hash[65];
        size_t nar_size = 0;
        if (nar_hash_directory(temp_dir, nar_hash, &nar_size) != 0) {
                fprintf(stderr, "%sprofile-hooks:%s nar_hash_directory(%s) "
                        "failed: %s\n",
                        C_YELLOW(), C_RESET(), temp_dir, strerror(errno));
                rmtree(temp_dir);
                return -1;
        }

        /* Compute the final store path: /nix/store/<base32>-hook-<name> */
        char *final_path = compute_store_path(nar_hash, "hook", name);
        if (!final_path) {
                fprintf(stderr, "%sprofile-hooks:%s compute_store_path failed "
                        "for %s\n", C_YELLOW(), C_RESET(), name);
                rmtree(temp_dir);
                return -1;
        }

        if (dry_run) {
                fprintf(stderr, "  %s[dry-run]%s would move %s -> %s "
                        "(nar %s, %zu bytes)\n",
                        C_DIM(), C_RESET(), temp_dir, final_path,
                        nar_hash, nar_size);
                cJSON *h = cJSON_CreateObject();
                cJSON_AddStringToObject(h, "name", name);
                cJSON_AddStringToObject(h, "store_path", final_path);
                cJSON_AddStringToObject(h, "nar_hash", nar_hash);
                if (dest) cJSON_AddStringToObject(h, "dest", dest);
                else cJSON_AddNullToObject(h, "dest");
                cJSON_AddItemToArray(hooks_arr, h);
                free(final_path);
                return 0;
        }

        /* Idempotency: if the final path already exists, drop the temp
         * dir and reuse the existing one. Same content hash means the
         * output is byte-identical. */
        struct stat st;
        if (stat(final_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                rmtree(temp_dir);
                fprintf(stderr, "  %sprofile-hooks:%s hook %s already in store "
                        "(%s)\n", C_DIM(), C_RESET(), name, final_path);
        } else if (rename(temp_dir, final_path) != 0) {
                fprintf(stderr, "%sprofile-hooks:%s rename %s -> %s failed: "
                        "%s\n", C_YELLOW(), C_RESET(),
                        temp_dir, final_path, strerror(errno));
                rmtree(temp_dir);
                free(final_path);
                return -1;
        }

        cJSON *h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "name", name);
        cJSON_AddStringToObject(h, "store_path", final_path);
        cJSON_AddStringToObject(h, "nar_hash", nar_hash);
        if (dest) cJSON_AddStringToObject(h, "dest", dest);
        else cJSON_AddNullToObject(h, "dest");
        cJSON_AddItemToArray(hooks_arr, h);

        fprintf(stderr, "  %sprofile-hooks:%s hook %s -> %s%s%s (%zu bytes)\n",
                C_GREEN(), C_RESET(), name, C_DIM(), final_path, C_RESET(),
                nar_size);
        free(final_path);
        return 0;
}

/* ── Per-hook runner ───────────────────────────────────────────────── */

/* Infer the symlink-farm destination for a built-in hook name. Returns
 * NULL for unknown names (custom hooks must declare their own dest). */
static const char *builtin_dest(const char *name)
{
        if (strcmp(name, "gtk-icon-cache") == 0)
                return "/usr/share/icons/hicolor/icon-theme.cache";
        if (strcmp(name, "desktop-database") == 0)
                return "/usr/share/applications/mimeinfo.cache";
        if (strcmp(name, "font-cache") == 0)
                return "/var/cache/fontconfig";
        if (strcmp(name, "ldconfig") == 0)
                return "/etc/ld.so.cache";
        return NULL;
}

static int is_builtin_name(const char *name)
{
        return strcmp(name, "gtk-icon-cache") == 0 ||
               strcmp(name, "desktop-database") == 0 ||
               strcmp(name, "font-cache") == 0 ||
               strcmp(name, "ldconfig") == 0;
}

/* Run one hook (built-in or custom). Returns 0 on success, 1 if the
 * hook was skipped (binary missing, command failed, empty output), -1
 * on error. Appends to hooks_arr on success or dry-run. */
static int run_hook(cJSON *hooks_arr, const char *name, int is_builtin,
                    const char *command, const char *dest_override,
                    int dry_run)
{
        const char *dest = dest_override;
        if (!dest && is_builtin) dest = builtin_dest(name);

        /* Dry-run: print what would happen and record a placeholder. */
        if (dry_run) {
                char safe_name[128];
                sanitize_name(safe_name, sizeof(safe_name), name);
                char fake_path[PATH_MAX];
                snprintf(fake_path, sizeof(fake_path),
                         "%s/<hash>-hook-%s", STORE_ROOT, safe_name);
                fprintf(stderr, "  %s[dry-run]%s hook %s -> %s%s%s\n",
                        C_DIM(), C_RESET(), name,
                        dest ? C_DIM() : "", dest ? dest : "(no dest)",
                        dest ? C_RESET() : "");
                if (!is_builtin && command)
                        fprintf(stderr, "  %s[dry-run]%s   sh -c '%s'\n",
                                C_DIM(), C_RESET(), command);
                cJSON *h = cJSON_CreateObject();
                cJSON_AddStringToObject(h, "name", name);
                cJSON_AddStringToObject(h, "store_path", fake_path);
                cJSON_AddStringToObject(h, "nar_hash", "(dry-run)");
                if (dest) cJSON_AddStringToObject(h, "dest", dest);
                else cJSON_AddNullToObject(h, "dest");
                cJSON_AddItemToArray(hooks_arr, h);
                return 0;
        }

        /* Non-dry-run: create temp dir and run the hook. */
        char safe_name[128];
        sanitize_name(safe_name, sizeof(safe_name), name);

        if (mkdir_p(STORE_TMP_DIR) != 0) {
                fprintf(stderr, "%sprofile-hooks:%s cannot create %s: %s\n",
                        C_YELLOW(), C_RESET(), STORE_TMP_DIR, strerror(errno));
                return -1;
        }
        char temp_dir[PATH_MAX];
        snprintf(temp_dir, sizeof(temp_dir), "%s/hook-%s-%d",
                 STORE_TMP_DIR, safe_name, (int)getpid());
        rmtree(temp_dir);
        if (mkdir(temp_dir, 0755) != 0) {
                fprintf(stderr, "%sprofile-hooks:%s mkdir %s failed: %s\n",
                        C_YELLOW(), C_RESET(), temp_dir, strerror(errno));
                return -1;
        }

        int rc;
        if (is_builtin) {
                if (strcmp(name, "gtk-icon-cache") == 0)
                        rc = run_gtk_icon_cache(temp_dir);
                else if (strcmp(name, "desktop-database") == 0)
                        rc = run_desktop_database(temp_dir);
                else if (strcmp(name, "font-cache") == 0)
                        rc = run_font_cache(temp_dir);
                else if (strcmp(name, "ldconfig") == 0)
                        rc = run_ldconfig(temp_dir);
                else {
                        fprintf(stderr, "%sprofile-hooks:%s unknown built-in "
                                "%s\n", C_YELLOW(), C_RESET(), name);
                        rmtree(temp_dir);
                        return 1;
                }
        } else {
                rc = run_sh_clean_env(command, temp_dir);
        }

        /* Binary missing: silently skip. */
        if (rc == 127) {
                fprintf(stderr, "  %sprofile-hooks:%s %s not installed "
                        "(skipped)\n", C_DIM(), C_RESET(), name);
                rmtree(temp_dir);
                return 1;
        }
        /* Command failed: warn and skip. */
        if (rc != 0) {
                fprintf(stderr, "%sprofile-hooks:%s hook %s failed (exit %d), "
                        "skipping\n",
                        C_YELLOW(), C_RESET(), name, rc);
                rmtree(temp_dir);
                return 1;
        }

        return finalize_hook_output(temp_dir, name, dest, hooks_arr, dry_run);
}

/* ── Main entry ────────────────────────────────────────────────────── */

int profile_hooks_apply(const char *manifest_json, const char *db_root,
                        int gen_id, int dry_run)
{
        if (getuid() != 0) {
                fprintf(stderr, "%sprofile-hooks:%s must run as root "
                        "(writes to /nix/store, runs system commands)\n",
                        C_YELLOW(), C_RESET());
                return 1;
        }
        if (!manifest_json) {
                fprintf(stderr, "%sprofile-hooks:%s manifest_json is NULL\n",
                        C_YELLOW(), C_RESET());
                return 1;
        }

        cJSON *root = cJSON_Parse(manifest_json);
        if (!root) {
                fprintf(stderr, "%sprofile-hooks:%s failed to parse manifest "
                        "JSON\n", C_YELLOW(), C_RESET());
                return 1;
        }

        cJSON *hooks_arr = cJSON_CreateArray();
        cJSON *ph = cJSON_GetObjectItem(root, "profileHooks");

        if (!ph || cJSON_IsNull(ph)) {
                /* No profileHooks block: run the three default built-ins.
                 * ldconfig stays off (DESIGN.md §7: store doesn't need it). */
                run_hook(hooks_arr, "gtk-icon-cache", 1, NULL, NULL, dry_run);
                run_hook(hooks_arr, "desktop-database", 1, NULL, NULL, dry_run);
                run_hook(hooks_arr, "font-cache", 1, NULL, NULL, dry_run);
        } else if (cJSON_IsObject(ph)) {
                cJSON *entry;
                cJSON_ArrayForEach(entry, ph) {
                        const char *name = entry->string;
                        if (!name || !name[0]) continue;

                        if (cJSON_IsBool(entry)) {
                                if (!cJSON_IsTrue(entry)) continue;
                                if (!is_builtin_name(name)) {
                                        fprintf(stderr, "%sprofile-hooks:%s "
                                                "unknown built-in %s, "
                                                "skipping\n",
                                                C_YELLOW(), C_RESET(), name);
                                        continue;
                                }
                                run_hook(hooks_arr, name, 1, NULL, NULL,
                                         dry_run);
                        } else if (cJSON_IsObject(entry)) {
                                cJSON *cmd = cJSON_GetObjectItem(entry,
                                                                 "command");
                                cJSON *dest = cJSON_GetObjectItem(entry,
                                                                   "dest");
                                const char *cmd_str =
                                        cJSON_IsString(cmd) ?
                                                cmd->valuestring : NULL;
                                const char *dest_str =
                                        cJSON_IsString(dest) ?
                                                dest->valuestring : NULL;
                                if (!cmd_str) {
                                        fprintf(stderr, "%sprofile-hooks:%s "
                                                "custom hook %s missing "
                                                "'command', skipping\n",
                                                C_YELLOW(), C_RESET(), name);
                                        continue;
                                }
                                run_hook(hooks_arr, name, 0, cmd_str, dest_str,
                                         dry_run);
                        }
                }
        }
        /* If profileHooks is present but not an object (e.g. array or
         * scalar), treat it as no hooks. The malformed config is the
         * user's problem; we just don't run anything. */

        cJSON_Delete(root);

        /* Write hooks.json to the generation directory. */
        if (!dry_run && db_root && gen_id > 0) {
                char dir[PATH_MAX];
                snprintf(dir, sizeof(dir), "%s/generations/%d",
                         db_root, gen_id);
                mkdir_p(dir);

                /* Build path separately so snprintf doesn't have to
                 * chain two PATH_MAX buffers. */
                char path[PATH_MAX + 32];
                snprintf(path, sizeof(path), "%s/hooks.json", dir);

                cJSON *out = cJSON_CreateObject();
                cJSON_AddNumberToObject(out, "generation", gen_id);
                cJSON_AddItemToObject(out, "hooks", hooks_arr);
                char *json_str = cJSON_Print(out);
                if (json_str) {
                        FILE *f = fopen(path, "w");
                        if (f) {
                                fputs(json_str, f);
                                fputc('\n', f);
                                fclose(f);
                        } else {
                                fprintf(stderr, "%sprofile-hooks:%s cannot "
                                        "write %s: %s\n",
                                        C_YELLOW(), C_RESET(), path,
                                        strerror(errno));
                        }
                        free(json_str);
                }
                cJSON_Delete(out);
        } else {
                cJSON_Delete(hooks_arr);
        }

        return 0;
}
