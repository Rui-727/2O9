/* services.c - 2O9 declarative systemd service management (Phase 4)
 *
 * Applies the `services` attrset from a 2O9 manifest to systemd. Each
 * entry can declare:
 *   enable (bool)         - whether to enable the service
 *   requires (str list)   - other service names this depends on; used
 *                           for topological ordering and the unit's
 *                           Requires= line
 *   after (str list)      - freeform systemd unit names for After=
 *   before (str list)     - freeform systemd unit names for Before=
 *   restartOnChange (bool)- restart on `209 apply` if already running
 *                           and the generated unit file changed
 *   type (string)         - systemd Service Type=, default "simple"
 *   execStart (string)    - command to run. When set, 2O9 generates
 *                           the unit file at
 *                           /etc/systemd/system/<name>.service.
 *                           When not set, 2O9 just enables an existing
 *                           system service (sshd, NetworkManager, ...).
 *   environment (attrset) - Environment= entries
 *
 * Services are topologically sorted by their `requires` edges before
 * activation. Kahn's algorithm (BFS) produces the order. Cycles are
 * detected and reported with the path
 * ("service cycle detected: a -> b -> a"). References to undeclared
 * service names in `requires` are an error
 * ("service 'nginx' requires 'nonexistent' which is not declared").
 *
 * Per service in topological order:
 *  1. If execStart is set, generate the unit file. Write it if the
 *     content differs from the existing file.
 *  2. If enable is true: `systemctl enable <name>`. If the unit file
 *     changed, restartOnChange is set, and the service is running,
 *     `systemctl restart <name>`. Otherwise if the service is not
 *     running, `systemctl start <name>`.
 *  3. If enable is false: `systemctl disable <name>` (exit code
 *     ignored; the service may not have been enabled).
 *
 * Services that existed in the previous manifest but not the current
 * one are disabled, stopped if running, and have their generated unit
 * file removed.
 *
 * After any change, run `systemctl daemon-reload` once.
 *
 * Requires root (systemctl enable/start/stop, writes under
 * /etc/systemd/system/). Checks getuid() == 0 and errors otherwise.
 *
 * The Makefile passes -D_GNU_SOURCE; open_memstream / asprintf / strdup
 * are available because of that.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>

#include "services.h"
#include "cJSON.h"

#define UNIT_DIR "/etc/systemd/system"

/* ── Subprocess and file helpers ─────────────────────────────────────── */

/* fork + execvp. Returns the exit status (0 on success), or -1 on
 * fork/wait failure. Exit 127 means "binary not found"; callers
 * detect and report it. */
static int run_cmd(const char *argv[])
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

/* Read an entire file into a malloc'd buffer. Returns NULL on missing
 * or unreadable file. Caller frees. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Write a file atomically via mkstemp + rename. The temp file is
 * created in the same directory as the target so rename works. */
static int write_file_atomic(const char *path, const char *content, size_t len)
{
    char tmp_path[PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.2O9.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        fprintf(stderr, "services: path too long for temp: %s\n", path);
        return -1;
    }
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        fprintf(stderr, "services: mkstemp for %s failed: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (write(fd, content, len) != (ssize_t)len) {
        fprintf(stderr, "services: write to %s failed: %s\n",
                path, strerror(errno));
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);
    if (chmod(tmp_path, 0644) != 0) {
        /* non-fatal: file is written, mode may be wrong */
    }
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "services: rename %s -> %s failed: %s\n",
                tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/* Validate a service name. systemd unit names allow alphanumerics,
 * '-', '_', '.', '@' (template instances). We reject '@' and any
 * other characters that aren't safe in a filename or unit name. */
static int valid_service_name(const char *name)
{
    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') return 0;
    }
    return 1;
}

/* ── Service model ───────────────────────────────────────────────────── */

typedef struct {
    char  *name;
    int    enable;
    char **requires;
    int    n_requires;
    char **after;
    int    n_after;
    char **before;
    int    n_before;
    int    restart_on_change;
    char  *type;        /* may be NULL; defaults to "simple" in unit */
    char  *exec_start;  /* may be NULL; if NULL, no unit file generated */
    char **env_keys;
    char **env_vals;
    int    n_env;
} svc_t;

typedef struct {
    svc_t *svcs;
    int    n;
    int    cap;
} svc_list_t;

static void svc_free(svc_t *svc)
{
    if (!svc) return;
    free(svc->name);
    free(svc->type);
    free(svc->exec_start);
    for (int i = 0; i < svc->n_requires; i++) free(svc->requires[i]);
    for (int i = 0; i < svc->n_after; i++) free(svc->after[i]);
    for (int i = 0; i < svc->n_before; i++) free(svc->before[i]);
    for (int i = 0; i < svc->n_env; i++) {
        free(svc->env_keys[i]);
        free(svc->env_vals[i]);
    }
    free(svc->requires);
    free(svc->after);
    free(svc->before);
    free(svc->env_keys);
    free(svc->env_vals);
}

static void svc_list_free(svc_list_t *list)
{
    if (!list) return;
    for (int i = 0; i < list->n; i++) svc_free(&list->svcs[i]);
    free(list->svcs);
    list->svcs = NULL;
    list->n = list->cap = 0;
}

/* Find a service by name. Returns its index, or -1. */
static int find_svc(svc_list_t *list, const char *name)
{
    for (int i = 0; i < list->n; i++) {
        if (strcmp(list->svcs[i].name, name) == 0) return i;
    }
    return -1;
}

/* ── cJSON parsing helpers ───────────────────────────────────────────── */

/* Parse a cJSON array of strings into a malloc'd char** array (each
 * entry malloc'd too). Sets *count to the number of entries. Returns
 * NULL if arr is missing/not an array/empty, or on OOM. NULL is a
 * valid "no entries" result; callers treat it as empty. */
static char **parse_str_array(cJSON *arr, int *count)
{
    *count = 0;
    if (!cJSON_IsArray(arr)) return NULL;
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return NULL;
    char **out = calloc((size_t)n, sizeof(char *));
    if (!out) return NULL;
    int k = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (!cJSON_IsString(e)) continue;
        out[k] = strdup(e->valuestring);
        if (!out[k]) {
            for (int i = 0; i < k; i++) free(out[i]);
            free(out);
            return NULL;
        }
        k++;
    }
    *count = k;
    return out;
}

/* Parse environment as parallel key/value arrays. Returns 0 on
 * success (including when env is missing or empty). Returns -1 on
 * OOM. Caller frees *keys_out and *vals_out via svc_free. */
static int parse_environment(cJSON *obj, char ***keys_out, char ***vals_out,
                             int *count_out)
{
    *keys_out = NULL;
    *vals_out = NULL;
    *count_out = 0;
    if (!cJSON_IsObject(obj)) return 0;

    int n = cJSON_GetArraySize(obj);
    if (n <= 0) return 0;

    char **keys = calloc((size_t)n, sizeof(char *));
    char **vals = calloc((size_t)n, sizeof(char *));
    if (!keys || !vals) { free(keys); free(vals); return -1; }

    int k = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, obj) {
        if (!e->string) continue;
        if (!cJSON_IsString(e)) continue;
        keys[k] = strdup(e->string);
        vals[k] = strdup(e->valuestring);
        if (!keys[k] || !vals[k]) {
            for (int i = 0; i < k; i++) { free(keys[i]); free(vals[i]); }
            free(keys); free(vals);
            return -1;
        }
        k++;
    }

    *keys_out = keys;
    *vals_out = vals;
    *count_out = k;
    return 0;
}

/* Parse the `services` object from the manifest into out. Returns 0
 * on success, -1 on OOM. An empty or missing `services` block is not
 * an error; out->n is set to 0. */
static int parse_services(cJSON *root, svc_list_t *out)
{
    out->svcs = NULL;
    out->n = out->cap = 0;

    cJSON *svc_obj = cJSON_GetObjectItem(root, "services");
    if (!svc_obj || !cJSON_IsObject(svc_obj)) return 0;

    cJSON *entry;
    cJSON_ArrayForEach(entry, svc_obj) {
        if (!cJSON_IsObject(entry)) continue;
        if (!entry->string || !entry->string[0]) continue;

        if (out->n == out->cap) {
            int new_cap = out->cap ? out->cap * 2 : 8;
            svc_t *grown = realloc(out->svcs, (size_t)new_cap * sizeof(svc_t));
            if (!grown) {
                fprintf(stderr, "services: out of memory\n");
                return -1;
            }
            out->svcs = grown;
            out->cap = new_cap;
        }

        svc_t *s = &out->svcs[out->n];
        memset(s, 0, sizeof(*s));
        s->name = strdup(entry->string);
        if (!s->name) { fprintf(stderr, "services: out of memory\n"); return -1; }

        cJSON *en = cJSON_GetObjectItem(entry, "enable");
        s->enable = cJSON_IsTrue(en) ? 1 : 0;

        cJSON *req = cJSON_GetObjectItem(entry, "requires");
        s->requires = parse_str_array(req, &s->n_requires);

        cJSON *af = cJSON_GetObjectItem(entry, "after");
        s->after = parse_str_array(af, &s->n_after);

        cJSON *bf = cJSON_GetObjectItem(entry, "before");
        s->before = parse_str_array(bf, &s->n_before);

        cJSON *rc = cJSON_GetObjectItem(entry, "restartOnChange");
        s->restart_on_change = cJSON_IsTrue(rc) ? 1 : 0;

        cJSON *ty = cJSON_GetObjectItem(entry, "type");
        if (cJSON_IsString(ty) && ty->valuestring[0]) {
            s->type = strdup(ty->valuestring);
            if (!s->type) { fprintf(stderr, "services: out of memory\n"); return -1; }
        }

        cJSON *es = cJSON_GetObjectItem(entry, "execStart");
        if (cJSON_IsString(es) && es->valuestring[0]) {
            s->exec_start = strdup(es->valuestring);
            if (!s->exec_start) { fprintf(stderr, "services: out of memory\n"); return -1; }
        }

        cJSON *env = cJSON_GetObjectItem(entry, "environment");
        if (parse_environment(env, &s->env_keys, &s->env_vals, &s->n_env) != 0) {
            fprintf(stderr, "services: out of memory\n");
            return -1;
        }

        out->n++;
    }

    return 0;
}

/* ── Validation ──────────────────────────────────────────────────────── */

/* Validate service names and `requires` references. Returns 0 on
 * success, -1 on error (message printed). */
static int validate_services(svc_list_t *list)
{
    for (int i = 0; i < list->n; i++) {
        svc_t *s = &list->svcs[i];
        if (!valid_service_name(s->name)) {
            fprintf(stderr, "services: invalid service name '%s': only "
                    "letters, digits, '-', '_', '.' are allowed\n", s->name);
            return -1;
        }
        for (int j = 0; j < s->n_requires; j++) {
            if (find_svc(list, s->requires[j]) < 0) {
                fprintf(stderr, "services: service '%s' requires '%s' which "
                        "is not declared\n", s->name, s->requires[j]);
                return -1;
            }
        }
    }
    return 0;
}

/* ── Topological sort (Kahn's algorithm) + cycle detection ──────────── */

/* Recursive DFS to find a cycle in the requires graph.
 * color: 0=white (unvisited), 1=gray (on current DFS stack), 2=black
 * (fully processed). On success, fills *result with a malloc'd string
 * like "a -> b -> a" (without the "service cycle detected:" prefix)
 * and returns 1. Returns 0 if no cycle is reachable from `node`. */
static int dfs_cycle(svc_list_t *list, int *color, int *stack, int depth,
                     int node, char **result)
{
    color[node] = 1;
    stack[depth] = node;
    svc_t *svc = &list->svcs[node];

    for (int j = 0; j < svc->n_requires; j++) {
        int dep = find_svc(list, svc->requires[j]);
        if (dep < 0) continue;

        if (color[dep] == 1) {
            /* Back edge: dep is on the current DFS stack. The cycle
             * runs from stack[start] through stack[depth] and back. */
            int start = 0;
            while (start <= depth && stack[start] != dep) start++;
            char *buf = NULL;
            size_t buf_len = 0;
            FILE *mem = open_memstream(&buf, &buf_len);
            if (!mem) return 1;
            for (int k = start; k <= depth; k++) {
                if (k > start) fputs(" -> ", mem);
                fputs(list->svcs[stack[k]].name, mem);
            }
            fprintf(mem, " -> %s", list->svcs[stack[start]].name);
            fclose(mem);
            *result = buf;
            return 1;
        }

        if (color[dep] == 0) {
            if (dfs_cycle(list, color, stack, depth + 1, dep, result))
                return 1;
        }
    }

    color[node] = 2;
    return 0;
}

/* Find one cycle in the requires graph and return its path as a
 * malloc'd string ("a -> b -> a"), or NULL if the graph is acyclic. */
static char *find_cycle(svc_list_t *list)
{
    int n = list->n;
    char *result = NULL;
    int *color = calloc((size_t)n, sizeof(int));
    int *stack = malloc((size_t)n * sizeof(int));
    if (!color || !stack) { free(color); free(stack); return NULL; }

    for (int i = 0; i < n; i++) {
        if (color[i] == 0) {
            if (dfs_cycle(list, color, stack, 0, i, &result)) break;
        }
    }

    free(color);
    free(stack);
    return result;
}

/* Topologically sort services by their `requires` edges (Kahn's
 * algorithm). On success, returns 0 and sets *order_out to a malloc'd
 * int array of service indices in topological order (dependencies
 * first). On cycle, returns -1 and sets *cycle_str to a malloc'd
 * cycle path string. */
static int topo_sort(svc_list_t *list, int **order_out, char **cycle_str)
{
    int n = list->n;
    *order_out = NULL;
    *cycle_str = NULL;
    if (n == 0) return 0;

    int *in_degree = calloc((size_t)n, sizeof(int));
    int *queue = malloc((size_t)n * sizeof(int));
    int *order = malloc((size_t)n * sizeof(int));
    int qhead = 0, qtail = 0;
    int n_ordered = 0;

    if (!in_degree || !queue || !order) {
        free(in_degree); free(queue); free(order);
        return -1;
    }

    /* in_degree[i] = number of services svc[i] depends on */
    for (int i = 0; i < n; i++) in_degree[i] = list->svcs[i].n_requires;

    /* Start with services that have no requires. */
    for (int i = 0; i < n; i++) {
        if (in_degree[i] == 0) queue[qtail++] = i;
    }

    while (qhead < qtail) {
        int u = queue[qhead++];
        order[n_ordered++] = u;
        /* For each service that depends on u, decrement in_degree. */
        for (int i = 0; i < n; i++) {
            if (i == u) continue;
            int found = 0;
            for (int j = 0; j < list->svcs[i].n_requires; j++) {
                if (strcmp(list->svcs[i].requires[j],
                           list->svcs[u].name) == 0) {
                    found = 1; break;
                }
            }
            if (found) {
                if (--in_degree[i] == 0) queue[qtail++] = i;
            }
        }
    }

    free(in_degree);
    free(queue);

    if (n_ordered < n) {
        free(order);
        *cycle_str = find_cycle(list);
        if (!*cycle_str) {
            *cycle_str = strdup("<unknown>");
        }
        return -1;
    }

    *order_out = order;
    return 0;
}

/* ── Unit file generation ────────────────────────────────────────────── */

/* True if v contains any character that needs quoting in a systemd
 * unit file. Letters, digits, and -_.:/+=@ are safe; everything else
 * triggers quoting. */
static int needs_quote(const char *v)
{
    if (!v) return 0;
    for (const char *p = v; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) continue;
        if (c == '-' || c == '_' || c == '.' || c == '/' || c == ':' ||
            c == '+' || c == '=' || c == '@') continue;
        return 1;
    }
    return 0;
}

/* Quote an environment value for systemd if needed. Wraps in double
 * quotes and escapes `\` and `"` inside. Returns a malloc'd string
 * (caller frees). */
static char *quote_env_value(const char *v)
{
    if (!v) return strdup("");
    if (!needs_quote(v)) return strdup(v);

    size_t extra = 0;
    for (const char *p = v; *p; p++) {
        if (*p == '\\' || *p == '"') extra++;
    }
    size_t len = strlen(v) + extra + 3; /* 2 quotes + NUL */
    char *out = malloc(len);
    if (!out) return NULL;

    size_t k = 0;
    out[k++] = '"';
    for (const char *p = v; *p; p++) {
        if (*p == '\\' || *p == '"') out[k++] = '\\';
        out[k++] = *p;
    }
    out[k++] = '"';
    out[k] = '\0';
    return out;
}

/* Generate the systemd unit file content for a service. Returns a
 * malloc'd string (caller frees). Empty After=/Before=/Requires=
 * lines are omitted. Environment is one line per entry. */
static char *generate_unit(svc_t *svc)
{
    char *buf = NULL;
    size_t buf_len = 0;
    FILE *mem = open_memstream(&buf, &buf_len);
    if (!mem) return NULL;

    const char *type = svc->type ? svc->type : "simple";

    fputs("# Generated by 2O9. Do not edit; run `209 apply` to regenerate.\n",
          mem);
    fputs("[Unit]\n", mem);
    fprintf(mem, "Description=2O9-managed service: %s\n", svc->name);

    if (svc->n_after > 0) {
        fputs("After=", mem);
        for (int i = 0; i < svc->n_after; i++) {
            fputs(svc->after[i], mem);
            if (i < svc->n_after - 1) fputc(' ', mem);
        }
        fputc('\n', mem);
    }
    if (svc->n_before > 0) {
        fputs("Before=", mem);
        for (int i = 0; i < svc->n_before; i++) {
            fputs(svc->before[i], mem);
            if (i < svc->n_before - 1) fputc(' ', mem);
        }
        fputc('\n', mem);
    }
    if (svc->n_requires > 0) {
        fputs("Requires=", mem);
        for (int i = 0; i < svc->n_requires; i++) {
            fputs(svc->requires[i], mem);
            if (i < svc->n_requires - 1) fputc(' ', mem);
        }
        fputc('\n', mem);
    }

    fputs("\n[Service]\n", mem);
    fprintf(mem, "Type=%s\n", type);
    fprintf(mem, "ExecStart=%s\n", svc->exec_start);

    for (int i = 0; i < svc->n_env; i++) {
        char *quoted = quote_env_value(svc->env_vals[i]);
        if (quoted) {
            fprintf(mem, "Environment=%s=%s\n", svc->env_keys[i], quoted);
            free(quoted);
        }
    }

    fputs("\n[Install]\n", mem);
    fputs("WantedBy=multi-user.target\n", mem);

    fclose(mem);
    return buf;
}

/* ── Service state ───────────────────────────────────────────────────── */

/* Check if a service is currently active (running). Returns 1 if
 * active, 0 otherwise (including when systemctl is unavailable). */
static int service_is_active(const char *name)
{
    const char *argv[] = {"systemctl", "is-active", "--quiet", name, NULL};
    int rc = run_cmd(argv);
    return rc == 0 ? 1 : 0;
}

/* ── Apply actions ───────────────────────────────────────────────────── */

/* Apply one service: write the unit file (if execStart), then
 * enable/disable, then start/restart. Sets *changed to 1 if any unit
 * file was written or removed. Returns 0 on success, -1 on error. */
static int apply_one(svc_t *svc, int dry_run, int *changed)
{
    char unit_path[PATH_MAX];
    snprintf(unit_path, sizeof(unit_path), "%s/%s.service", UNIT_DIR, svc->name);

    int unit_changed = 0;

    /* Generate and write unit file if execStart is set. */
    if (svc->exec_start) {
        char *content = generate_unit(svc);
        if (!content) {
            fprintf(stderr, "services: failed to generate unit for %s\n",
                    svc->name);
            return -1;
        }

        char *existing = read_file(unit_path);
        int differs = !existing || strcmp(existing, content) != 0;
        free(existing);

        if (differs) {
            if (dry_run) {
                printf("services: [dry-run] would write %s (%zu bytes)\n",
                       unit_path, strlen(content));
            } else {
                if (write_file_atomic(unit_path, content, strlen(content)) != 0) {
                    free(content);
                    return -1;
                }
                printf("services: wrote %s\n", unit_path);
            }
            unit_changed = 1;
            *changed = 1;
        }
        free(content);
    }

    if (svc->enable) {
        if (dry_run) {
            printf("services: [dry-run] would run: systemctl enable %s\n",
                   svc->name);
        } else {
            printf("services: systemctl enable %s\n", svc->name);
            const char *en_argv[] = {"systemctl", "enable", svc->name, NULL};
            int rc = run_cmd(en_argv);
            if (rc != 0 && rc != 127) {
                fprintf(stderr, "services: systemctl enable %s exited %d\n",
                        svc->name, rc);
            }
        }

        int active = service_is_active(svc->name);
        if (svc->restart_on_change && unit_changed && active) {
            if (dry_run) {
                printf("services: [dry-run] would run: systemctl restart %s "
                       "(unit changed)\n", svc->name);
            } else {
                printf("services: systemctl restart %s (unit changed)\n",
                       svc->name);
                const char *rs_argv[] = {"systemctl", "restart", svc->name, NULL};
                int rc = run_cmd(rs_argv);
                if (rc != 0 && rc != 127) {
                    fprintf(stderr, "services: systemctl restart %s exited %d\n",
                            svc->name, rc);
                }
            }
        } else if (!active) {
            if (dry_run) {
                printf("services: [dry-run] would run: systemctl start %s\n",
                       svc->name);
            } else {
                printf("services: systemctl start %s\n", svc->name);
                const char *st_argv[] = {"systemctl", "start", svc->name, NULL};
                int rc = run_cmd(st_argv);
                if (rc != 0 && rc != 127) {
                    fprintf(stderr, "services: systemctl start %s exited %d\n",
                            svc->name, rc);
                }
            }
        }
    } else {
        /* enable is false: disable. systemctl disable on an unenabled
         * unit returns non-zero; we ignore the exit code. */
        if (dry_run) {
            printf("services: [dry-run] would run: systemctl disable %s\n",
                   svc->name);
        } else {
            printf("services: systemctl disable %s\n", svc->name);
            const char *dis_argv[] = {"systemctl", "disable", svc->name, NULL};
            run_cmd(dis_argv);
        }
    }

    return 0;
}

/* Tear down a service that was in the previous manifest but not the
 * current one: disable, stop if running, remove the generated unit
 * file if it exists. Sets *changed to 1 if a unit file was removed. */
static int remove_one(const char *name, int dry_run, int *changed)
{
    char unit_path[PATH_MAX];
    snprintf(unit_path, sizeof(unit_path), "%s/%s.service", UNIT_DIR, name);

    if (dry_run) {
        printf("services: [dry-run] would remove service %s (disable, stop, "
               "remove unit)\n", name);
        return 0;
    }

    printf("services: removing service %s (no longer declared)\n", name);

    const char *dis_argv[] = {"systemctl", "disable", name, NULL};
    run_cmd(dis_argv);

    if (service_is_active(name)) {
        printf("services: systemctl stop %s\n", name);
        const char *stop_argv[] = {"systemctl", "stop", name, NULL};
        run_cmd(stop_argv);
    }

    struct stat st;
    if (stat(unit_path, &st) == 0) {
        if (unlink(unit_path) == 0) {
            printf("services: removed %s\n", unit_path);
            *changed = 1;
        } else {
            fprintf(stderr, "services: failed to remove %s: %s\n",
                    unit_path, strerror(errno));
        }
    }

    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int services_apply(const char *manifest_json, const char *prev_manifest_json,
                   int dry_run)
{
    if (getuid() != 0) {
        fprintf(stderr, "services: must run as root (systemctl enable/start/"
                "stop, writes to /etc/systemd/system)\n");
        return 1;
    }
    if (!manifest_json) {
        fprintf(stderr, "services: manifest_json is NULL\n");
        return 1;
    }

    cJSON *root = cJSON_Parse(manifest_json);
    if (!root) {
        fprintf(stderr, "services: failed to parse manifest JSON\n");
        return 1;
    }

    cJSON *prev_root = NULL;
    if (prev_manifest_json) {
        prev_root = cJSON_Parse(prev_manifest_json);
        /* NULL is OK; we just skip the prev comparison. */
    }

    svc_list_t list = {0};
    if (parse_services(root, &list) != 0) {
        cJSON_Delete(root);
        cJSON_Delete(prev_root);
        svc_list_free(&list);
        return 1;
    }

    if (validate_services(&list) != 0) {
        cJSON_Delete(root);
        cJSON_Delete(prev_root);
        svc_list_free(&list);
        return 1;
    }

    int *order = NULL;
    char *cycle_str = NULL;
    if (topo_sort(&list, &order, &cycle_str) != 0) {
        if (cycle_str) {
            fprintf(stderr, "services: service cycle detected: %s\n",
                    cycle_str);
            free(cycle_str);
        } else {
            fprintf(stderr, "services: cycle detected in service dependencies\n");
        }
        free(order);
        cJSON_Delete(root);
        cJSON_Delete(prev_root);
        svc_list_free(&list);
        return 1;
    }

    int changed = 0;

    /* Apply each service in topological order. */
    for (int i = 0; i < list.n; i++) {
        int idx = order[i];
        if (apply_one(&list.svcs[idx], dry_run, &changed) != 0) {
            fprintf(stderr, "services: error applying %s\n",
                    list.svcs[idx].name);
        }
    }

    /* Tear down services that were in the previous manifest but not
     * the current one. */
    if (prev_root) {
        cJSON *prev_svc = cJSON_GetObjectItem(prev_root, "services");
        if (prev_svc && cJSON_IsObject(prev_svc)) {
            cJSON *entry;
            cJSON_ArrayForEach(entry, prev_svc) {
                if (!entry->string || !entry->string[0]) continue;
                if (find_svc(&list, entry->string) >= 0) continue;
                remove_one(entry->string, dry_run, &changed);
            }
        }
    }

    /* daemon-reload once if anything on disk changed. */
    if (changed) {
        if (dry_run) {
            printf("services: [dry-run] would run: systemctl daemon-reload\n");
        } else {
            printf("services: systemctl daemon-reload\n");
            const char *dr_argv[] = {"systemctl", "daemon-reload", NULL};
            int rc = run_cmd(dr_argv);
            if (rc != 0 && rc != 127) {
                fprintf(stderr, "services: systemctl daemon-reload exited %d\n",
                        rc);
            }
        }
    } else if (list.n == 0 && !prev_root) {
        printf("services: no services declared\n");
    } else if (list.n > 0) {
        printf("services: no changes\n");
    }

    free(order);
    cJSON_Delete(root);
    cJSON_Delete(prev_root);
    svc_list_free(&list);
    return 0;
}
