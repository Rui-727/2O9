/* users.c - declarative user and group management (Phase 4)
 *
 * Applies the `users` and `groups` attrsets from a 2O9 manifest to the
 * live system. Idempotent: every `209 apply` reconciles the declared
 * set against what is on disk. Groups first, then users, then
 * passwords, then supplementary group memberships.
 *
 * Model:
 * - Each group in `groups` becomes a system group: groupadd if new,
 *   groupmod -g if the gid changed.
 * - Each user in `users` becomes a system user: useradd if new, usermod
 *   for fields that changed.
 * - A user's primary `group` is auto-created with gid = uid if it is
 *   not declared explicitly in `groups` (the common "rui/rui 1000"
 *   pattern).
 * - Supplementary groups come from two places: the user's `groups`
 *   list, and `members` arrays on each group. Both translate to
 *   `gpasswd -a`. Groups the user is in but no longer declared in
 *   either place get `gpasswd -d`.
 * - hashedPassword / password go through `chpasswd -e` / `chpasswd`
 *   via stdin. Plaintext passwords print a warning.
 *
 * Removal:
 * - Users and groups that existed in the previous manifest but not the
 *   current one are removed after a [y/N] prompt. userdel with files
 *   in the home dir gets a second prompt.
 *
 * Permissions:
 * - All of this needs root. users_apply errors out early if getuid()
 *   is not 0.
 *
 * Commands go through system() with single-quote-escaped args. The
 * config lives under /nix/config/ and is root-owned, so shell
 * injection is not a real threat; the quoting is still there as
 * defense in depth.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <grp.h>
#include <pwd.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>

#include "users.h"
#include "cJSON.h"

/* Cap supplementary groups we track per user. 64 is well above the
 * Linux default of 32 (NGROUPS_MAX) and generous enough for any
 * realistic 2O9 config. */
#define MAX_SUPP_GROUPS 64

/* ── Color helpers (self-contained - main.c's are static) ─────────── */

static int want_color(void)
{
        static int v = -1;
        if (v < 0) v = isatty(STDERR_FILENO);
        return v;
}
static const char *C_RESET(void)  { return want_color() ? "\033[0m"  : ""; }
static const char *C_GREEN(void)  { return want_color() ? "\033[32m" : ""; }
static const char *C_RED(void)    { return want_color() ? "\033[31m" : ""; }
static const char *C_YELLOW(void) { return want_color() ? "\033[33m" : ""; }
static const char *C_DIM(void)    { return want_color() ? "\033[2m"  : ""; }
static const char *C_BOLD(void)   { return want_color() ? "\033[1m"  : ""; }

/* ── Shell quoting ────────────────────────────────────────────────── */

/* Wrap src in single quotes for /bin/sh. Embedded single quotes are
 * escaped via the '\'' idiom. dest must hold at least 2*strlen(src)+3.
 * Returns dest on success, NULL on overflow. */
static char *shell_quote(char *dest, size_t n, const char *src)
{
        if (!src) src = "";
        size_t i = 0;
        if (i + 1 >= n) return NULL;
        dest[i++] = '\'';
        for (const char *p = src; *p; p++) {
                if (*p == '\'') {
                        if (i + 4 >= n) return NULL;
                        dest[i++] = '\''; dest[i++] = '\\';
                        dest[i++] = '\''; dest[i++] = '\'';
                } else {
                        if (i + 1 >= n) return NULL;
                        dest[i++] = *p;
                }
        }
        if (i + 2 >= n) return NULL;
        dest[i++] = '\'';
        dest[i] = '\0';
        return dest;
}

/* ── Command execution ────────────────────────────────────────────── */

/* Build and run a shell command via system(). Returns 0 on success,
 * non-zero on failure. dry_run prints the command instead of running
 * it. */
static int runf(int dry_run, const char *why, const char *fmt, ...)
{
        char cmd[8192];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(cmd, sizeof(cmd), fmt, ap);
        va_end(ap);
        if (n < 0 || (size_t)n >= sizeof(cmd)) {
                fprintf(stderr, "%susers:%s command too long: %s\n",
                        C_RED(), C_RESET(), why);
                return -1;
        }
        if (dry_run) {
                fprintf(stderr, "  %s[dry-run]%s %s\n",
                        C_DIM(), C_RESET(), cmd);
                return 0;
        }
        fprintf(stderr, "  %srun%s %s\n", C_DIM(), C_RESET(), cmd);
        int rc = system(cmd);
        if (rc == -1) {
                fprintf(stderr, "%susers:%s system() failed: %s\n",
                        C_RED(), C_RESET(), why);
                return -1;
        }
        if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) return 0;
        int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        fprintf(stderr, "%susers:%s %s failed (exit %d)\n",
                C_RED(), C_RESET(), why, code);
        return code ? code : -1;
}

/* Run a command and write a string to its stdin (for chpasswd).
 * Returns 0 on success, non-zero on failure. dry_run prints the
 * command with the input size (never the input itself). */
static int run_with_stdin(int dry_run, const char *cmd_str,
                          const char *input, const char *why)
{
        if (dry_run) {
                fprintf(stderr, "  %s[dry-run]%s %s (stdin: %zu bytes)\n",
                        C_DIM(), C_RESET(), cmd_str, strlen(input));
                return 0;
        }
        fprintf(stderr, "  %srun%s %s\n", C_DIM(), C_RESET(), cmd_str);
        FILE *p = popen(cmd_str, "w");
        if (!p) {
                fprintf(stderr, "%susers:%s popen failed: %s\n",
                        C_RED(), C_RESET(), why);
                return -1;
        }
        if (fputs(input, p) == EOF || fputc('\n', p) == EOF) {
                pclose(p);
                fprintf(stderr, "%susers:%s write to stdin failed: %s\n",
                        C_RED(), C_RESET(), why);
                return -1;
        }
        int rc = pclose(p);
        if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) return 0;
        int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        fprintf(stderr, "%susers:%s %s failed (exit %d)\n",
                C_RED(), C_RESET(), why, code);
        return code ? code : -1;
}

/* ── passwd/group lookups ─────────────────────────────────────────── */

static int group_exists(const char *name)
{
        if (!name || !*name) return 0;
        errno = 0;
        return getgrnam(name) != NULL;
}

static int user_exists(const char *name)
{
        if (!name || !*name) return 0;
        errno = 0;
        return getpwnam(name) != NULL;
}

static gid_t group_get_gid(const char *name)
{
        if (!name) return (gid_t)-1;
        errno = 0;
        struct group *g = getgrnam(name);
        return g ? g->gr_gid : (gid_t)-1;
}

static uid_t user_get_uid(const char *name)
{
        if (!name) return (uid_t)-1;
        errno = 0;
        struct passwd *p = getpwnam(name);
        return p ? p->pw_uid : (uid_t)-1;
}

/* Return the user's primary group name, or NULL if the user is gone
 * or the gid has no matching group entry. */
static const char *user_primary_group_name(const char *name,
                                           char *buf, size_t buflen)
{
        if (!name) return NULL;
        errno = 0;
        struct passwd *p = getpwnam(name);
        if (!p) return NULL;
        errno = 0;
        struct group *g = getgrgid(p->pw_gid);
        if (!g || !g->gr_name) return NULL;
        snprintf(buf, buflen, "%s", g->gr_name);
        return buf;
}

/* Write the user's supplementary group names into out[]. Returns the
 * total count (may exceed cap, in which case only the first cap are
 * written). Caller frees each non-NULL entry with free(). Names come
 * from getgrent() iteration over /etc/group. */
static int user_supp_groups(const char *name, char **out, int cap)
{
        int n = 0;
        if (!name) return 0;
        setgrent();
        struct group *g;
        while ((g = getgrent()) != NULL) {
                if (!g->gr_mem || !g->gr_name) continue;
                for (char **m = g->gr_mem; *m; m++) {
                        if (strcmp(*m, name) == 0) {
                                if (n < cap) out[n] = strdup(g->gr_name);
                                n++;
                                break;
                        }
                }
        }
        endgrent();
        return n;
}

/* ── Prompt ───────────────────────────────────────────────────────── */

static int prompt_yes_no(const char *prompt, int default_yes)
{
        fprintf(stderr, "  %s%s%s %s ",
                C_BOLD(), prompt, C_RESET(),
                default_yes ? "[Y/n]" : "[y/N]");
        fflush(stderr);
        char buf[16];
        if (!fgets(buf, sizeof(buf), stdin)) return 0;
        if (buf[0] == 'y' || buf[0] == 'Y') return 1;
        if (buf[0] == 'n' || buf[0] == 'N') return 0;
        return default_yes;
}

/* ── Apply groups ─────────────────────────────────────────────────── */

static int apply_groups(cJSON *groups, int dry_run)
{
        int errors = 0;
        cJSON *g;
        cJSON_ArrayForEach(g, groups) {
                const char *name = g->string;
                if (!name || !cJSON_IsObject(g)) continue;

                cJSON *jgid = cJSON_GetObjectItem(g, "gid");
                long gid = (jgid && cJSON_IsNumber(jgid))
                        ? (long)jgid->valuedouble : -1;

                char qname[256];
                if (!shell_quote(qname, sizeof(qname), name)) {
                        fprintf(stderr, "%susers:%s group name too long: %s\n",
                                C_RED(), C_RESET(), name);
                        errors++;
                        continue;
                }

                if (!group_exists(name)) {
                        if (gid >= 0) {
                                if (runf(dry_run, "groupadd",
                                         "groupadd -g %ld %s", gid, qname) != 0)
                                        errors++;
                        } else {
                                if (runf(dry_run, "groupadd",
                                         "groupadd %s", qname) != 0)
                                        errors++;
                        }
                } else if (gid >= 0) {
                        gid_t cur = group_get_gid(name);
                        if (cur != (gid_t)gid) {
                                if (runf(dry_run, "groupmod -g",
                                         "groupmod -g %ld %s", gid, qname) != 0)
                                        errors++;
                        }
                }
                /* gid matches or no gid declared: skip */
        }
        return errors;
}

/* ── Apply supplementary groups for one user ──────────────────────── */

static int apply_supp_groups(const char *name, cJSON *user_decl,
                             cJSON *groups, int dry_run)
{
        int errors = 0;
        const char *desired[MAX_SUPP_GROUPS];
        int n_desired = 0;

        /* From the user's own `groups` list */
        cJSON *jgroups = cJSON_GetObjectItem(user_decl, "groups");
        if (jgroups && cJSON_IsArray(jgroups)) {
                cJSON *gn;
                cJSON_ArrayForEach(gn, jgroups) {
                        if (cJSON_IsString(gn) && n_desired < MAX_SUPP_GROUPS)
                                desired[n_desired++] = gn->valuestring;
                }
        }

        /* From `members` arrays on each declared group */
        if (groups && cJSON_IsObject(groups)) {
                cJSON *gd;
                cJSON_ArrayForEach(gd, groups) {
                        cJSON *jm = cJSON_GetObjectItem(gd, "members");
                        if (!jm || !cJSON_IsArray(jm)) continue;
                        cJSON *m;
                        cJSON_ArrayForEach(m, jm) {
                                if (!cJSON_IsString(m)) continue;
                                if (strcmp(m->valuestring, name) != 0) continue;
                                int found = 0;
                                for (int i = 0; i < n_desired; i++) {
                                        if (strcmp(desired[i], gd->string) == 0) {
                                                found = 1; break;
                                        }
                                }
                                if (!found && n_desired < MAX_SUPP_GROUPS)
                                        desired[n_desired++] = gd->string;
                                break;
                        }
                }
        }

        char quser[256];
        if (!shell_quote(quser, sizeof(quser), name)) return 1;

        /* Add to every desired group */
        for (int i = 0; i < n_desired; i++) {
                char qg[256];
                if (!shell_quote(qg, sizeof(qg), desired[i])) continue;
                if (runf(dry_run, "gpasswd -a",
                         "gpasswd -a %s %s", quser, qg) != 0)
                        errors++;
        }

        /* Remove from groups no longer desired (skip the primary group) */
        char pbuf[256];
        const char *primary = user_primary_group_name(name, pbuf, sizeof(pbuf));
        char *current[MAX_SUPP_GROUPS] = {0};
        int n_current = user_supp_groups(name, current, MAX_SUPP_GROUPS);
        for (int i = 0; i < n_current && i < MAX_SUPP_GROUPS; i++) {
                const char *g = current[i];
                if (!g) continue;
                if (primary && strcmp(g, primary) == 0) continue;
                int found = 0;
                for (int j = 0; j < n_desired; j++) {
                        if (strcmp(desired[j], g) == 0) { found = 1; break; }
                }
                if (found) continue;
                char qg[256];
                if (!shell_quote(qg, sizeof(qg), g)) continue;
                if (runf(dry_run, "gpasswd -d",
                         "gpasswd -d %s %s", quser, qg) != 0)
                        errors++;
        }
        for (int i = 0; i < n_current && i < MAX_SUPP_GROUPS; i++)
                free(current[i]);

        return errors;
}

/* ── Apply users ──────────────────────────────────────────────────── */

static int apply_users(cJSON *users, cJSON *groups, int dry_run)
{
        int errors = 0;
        cJSON *u;
        cJSON_ArrayForEach(u, users) {
                const char *name = u->string;
                if (!name || !cJSON_IsObject(u)) continue;

                cJSON *juid    = cJSON_GetObjectItem(u, "uid");
                cJSON *jgroup  = cJSON_GetObjectItem(u, "group");
                cJSON *jshell  = cJSON_GetObjectItem(u, "shell");
                cJSON *jhome   = cJSON_GetObjectItem(u, "home");
                cJSON *jdesc   = cJSON_GetObjectItem(u, "description");
                cJSON *jnormal = cJSON_GetObjectItem(u, "isNormalUser");
                cJSON *jsys    = cJSON_GetObjectItem(u, "isSystemUser");
                cJSON *jcreate = cJSON_GetObjectItem(u, "createHome");
                cJSON *jhash   = cJSON_GetObjectItem(u, "hashedPassword");
                cJSON *jpass   = cJSON_GetObjectItem(u, "password");

                long uid = (juid && cJSON_IsNumber(juid))
                        ? (long)juid->valuedouble : -1;
                int is_normal = (jnormal && cJSON_IsTrue(jnormal)) ? 1 : 0;
                int is_sys    = (jsys && cJSON_IsTrue(jsys)) ? 1 : 0;

                int create_home;
                if (jcreate && cJSON_IsBool(jcreate))
                        create_home = cJSON_IsTrue(jcreate);
                else
                        create_home = is_normal;

                /* Defaults */
                const char *group = (jgroup && cJSON_IsString(jgroup))
                        ? jgroup->valuestring : name;
                const char *shell = (jshell && cJSON_IsString(jshell))
                        ? jshell->valuestring
                        : (is_sys ? "/sbin/nologin" : "/bin/bash");
                const char *desc = (jdesc && cJSON_IsString(jdesc))
                        ? jdesc->valuestring : NULL;

                /* Home: declared wins; otherwise normal users get
                 * /home/<name>, system users get nothing (omit -d). */
                const char *home = (jhome && cJSON_IsString(jhome))
                        ? jhome->valuestring : NULL;
                char homebuf[PATH_MAX];
                if (!home && !is_sys) {
                        snprintf(homebuf, sizeof(homebuf), "/home/%s", name);
                        home = homebuf;
                }

                /* Auto-create primary group if not declared in `groups`
                 * and not already present on the system. */
                if (group && !group_exists(group)) {
                        cJSON *decl = groups ? cJSON_GetObjectItem(groups, group) : NULL;
                        if (!decl) {
                                long ggid = (uid >= 0) ? uid : -1;
                                char qg[256];
                                if (!shell_quote(qg, sizeof(qg), group)) {
                                        fprintf(stderr, "%susers:%s group name too long: %s\n",
                                                C_RED(), C_RESET(), group);
                                        errors++;
                                        continue;
                                }
                                if (ggid >= 0)
                                        runf(dry_run, "groupadd (auto)",
                                             "groupadd -g %ld %s", ggid, qg);
                                else
                                        runf(dry_run, "groupadd (auto)",
                                             "groupadd %s", qg);
                        }
                        /* If the group is declared in `groups`,
                         * apply_groups already created it. If the
                         * auto-create failed, useradd will fail with
                         * a clear error from the system. */
                }

                char qname[256], qgroup[256], qhome[PATH_MAX * 2 + 3],
                     qshell[256], qdesc[1024];
                if (!shell_quote(qname, sizeof(qname), name) ||
                    !shell_quote(qgroup, sizeof(qgroup), group ? group : "") ||
                    !shell_quote(qshell, sizeof(qshell), shell ? shell : "") ||
                    (desc && !shell_quote(qdesc, sizeof(qdesc), desc))) {
                        fprintf(stderr, "%susers:%s field too long for user: %s\n",
                                C_RED(), C_RESET(), name);
                        errors++;
                        continue;
                }
                if (home && !shell_quote(qhome, sizeof(qhome), home)) {
                        fprintf(stderr, "%susers:%s home path too long: %s\n",
                                C_RED(), C_RESET(), name);
                        errors++;
                        continue;
                }

                if (!user_exists(name)) {
                        /* useradd */
                        char opt[1024];
                        opt[0] = '\0';
                        size_t off = 0;
                        if (uid >= 0)
                                off += snprintf(opt + off, sizeof(opt) - off,
                                                " -u %ld", uid);
                        if (group)
                                off += snprintf(opt + off, sizeof(opt) - off,
                                                " -g %s", qgroup);
                        if (home)
                                off += snprintf(opt + off, sizeof(opt) - off,
                                                " -d %s", qhome);
                        off += snprintf(opt + off, sizeof(opt) - off,
                                        " -s %s", qshell);
                        if (desc)
                                off += snprintf(opt + off, sizeof(opt) - off,
                                                " -c %s", qdesc);
                        const char *home_opt = create_home ? " -m" : " --no-create-home";
                        if (runf(dry_run, "useradd",
                                 "useradd%s %s %s", opt, home_opt, qname) != 0)
                                errors++;
                } else {
                        /* usermod - update fields that changed */
                        char opt[1024];
                        opt[0] = '\0';
                        size_t off = 0;
                        if (uid >= 0 && user_get_uid(name) != (uid_t)uid)
                                off += snprintf(opt + off, sizeof(opt) - off,
                                                " -u %ld", uid);
                        if (group) {
                                char pbuf[256];
                                const char *cur = user_primary_group_name(name,
                                        pbuf, sizeof(pbuf));
                                if (!cur || strcmp(cur, group) != 0)
                                        off += snprintf(opt + off, sizeof(opt) - off,
                                                        " -g %s", qgroup);
                        }
                        if (home)
                                off += snprintf(opt + off, sizeof(opt) - off,
                                                " -d %s", qhome);
                        off += snprintf(opt + off, sizeof(opt) - off,
                                        " -s %s", qshell);
                        if (desc)
                                off += snprintf(opt + off, sizeof(opt) - off,
                                                " -c %s", qdesc);
                        if (opt[0]) {
                                if (runf(dry_run, "usermod",
                                         "usermod%s %s", opt, qname) != 0)
                                        errors++;
                        }
                }

                /* Passwords */
                if (jhash && cJSON_IsString(jhash)) {
                        char line[8192];
                        int ln = snprintf(line, sizeof(line), "%s:%s",
                                          name, jhash->valuestring);
                        if (ln < 0 || (size_t)ln >= sizeof(line)) {
                                fprintf(stderr, "%susers:%s hashed password too long for: %s\n",
                                        C_RED(), C_RESET(), name);
                                errors++;
                        } else if (run_with_stdin(dry_run, "chpasswd -e",
                                                  line, "set hashed password") != 0)
                                errors++;
                } else if (jpass && cJSON_IsString(jpass)) {
                        fprintf(stderr, "%susers:%s warning: plaintext password for '%s' "
                                "(use hashedPassword instead)\n",
                                C_YELLOW(), C_RESET(), name);
                        char line[8192];
                        int ln = snprintf(line, sizeof(line), "%s:%s",
                                          name, jpass->valuestring);
                        if (ln < 0 || (size_t)ln >= sizeof(line)) {
                                fprintf(stderr, "%susers:%s password too long for: %s\n",
                                        C_RED(), C_RESET(), name);
                                errors++;
                        } else if (run_with_stdin(dry_run, "chpasswd",
                                                  line, "set plaintext password") != 0)
                                errors++;
                }

                /* Supplementary groups (skip if user doesn't exist and
                 * we're not in dry-run, so gpasswd doesn't error out). */
                if (user_exists(name) || dry_run) {
                        if (apply_supp_groups(name, u, groups, dry_run) != 0)
                                errors++;
                }
        }
        return errors;
}

/* ── Removal of users/groups absent from the current manifest ─────── */

static int home_has_files(const char *home)
{
        if (!home) return 0;
        DIR *d = opendir(home);
        if (!d) return 0;
        struct dirent *e;
        int found = 0;
        while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") == 0 ||
                    strcmp(e->d_name, "..") == 0)
                        continue;
                found = 1;
                break;
        }
        closedir(d);
        return found;
}

static int remove_prev_users(cJSON *prev_users, cJSON *cur_users, int dry_run)
{
        int errors = 0;
        if (!prev_users) return 0;
        cJSON *u;
        cJSON_ArrayForEach(u, prev_users) {
                const char *name = u->string;
                if (!name) continue;
                if (cJSON_GetObjectItem(cur_users, name)) continue;

                fprintf(stderr, "%susers:%s would remove user '%s' "
                        "(was in previous generation, not in current)\n",
                        C_YELLOW(), C_RESET(), name);
                if (dry_run) continue;

                if (!prompt_yes_no("remove user?", 0)) {
                        fprintf(stderr, "  %sskipped%s\n", C_DIM(), C_RESET());
                        continue;
                }

                cJSON *jhome = cJSON_GetObjectItem(u, "home");
                const char *home = (jhome && cJSON_IsString(jhome))
                        ? jhome->valuestring : NULL;
                /* If the previous declaration didn't carry a home,
                 * fall back to /home/<name> for the file check. */
                char homebuf[PATH_MAX];
                if (!home) {
                        snprintf(homebuf, sizeof(homebuf), "/home/%s", name);
                        home = homebuf;
                }

                char qname[256];
                if (!shell_quote(qname, sizeof(qname), name)) {
                        errors++;
                        continue;
                }

                if (home_has_files(home)) {
                        fprintf(stderr, "  user '%s' has files in %s\n",
                                name, home);
                        if (!prompt_yes_no("remove anyway (files will be lost)?", 0)) {
                                fprintf(stderr, "  %sskipped%s\n",
                                        C_DIM(), C_RESET());
                                continue;
                        }
                        if (runf(0, "userdel -r",
                                 "userdel -r %s", qname) != 0)
                                errors++;
                } else {
                        if (runf(0, "userdel",
                                 "userdel %s", qname) != 0)
                                errors++;
                }
        }
        return errors;
}

static int remove_prev_groups(cJSON *prev_groups, cJSON *cur_groups, int dry_run)
{
        int errors = 0;
        if (!prev_groups) return 0;
        cJSON *g;
        cJSON_ArrayForEach(g, prev_groups) {
                const char *name = g->string;
                if (!name) continue;
                if (cJSON_GetObjectItem(cur_groups, name)) continue;

                fprintf(stderr, "%susers:%s would remove group '%s' "
                        "(was in previous generation, not in current)\n",
                        C_YELLOW(), C_RESET(), name);
                if (dry_run) continue;

                if (!prompt_yes_no("remove group?", 0)) {
                        fprintf(stderr, "  %sskipped%s\n", C_DIM(), C_RESET());
                        continue;
                }
                char qname[256];
                if (!shell_quote(qname, sizeof(qname), name)) {
                        errors++;
                        continue;
                }
                if (runf(0, "groupdel", "groupdel %s", qname) != 0)
                        errors++;
        }
        return errors;
}

/* ── Public entry point ───────────────────────────────────────────── */

int users_apply(const char *manifest_json,
                const char *prev_manifest_json, int dry_run)
{
        if (getuid() != 0) {
                fprintf(stderr, "%s209:%s users_apply requires root. Run with sudo.\n",
                        C_RED(), C_RESET());
                return -1;
        }
        if (!manifest_json) {
                fprintf(stderr, "%susers:%s no manifest provided\n",
                        C_RED(), C_RESET());
                return -1;
        }

        cJSON *root = cJSON_Parse(manifest_json);
        if (!root) {
                fprintf(stderr, "%susers:%s failed to parse manifest JSON\n",
                        C_RED(), C_RESET());
                return -1;
        }

        cJSON *prev_root = NULL;
        if (prev_manifest_json) {
                prev_root = cJSON_Parse(prev_manifest_json);
                if (!prev_root) {
                        fprintf(stderr, "%susers:%s warning: failed to parse prev manifest, "
                                "skipping removal check\n",
                                C_YELLOW(), C_RESET());
                }
        }

        cJSON *groups      = cJSON_GetObjectItem(root, "groups");
        cJSON *users       = cJSON_GetObjectItem(root, "users");
        cJSON *prev_groups = prev_root ? cJSON_GetObjectItem(prev_root, "groups") : NULL;
        cJSON *prev_users  = prev_root ? cJSON_GetObjectItem(prev_root, "users")  : NULL;

        fprintf(stderr, "=== users and groups phase ===\n");
        if (dry_run)
                fprintf(stderr, "  %s[dry run]%s no changes will be made\n",
                        C_YELLOW(), C_RESET());

        int errors = 0;

        if (groups && cJSON_IsObject(groups))
                errors += apply_groups(groups, dry_run);

        if (users && cJSON_IsObject(users))
                errors += apply_users(users, groups, dry_run);

        /* Removals run after additions so a user that moved between
         * manifests is created in its new spot before the stale entry
         * is touched. */
        if (prev_users)
                errors += remove_prev_users(prev_users, users, dry_run);
        if (prev_groups)
                errors += remove_prev_groups(prev_groups, groups, dry_run);

        cJSON_Delete(root);
        if (prev_root) cJSON_Delete(prev_root);

        if (errors) {
                fprintf(stderr, "%s=== users and groups phase had %d error(s) ===%s\n",
                        C_RED(), errors, C_RESET());
                return -1;
        }
        fprintf(stderr, "%s=== users and groups phase complete ===%s\n",
                C_GREEN(), C_RESET());
        return 0;
}
