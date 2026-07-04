/* subs_ui.c - interactive subscription picker
 *
 * See subs_ui.h for the design. Raw termios, no ncurses. We open
 * /dev/tty for both key input and rendering so the picker works even
 * when stdin/stdout are pipes (e.g. `209 subs | cat` should still
 * drop into the picker on /dev/tty; if /dev/tty isn't available, we
 * fall back to non-interactive mode on stdout).
 *
 * Key bindings:
 *   Up/k, Down/j  - move selection
 *   Enter         - activate current row (open sub / open item)
 *   b             - back (sub view -> sub list)
 *   q / Esc       - quit current view (sub view -> sub list, sub list -> exit)
 *
 * The TUI is intentionally minimal: no scrolling for short lists,
 * clear-and-redraw on every keypress. For very long sub lists or
 * index.json item lists, the render caps at the terminal height.
 */
#include "subs_ui.h"
#include "config.h"
#include "binary-cache.h"
#include "narinfo.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>

/* ── termios helpers ──────────────────────────────────────────────── */

static int g_tty_fd = -1;
static struct termios g_orig_termios;
static int g_orig_set = 0;

static int tty_open(void)
{
        if (g_tty_fd >= 0) return 0;
        g_tty_fd = open("/dev/tty", O_RDWR);
        if (g_tty_fd < 0) return -1;
        if (tcgetattr(g_tty_fd, &g_orig_termios) != 0) {
                close(g_tty_fd); g_tty_fd = -1;
                return -1;
        }
        g_orig_set = 1;
        struct termios raw = g_orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG);
        raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_oflag &= ~OPOST;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(g_tty_fd, TCSANOW, &raw) != 0) {
                close(g_tty_fd); g_tty_fd = -1;
                return -1;
        }
        return 0;
}

static void tty_close(void)
{
        if (g_tty_fd < 0) return;
        if (g_orig_set) tcsetattr(g_tty_fd, TCSANOW, &g_orig_termios);
        close(g_tty_fd);
        g_tty_fd = -1;
        g_orig_set = 0;
}

static void tty_write(const char *s)
{
        if (g_tty_fd >= 0 && s) {
                size_t n = strlen(s);
                ssize_t w = write(g_tty_fd, s, n);
                (void)w;
        }
}

/* Read a single logical key. Returns one of:
 *   'U' (up), 'D' (down), '\r' (enter), 'b' (back),
 *   'q' (quit), or the raw char for anything else.
 *   -1 on EOF/error. */
static int read_key(void)
{
        char c;
        ssize_t n = read(g_tty_fd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\x1b') {
                char seq[2] = {0, 0};
                n = read(g_tty_fd, &seq[0], 1);
                if (n <= 0) return 'q';  /* bare Escape */
                if (seq[0] == '[') {
                        n = read(g_tty_fd, &seq[1], 1);
                        if (n <= 0) return -1;
                        if (seq[1] == 'A') return 'U';
                        if (seq[1] == 'B') return 'D';
                }
                return -1;  /* unknown escape */
        }
        if (c == 3 || c == 4) return 'q';  /* Ctrl-C, Ctrl-D */
        if (c == 'q' || c == 'Q') return 'q';
        if (c == 'b' || c == 'B') return 'b';
        if (c == '\r' || c == '\n') return '\r';
        if (c == 'k') return 'U';
        if (c == 'j') return 'D';
        return c;
}

/* ── Helpers for working with a sub's index ──────────────────────── */

/* Count entries in a NULL-terminated string list. */
static size_t str_list_len(char **list)
{
        size_t n = 0;
        if (list) while (list[n]) n++;
        return n;
}

/* Deep-copy a NULL-terminated string list. Returns NULL if input is
 * NULL or empty. Caller frees each entry + the list. */
static char **str_list_dup(const char *const *list)
{
        if (!list) return NULL;
        size_t n = 0;
        while (list[n]) n++;
        if (n == 0) return NULL;
        char **copy = calloc(n + 1, sizeof(char *));
        if (!copy) return NULL;
        for (size_t i = 0; i < n; i++) {
                copy[i] = strdup(list[i]);
                if (!copy[i]) {
                        for (size_t j = 0; j < i; j++) free(copy[j]);
                        free(copy);
                        return NULL;
                }
        }
        return copy;
}

/* Build a binary_cache_t for one URL of a sub, with the sub's public
 * keys copied (the cache takes ownership). Returns NULL on alloc
 * failure. */
static binary_cache_t *bc_for_sub_url(const two9_sub_t *s, const char *url)
{
        char **keys_copy = str_list_dup((const char *const *)s->public_keys);
        return binary_cache_new(url, keys_copy, s->allow_unsigned);
}

/* Fetch index.json from one URL of a sub. Returns a cJSON object or
 * NULL on failure. Caller cJSON_Delete's the result. */
static cJSON *fetch_index_for_url(const two9_sub_t *s, const char *url)
{
        binary_cache_t *bc = bc_for_sub_url(s, url);
        if (!bc) return NULL;
        cJSON *idx = binary_cache_fetch_index(bc);
        binary_cache_free(bc);
        return idx;
}

/* Collect the union of items from each URL's index.json into a single
 * cJSON array (caller cJSON_Delete's it). Items are deduped by hash.
 * Returns NULL if no URL has an index. */
static cJSON *collect_sub_items(const two9_sub_t *s)
{
        if (!s->urls) return NULL;
        cJSON *all = cJSON_CreateArray();
        if (!all) return NULL;
        for (size_t u = 0; s->urls[u]; u++) {
                cJSON *idx = fetch_index_for_url(s, s->urls[u]);
                if (!idx) continue;
                cJSON *items = cJSON_GetObjectItemCaseSensitive(idx, "items");
                if (items && cJSON_IsArray(items)) {
                        cJSON *item;
                        cJSON_ArrayForEach(item, items) {
                                cJSON *h = cJSON_GetObjectItemCaseSensitive(item, "hash");
                                if (!h || !cJSON_IsString(h)) continue;
                                /* Dedup by hash. */
                                int dup = 0;
                                cJSON *existing;
                                cJSON_ArrayForEach(existing, all) {
                                        cJSON *eh = cJSON_GetObjectItemCaseSensitive(existing, "hash");
                                        if (eh && cJSON_IsString(eh) &&
                                            strcmp(eh->valuestring, h->valuestring) == 0) {
                                                dup = 1;
                                                break;
                                        }
                                }
                                if (!dup) {
                                        cJSON *copy = cJSON_Duplicate(item, 1);
                                        if (copy) cJSON_AddItemToArray(all, copy);
                                }
                        }
                }
                cJSON_Delete(idx);
        }
        return all;
}

/* Format a byte count as a human-readable string ("12.4 MB", etc).
 * Writes to buf (caller-allocated, at least 24 bytes). */
static void format_size(int64_t bytes, char *buf, size_t buf_sz)
{
        if (bytes < 1024) {
                snprintf(buf, buf_sz, "%lld B", (long long)bytes);
        } else if (bytes < 1024 * 1024) {
                snprintf(buf, buf_sz, "%.1f KB", bytes / 1024.0);
        } else if (bytes < 1024LL * 1024 * 1024) {
                snprintf(buf, buf_sz, "%.1f MB", bytes / (1024.0 * 1024));
        } else {
                snprintf(buf, buf_sz, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
        }
}

/* Format a Unix timestamp as "YYYY-MM-DD HH:MM". Writes to buf. */
static void format_time(int64_t ts, char *buf, size_t buf_sz)
{
        time_t t = (time_t)ts;
        struct tm tm;
        if (localtime_r(&t, &tm) == NULL) {
                snprintf(buf, buf_sz, "%lld", (long long)ts);
                return;
        }
        strftime(buf, buf_sz, "%Y-%m-%d %H:%M", &tm);
}

/* ── Render: sub list ─────────────────────────────────────────────── */

/* Render the sub list. `sel` is the 0-based selection index. */
static void render_sub_list(const two9_config_t *cfg, int sel)
{
        tty_write("\033[2J\033[H");  /* clear + home */
        char line[512];
        snprintf(line, sizeof(line), "%s2O9 Subs%s\r\n\r\n",
                 "\033[1m", "\033[0m");
        tty_write(line);

        int i = 0;
        for (two9_sub_t *s = cfg->subs; s; s = s->next, i++) {
                /* Build a comma-separated URL summary. */
                char urls[256] = "";
                size_t url_count = str_list_len(s->urls);
                for (size_t u = 0; u < url_count && u < 2; u++) {
                        if (u > 0) strncat(urls, ", ", sizeof(urls) - strlen(urls) - 1);
                        strncat(urls, s->urls[u], sizeof(urls) - strlen(urls) - 1);
                }
                if (url_count > 2) {
                        char more[32];
                        snprintf(more, sizeof(more), " (+%zu more)", url_count - 2);
                        strncat(urls, more, sizeof(urls) - strlen(urls) - 1);
                }
                size_t key_count = str_list_len(s->public_keys);
                const char *marker = (i == sel) ? ">" : " ";
                const char *bold = (i == sel) ? "\033[1m" : "";
                const char *reset = (i == sel) ? "\033[0m" : "";
                int is_legacy = s->name && strcmp(s->name, "legacy") == 0;
                snprintf(line, sizeof(line),
                         "%s%s%d. %-14s %-44s (%zu URL%s, %zu key%s%s)%s\r\n",
                         marker, bold, i + 1,
                         s->name ? s->name : "(unnamed)",
                         urls,
                         url_count, url_count == 1 ? "" : "s",
                         key_count, key_count == 1 ? "" : "s",
                         is_legacy ? ", deprecated" : "",
                         reset);
                tty_write(line);
        }
        tty_write("\r\n");
        tty_write("Up/Down move, Enter view, q quit\r\n");
}

/* ── Render: sub view ─────────────────────────────────────────────── */

static void render_sub_view(const two9_sub_t *s, cJSON *items, int sel)
{
        tty_write("\033[2J\033[H");
        char line[1024];

        snprintf(line, sizeof(line), "%sSub: %s%s\r\n",
                 "\033[1m", s->name ? s->name : "(unnamed)", "\033[0m");
        tty_write(line);

        /* URLs line. */
        char urls[512] = "";
        size_t url_count = str_list_len(s->urls);
        for (size_t u = 0; u < url_count; u++) {
                if (u > 0) strncat(urls, ", ", sizeof(urls) - strlen(urls) - 1);
                strncat(urls, s->urls[u], sizeof(urls) - strlen(urls) - 1);
        }
        snprintf(line, sizeof(line), "URLs: %s\r\n", urls);
        tty_write(line);

        size_t key_count = str_list_len(s->public_keys);
        snprintf(line, sizeof(line), "Public keys: %zu\r\n", key_count);
        tty_write(line);
        snprintf(line, sizeof(line), "AllowUnsigned: %s\r\n",
                 s->allow_unsigned ? "true" : "false");
        tty_write(line);
        tty_write("\r\n");

        /* Items. */
        const char *first_url = (url_count > 0) ? s->urls[0] : "(no URL)";
        snprintf(line, sizeof(line),
                 "Contents (fetched from %s/index.json):\r\n", first_url);
        tty_write(line);

        if (!items || cJSON_GetArraySize(items) == 0) {
                tty_write("  (no items or index.json not found)\r\n");
        } else {
                int i = 0;
                cJSON *item;
                cJSON_ArrayForEach(item, items) {
                        cJSON *jh = cJSON_GetObjectItemCaseSensitive(item, "hash");
                        cJSON *jn = cJSON_GetObjectItemCaseSensitive(item, "name");
                        cJSON *jt = cJSON_GetObjectItemCaseSensitive(item, "type");
                        cJSON *js = cJSON_GetObjectItemCaseSensitive(item, "nar_size");
                        cJSON *jp = cJSON_GetObjectItemCaseSensitive(item, "pushed_at");

                        const char *h = (jh && cJSON_IsString(jh)) ? jh->valuestring : "";
                        const char *nm = (jn && cJSON_IsString(jn)) ? jn->valuestring : "";
                        const char *tp = (jt && cJSON_IsString(jt)) ? jt->valuestring : "?";
                        int64_t sz = (js && cJSON_IsNumber(js)) ? (int64_t)js->valuedouble : 0;
                        int64_t ts = (jp && cJSON_IsNumber(jp)) ? (int64_t)jp->valuedouble : 0;

                        char sz_buf[24];
                        format_size(sz, sz_buf, sizeof(sz_buf));
                        char t_buf[32];
                        format_time(ts, t_buf, sizeof(t_buf));

                        const char *marker = (i == sel) ? ">" : " ";
                        const char *bold = (i == sel) ? "\033[1m" : "";
                        const char *reset = (i == sel) ? "\033[0m" : "";
                        snprintf(line, sizeof(line),
                                 "%s%s%2d.%s %-12s %-5s %-18s %8s  %s%s\r\n",
                                 marker, bold, i + 1, "",
                                 h, tp, nm, sz_buf, t_buf, reset);
                        tty_write(line);
                        i++;
                }
        }
        tty_write("\r\n");
        tty_write("Up/Down move, Enter view item, b back, q quit\r\n");
}

/* ── Render: item view (just prints the narinfo) ──────────────────── */

static void render_item_view(const two9_sub_t *s, const cJSON *item)
{
        tty_write("\033[2J\033[H");
        char line[1024];
        cJSON *jh = cJSON_GetObjectItemCaseSensitive(item, "hash");
        const char *h = (jh && cJSON_IsString(jh)) ? jh->valuestring : "";

        snprintf(line, sizeof(line), "%sItem: %s%s\r\n\r\n",
                 "\033[1m", h, "\033[0m");
        tty_write(line);

        /* Fetch the narinfo from the first URL that has it. */
        if (!s->urls) {
                tty_write("(no URLs configured)\r\n");
        } else {
                for (size_t u = 0; s->urls[u]; u++) {
                        binary_cache_t *bc = bc_for_sub_url(s, s->urls[u]);
                        if (!bc) continue;
                        narinfo_t *ni = binary_cache_lookup_by_hash(bc, h);
                        binary_cache_free(bc);
                        if (!ni) continue;

                        char *text = narinfo_serialize(ni);
                        narinfo_free(ni);
                        if (text) {
                                /* Replace \n with \r\n for terminal. */
                                for (char *p = text; *p; p++) {
                                        if (*p == '\n') tty_write("\r\n");
                                        else { char c[2] = { *p, 0 }; tty_write(c); }
                                }
                                if (text[strlen(text) - 1] != '\n')
                                        tty_write("\r\n");
                                free(text);
                        }
                        break;
                }
        }
        tty_write("\r\n");
        tty_write("Any key to go back\r\n");
}

/* ── Main interactive loop ────────────────────────────────────────── */

static int interactive_loop(const two9_config_t *cfg)
{
        if (tty_open() != 0) {
                /* No /dev/tty available. Fall back to non-interactive. */
                return subs_ui_print_sub(NULL);
        }

        /* Sub list view. */
        int sel = 0;
        int n_subs = 0;
        for (two9_sub_t *s = cfg->subs; s; s = s->next) n_subs++;
        if (n_subs == 0) {
                tty_write("\033[2J\033[H");
                tty_write("No subs configured. Add one with: 209 subs add <name>\r\n");
                tty_write("Press q to quit.\r\n");
                while (read_key() != 'q') ;
                tty_close();
                return 0;
        }

        int view = 0;  /* 0 = sub list, 1 = sub view, 2 = item view */
        const two9_sub_t *cur_sub = NULL;
        cJSON *cur_items = NULL;
        int item_sel = 0;

        for (;;) {
                if (view == 0) {
                        if (sel >= n_subs) sel = n_subs - 1;
                        if (sel < 0) sel = 0;
                        render_sub_list(cfg, sel);
                        int k = read_key();
                        if (k == 'q') break;
                        if (k == 'U') sel--;
                        if (k == 'D') sel++;
                        if (k == '\r') {
                                /* Open sub. */
                                int i = 0;
                                cur_sub = NULL;
                                for (two9_sub_t *s = cfg->subs; s; s = s->next, i++) {
                                        if (i == sel) { cur_sub = s; break; }
                                }
                                if (cur_sub) {
                                        if (cur_items) cJSON_Delete(cur_items);
                                        cur_items = collect_sub_items(cur_sub);
                                        item_sel = 0;
                                        view = 1;
                                }
                        }
                } else if (view == 1) {
                        int n_items = cur_items ? cJSON_GetArraySize(cur_items) : 0;
                        if (item_sel >= n_items) item_sel = n_items - 1;
                        if (item_sel < 0) item_sel = 0;
                        render_sub_view(cur_sub, cur_items, item_sel);
                        int k = read_key();
                        if (k == 'q') break;
                        if (k == 'b') {
                                view = 0;
                                if (cur_items) { cJSON_Delete(cur_items); cur_items = NULL; }
                                cur_sub = NULL;
                        }
                        if (k == 'U') item_sel--;
                        if (k == 'D') item_sel++;
                        if (k == '\r' && n_items > 0) {
                                cJSON *item = cJSON_GetArrayItem(cur_items, item_sel);
                                if (item) {
                                        view = 2;
                                }
                        }
                } else if (view == 2) {
                        cJSON *item = cJSON_GetArrayItem(cur_items, item_sel);
                        render_item_view(cur_sub, item);
                        (void)read_key();  /* any key */
                        view = 1;
                }
        }

        if (cur_items) cJSON_Delete(cur_items);
        tty_close();
        return 0;
}

/* ── Non-interactive: print sub list ──────────────────────────────── */

int subs_ui_run(void)
{
        two9_config_t *cfg = two9_config_load();
        if (!cfg) {
                fprintf(stderr, "209: cannot load config\n");
                return 1;
        }
        if (!cfg->subs) {
                printf("No subs configured.\n");
                printf("Add one with: 209 subs add <name>\n");
                two9_config_free(cfg);
                return 0;
        }

        int rc;
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
                /* Non-interactive: print the sub list. */
                printf("2O9 Subs\n\n");
                int i = 1;
                for (two9_sub_t *s = cfg->subs; s; s = s->next, i++) {
                        char urls[512] = "";
                        size_t url_count = str_list_len(s->urls);
                        for (size_t u = 0; u < url_count; u++) {
                                if (u > 0) strncat(urls, ", ", sizeof(urls) - strlen(urls) - 1);
                                strncat(urls, s->urls[u], sizeof(urls) - strlen(urls) - 1);
                        }
                        size_t key_count = str_list_len(s->public_keys);
                        int is_legacy = s->name && strcmp(s->name, "legacy") == 0;
                        printf("  %d. %-14s %-44s (%zu URL%s, %zu key%s%s)\n",
                               i, s->name ? s->name : "(unnamed)", urls,
                               url_count, url_count == 1 ? "" : "s",
                               key_count, key_count == 1 ? "" : "s",
                               is_legacy ? ", deprecated" : "");
                }
                rc = 0;
        } else {
                rc = interactive_loop(cfg);
        }
        two9_config_free(cfg);
        return rc;
}

/* ── Non-interactive: print one sub ───────────────────────────────── */

int subs_ui_print_sub(const char *name)
{
        two9_config_t *cfg = two9_config_load();
        if (!cfg) {
                fprintf(stderr, "209: cannot load config\n");
                return 1;
        }
        if (!cfg->subs) {
                fprintf(stderr, "209: no subs configured\n");
                two9_config_free(cfg);
                return 1;
        }

        two9_sub_t *found = NULL;
        for (two9_sub_t *s = cfg->subs; s; s = s->next) {
                if (s->name && strcmp(s->name, name) == 0) {
                        found = s;
                        break;
                }
        }
        if (!found) {
                fprintf(stderr, "209: no sub named '%s'\n", name);
                two9_config_free(cfg);
                return 1;
        }

        printf("Sub: %s\n", found->name ? found->name : "(unnamed)");
        char urls[512] = "";
        size_t url_count = str_list_len(found->urls);
        for (size_t u = 0; u < url_count; u++) {
                if (u > 0) strncat(urls, ", ", sizeof(urls) - strlen(urls) - 1);
                strncat(urls, found->urls[u], sizeof(urls) - strlen(urls) - 1);
        }
        printf("URLs: %s\n", urls);
        printf("Public keys: %zu\n", str_list_len(found->public_keys));
        printf("AllowUnsigned: %s\n", found->allow_unsigned ? "true" : "false");
        printf("\n");

        cJSON *items = collect_sub_items(found);
        const char *first_url = (url_count > 0) ? found->urls[0] : "(no URL)";
        printf("Contents (fetched from %s/index.json):\n", first_url);
        if (!items || cJSON_GetArraySize(items) == 0) {
                printf("  (no items or index.json not found)\n");
        } else {
                cJSON *item;
                cJSON_ArrayForEach(item, items) {
                        cJSON *jh = cJSON_GetObjectItemCaseSensitive(item, "hash");
                        cJSON *jn = cJSON_GetObjectItemCaseSensitive(item, "name");
                        cJSON *jt = cJSON_GetObjectItemCaseSensitive(item, "type");
                        cJSON *js = cJSON_GetObjectItemCaseSensitive(item, "nar_size");
                        cJSON *jp = cJSON_GetObjectItemCaseSensitive(item, "pushed_at");

                        const char *h = (jh && cJSON_IsString(jh)) ? jh->valuestring : "";
                        const char *nm = (jn && cJSON_IsString(jn)) ? jn->valuestring : "";
                        const char *tp = (jt && cJSON_IsString(jt)) ? jt->valuestring : "?";
                        int64_t sz = (js && cJSON_IsNumber(js)) ? (int64_t)js->valuedouble : 0;
                        int64_t ts = (jp && cJSON_IsNumber(jp)) ? (int64_t)jp->valuedouble : 0;

                        char sz_buf[24];
                        format_size(sz, sz_buf, sizeof(sz_buf));
                        char t_buf[32];
                        format_time(ts, t_buf, sizeof(t_buf));
                        printf("  %-12s %-5s %-18s %8s  %s\n",
                               h, tp, nm, sz_buf, t_buf);
                }
        }
        if (items) cJSON_Delete(items);
        two9_config_free(cfg);
        return 0;
}
