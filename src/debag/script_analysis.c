/* script_analysis.c - parse .install scripts and extract intent
 *
 * pacman's .install scripts are shell scripts with three functions:
 *   post_install(), post_upgrade(), pre_remove()
 *
 * 2O9 doesn't run them blindly. Instead, we parse the script text and
 * extract what each function does - files created, services enabled,
 * commands run - and present it to the user as a summary before they
 * decide whether to run it.
 *
 * This is a heuristic parser, not a full shell interpreter. It looks
 * for known patterns:
 * - systemctl enable/disable/start/stop
 * - systemd-sysusers
 * - systemd-tmpfiles
 * - install -D, mkdir, touch, cp (file creation)
 * - chown, chmod (permission changes)
 * - gtk-update-icon-cache, update-desktop-database, fc-cache (cache rebuilds)
 * - useradd, groupadd (user/group creation)
 * - rm, rmdir (file removal)
 *
 * Unknown commands are listed as "Runs: <command>" so the user can
 * inspect them manually.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Intent types ────────────────────────────────────────────────── */

typedef enum {
    INTENT_FILE_CREATE,
    INTENT_FILE_REMOVE,
    INTENT_SERVICE_ENABLE,
    INTENT_SERVICE_DISABLE,
    INTENT_SERVICE_START,
    INTENT_SERVICE_STOP,
    INTENT_SERVICE_RELOAD,
    INTENT_USER_CREATE,
    INTENT_GROUP_CREATE,
    INTENT_CACHE_REBUILD,
    INTENT_PERMISSION_CHANGE,
    INTENT_RUN_COMMAND,
    INTENT_SYSUSERS,
    INTENT_TMPFILES,
} intent_type_t;

typedef struct script_intent {
    intent_type_t type;
    char *target;       /* file path, service name, or command */
    char *detail;       /* extra info (e.g., chown target) */
    struct script_intent *next;
} script_intent_t;

typedef struct script_analysis {
    char *function_name;    /* "post_install", "post_upgrade", "pre_remove" */
    script_intent_t *intents;
    size_t intent_count;
    struct script_analysis *next;  /* linked list of functions */
} script_analysis_t;

/* ── Helpers ─────────────────────────────────────────────────────── */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static char *extract_arg(const char *line, const char *cmd)
{
    /* Skip past the command and any leading whitespace */
    const char *p = line + strlen(cmd);
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return NULL;

    /* Find the end of the first argument (space or end of line) */
    const char *end = p;
    while (*end && !isspace((unsigned char)*end)) end++;

    size_t len = end - p;
    char *result = malloc(len + 1);
    memcpy(result, p, len);
    result[len] = '\0';
    return result;
}

static char *extract_quoted_arg(const char *line, const char *cmd)
{
    /* Like extract_arg but handles quotes */
    const char *p = line + strlen(cmd);
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return NULL;

    /* Skip flags like -D, --mode=, etc. */
    while (*p == '-') {
        while (*p && !isspace((unsigned char)*p)) p++;
        while (*p && isspace((unsigned char)*p)) p++;
    }

    const char *end = p;
    while (*end && !isspace((unsigned char)*end)) end++;

    size_t len = end - p;
    char *result = malloc(len + 1);
    memcpy(result, p, len);
    result[len] = '\0';
    return result;
}

static script_intent_t *new_intent(intent_type_t type, const char *target, const char *detail)
{
    script_intent_t *i = calloc(1, sizeof(*i));
    i->type = type;
    i->target = target ? strdup(target) : NULL;
    i->detail = detail ? strdup(detail) : NULL;
    return i;
}

static void free_intents(script_intent_t *list)
{
    while (list) {
        script_intent_t *next = list->next;
        free(list->target);
        free(list->detail);
        free(list);
        list = next;
    }
}

/* ── Parse a single line for intent ──────────────────────────────── */

static script_intent_t *parse_line(const char *line)
{
    char buf[4096];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *trimmed = trim(buf);

    /* Skip empty lines, comments, and shell builtins */
    if (!*trimmed || *trimmed == '#' || starts_with(trimmed, "echo") ||
        starts_with(trimmed, "export") || starts_with(trimmed, "local"))
        return NULL;

    /* systemctl enable/disable/start/stop/reload/daemon-reload */
    if (starts_with(trimmed, "systemctl ")) {
        if (strstr(trimmed, "enable")) {
            char *svc = extract_arg(strstr(trimmed, "enable"), "enable");
            if (svc && svc[0] != '-') return new_intent(INTENT_SERVICE_ENABLE, svc, NULL);
            free(svc);
        }
        if (strstr(trimmed, "disable")) {
            char *svc = extract_arg(strstr(trimmed, "disable"), "disable");
            if (svc && svc[0] != '-') return new_intent(INTENT_SERVICE_DISABLE, svc, NULL);
            free(svc);
        }
        if (strstr(trimmed, "start")) {
            char *svc = extract_arg(strstr(trimmed, "start"), "start");
            if (svc && svc[0] != '-') return new_intent(INTENT_SERVICE_START, svc, NULL);
            free(svc);
        }
        if (strstr(trimmed, "stop")) {
            char *svc = extract_arg(strstr(trimmed, "stop"), "stop");
            if (svc && svc[0] != '-') return new_intent(INTENT_SERVICE_STOP, svc, NULL);
            free(svc);
        }
        if (strstr(trimmed, "daemon-reload"))
            return new_intent(INTENT_SERVICE_RELOAD, "daemon-reload", NULL);
        return new_intent(INTENT_RUN_COMMAND, trimmed, "systemctl");
    }

    /* systemd-sysusers */
    if (starts_with(trimmed, "systemd-sysusers") || starts_with(trimmed, "sysusers"))
        return new_intent(INTENT_SYSUSERS, trimmed, NULL);

    /* systemd-tmpfiles */
    if (starts_with(trimmed, "systemd-tmpfiles") || starts_with(trimmed, "tmpfiles"))
        return new_intent(INTENT_TMPFILES, trimmed, NULL);

    /* install -D / mkdir / touch / cp - file creation */
    if (starts_with(trimmed, "install ") || starts_with(trimmed, "install\t")) {
        char *target = extract_quoted_arg(trimmed, "install");
        if (target) return new_intent(INTENT_FILE_CREATE, target, "install");
        free(target);
    }
    if (starts_with(trimmed, "mkdir ")) {
        char *target = extract_arg(trimmed, "mkdir");
        if (target) return new_intent(INTENT_FILE_CREATE, target, "mkdir");
        free(target);
    }
    if (starts_with(trimmed, "touch ")) {
        char *target = extract_arg(trimmed, "touch");
        if (target) return new_intent(INTENT_FILE_CREATE, target, "touch");
        free(target);
    }

    /* rm / rmdir - file removal */
    if (starts_with(trimmed, "rm ") || starts_with(trimmed, "rmdir ")) {
        char *target = extract_arg(trimmed, starts_with(trimmed, "rm ") ? "rm " : "rmdir ");
        if (target) return new_intent(INTENT_FILE_REMOVE, target, NULL);
        free(target);
    }

    /* chown - permission change */
    if (starts_with(trimmed, "chown ")) {
        char *arg = extract_arg(trimmed, "chown");
        if (arg) return new_intent(INTENT_PERMISSION_CHANGE, arg, "chown");
        free(arg);
    }
    if (starts_with(trimmed, "chmod ")) {
        char *arg = extract_arg(trimmed, "chmod");
        if (arg) return new_intent(INTENT_PERMISSION_CHANGE, arg, "chmod");
        free(arg);
    }

    /* useradd / groupadd */
    if (starts_with(trimmed, "useradd ")) {
        char *arg = extract_arg(trimmed, "useradd");
        if (arg) return new_intent(INTENT_USER_CREATE, arg, NULL);
        free(arg);
    }
    if (starts_with(trimmed, "groupadd ")) {
        char *arg = extract_arg(trimmed, "groupadd");
        if (arg) return new_intent(INTENT_GROUP_CREATE, arg, NULL);
        free(arg);
    }

    /* Cache rebuilds */
    if (starts_with(trimmed, "gtk-update-icon-cache") ||
        starts_with(trimmed, "update-desktop-database") ||
        starts_with(trimmed, "fc-cache") ||
        starts_with(trimmed, "ldconfig"))
        return new_intent(INTENT_CACHE_REBUILD, trimmed, NULL);

    /* Anything else is an unknown command */
    return new_intent(INTENT_RUN_COMMAND, trimmed, NULL);
}

/* ── Parse an entire .install script ─────────────────────────────── */

script_analysis_t *debag_analyze_script(const char *script_path)
{
    FILE *f = fopen(script_path, "r");
    if (!f) return NULL;

    script_analysis_t *head = NULL, *tail = NULL;
    char line[4096];
    script_analysis_t *current_func = NULL;
    int in_function = 0;

    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);

        /* Detect function definitions: post_install(), post_upgrade(), pre_remove() */
        if (starts_with(trimmed, "post_install") ||
            starts_with(trimmed, "post_upgrade") ||
            starts_with(trimmed, "pre_remove") ||
            starts_with(trimmed, "post_remove")) {

            /* Extract function name */
            char name[64] = {0};
            const char *paren = strchr(trimmed, '(');
            if (paren) {
                size_t len = paren - trimmed;
                if (len >= sizeof(name)) len = sizeof(name) - 1;
                memcpy(name, trimmed, len);
                name[len] = '\0';
            } else {
                strncpy(name, trimmed, sizeof(name) - 1);
            }

            current_func = calloc(1, sizeof(*current_func));
            current_func->function_name = strdup(name);
            if (tail) tail->next = current_func; else head = current_func;
            tail = current_func;
            in_function = 1;
            continue;
        }

        /* End of function (closing brace) */
        if (in_function && *trimmed == '}') {
            in_function = 0;
            current_func = NULL;
            continue;
        }

        /* Parse lines inside a function */
        if (in_function && current_func) {
            script_intent_t *intent = parse_line(trimmed);
            if (intent) {
                if (current_func->intents) {
                    script_intent_t *t = current_func->intents;
                    while (t->next) t = t->next;
                    t->next = intent;
                } else {
                    current_func->intents = intent;
                }
                current_func->intent_count++;
            }
        }
    }

    fclose(f);
    return head;
}

void debag_free_script_analysis(script_analysis_t *a)
{
    while (a) {
        script_analysis_t *next = a->next;
        free(a->function_name);
        free_intents(a->intents);
        free(a);
        a = next;
    }
}

/* ── Print analysis in human-readable format ─────────────────────── */

static const char *intent_label(intent_type_t t)
{
    switch (t) {
    case INTENT_FILE_CREATE:        return "Creates";
    case INTENT_FILE_REMOVE:        return "Removes";
    case INTENT_SERVICE_ENABLE:     return "Enables service";
    case INTENT_SERVICE_DISABLE:    return "Disables service";
    case INTENT_SERVICE_START:      return "Starts service";
    case INTENT_SERVICE_STOP:       return "Stops service";
    case INTENT_SERVICE_RELOAD:     return "Reloads systemd";
    case INTENT_USER_CREATE:        return "Creates user";
    case INTENT_GROUP_CREATE:       return "Creates group";
    case INTENT_CACHE_REBUILD:      return "Rebuilds cache";
    case INTENT_PERMISSION_CHANGE:  return "Changes permissions";
    case INTENT_RUN_COMMAND:        return "Runs";
    case INTENT_SYSUSERS:           return "Applies sysusers";
    case INTENT_TMPFILES:           return "Applies tmpfiles";
    default:                        return "Unknown";
    }
}

void debag_print_script_analysis(const script_analysis_t *a, FILE *out)
{
    if (!a) {
        fprintf(out, "No .install script found or script is empty.\n");
        return;
    }

    fprintf(out, "\n Debag static analysis of .install script:\n\n");

    for (const script_analysis_t *func = a; func; func = func->next) {
        if (func->intent_count == 0) continue;
        fprintf(out, "  %s():\n", func->function_name);
        for (script_intent_t *i = func->intents; i; i = i->next) {
            fprintf(out, "  - %s: %s", intent_label(i->type),
                    i->target ? i->target : "?");
            if (i->detail)
                fprintf(out, " (%s)", i->detail);
            fprintf(out, "\n");
        }
        fprintf(out, "\n");
    }
}
