/* bootloader.c - 2O9 declarative bootloader management (Phase 4)
 *
 * Applies the `boot.loader` block from a 2O9 manifest. Two backends:
 * grub (BIOS + UEFI) and systemd-boot (UEFI only). Each 2O9 generation
 * gets its own boot menu entry pointing at that generation's kernel,
 * initrd, and the hoshizora init in /nix/store.
 *
 * Generation enumeration walks <db_root>/generations/. Each subdirectory
 * is a generation number; its manifest.json supplies the package list
 * (kernel + hoshizora) and the file mtime supplies the menu date. The
 * current generation (per <db_root>/current) is the default entry.
 *
 * grub flow:
 *   1. grub-install (i386-pc for BIOS, x86_64-efi for UEFI).
 *   2. mkdir -p /boot/grub.
 *   3. Generate /boot/grub/grub.cfg:
 *      - If useOSProber: run GRUB_DISABLE_OS_PROBER=false grub-mkconfig,
 *        then append 2O9 entries.
 *      - Otherwise: write a custom grub.cfg (set default=0, set timeout,
 *        one menuentry per generation, then extraEntries and extraConfig).
 *   4. Check declared kernel (boot.kernel) is in the current generation's
 *      package list. If not, print a note and skip initrd.
 *   5. Run mkinitcpio -P. If boot.initrdModules is set, write a temporary
 *      mkinitcpio.conf with those modules first.
 *
 * systemd-boot flow:
 *   1. bootctl install (if /boot/EFI/systemd/systemd-bootx64.efi is
 *      missing) or bootctl update.
 *   2. Write /boot/loader/loader.conf with console-mode, editor, and
 *      default set to the current generation's entry.
 *   3. Write /boot/loader/entries/2O9-<N>.conf for each generation.
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
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <time.h>

#include "bootloader.h"
#include "cJSON.h"

#define GRUB_DIR              "/boot/grub"
#define GRUB_CFG_PATH         "/boot/grub/grub.cfg"
#define SYSTEMD_BOOT_EFI      "/boot/EFI/systemd/systemd-bootx64.efi"
#define SYSTEMD_LOADER_CONF   "/boot/loader/loader.conf"
#define SYSTEMD_ENTRIES_DIR   "/boot/loader/entries"
#define EFI_DIR               "/boot/EFI"

#define DEFAULT_TIMEOUT       5
#define FALLBACK_ROOT_DEVICE  "/dev/sda2"
#define FALLBACK_HOSHIZORA    "/nix/store/unknown-hoshizora"

/* ── Subprocess helpers ────────────────────────────────────────────── */

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

/* fork + execvp with one extra environment variable set in the child.
 * Used for GRUB_DISABLE_OS_PROBER=false grub-mkconfig. */
static int run_cmd_env(const char *argv[],
                       const char *env_key, const char *env_val)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        if (setenv(env_key, env_val, 1) != 0) _exit(127);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return -1; }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* ── File helpers ──────────────────────────────────────────────────── */

/* Read entire file into a malloc'd buffer. Returns NULL on missing or
 * unreadable file. Caller frees. */
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

/* Write a file atomically via mkstemp + rename. The temp file is
 * created in the same directory as the target so rename works. */
static int write_file_atomic(const char *path, const char *content,
                             size_t len, mode_t mode)
{
    char tmp_path[PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.2O9.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        fprintf(stderr, "bootloader: path too long for temp: %s\n", path);
        return -1;
    }
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        fprintf(stderr, "bootloader: mkstemp for %s failed: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (write(fd, content, len) != (ssize_t)len) {
        fprintf(stderr, "bootloader: write to %s failed: %s\n",
                path, strerror(errno));
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);
    if (chmod(tmp_path, mode) != 0) {
        /* non-fatal: the file is already written; mode may be wrong. */
    }
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "bootloader: rename %s -> %s failed: %s\n",
                tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/* ── Manifest accessors ────────────────────────────────────────────── */

/* Return non-zero if name looks like a kernel package. "linux" matches
 * exactly; "linux-lts", "linux-zen", "linux-hardened" match. Headers,
 * firmware, docs, and api-headers packages are excluded. */
static int is_kernel_package(const char *name)
{
    if (!name) return 0;
    if (strcmp(name, "linux") == 0) return 1;
    if (strncmp(name, "linux-", 6) != 0) return 0;
    if (strstr(name, "headers")) return 0;
    if (strstr(name, "firmware")) return 0;
    if (strstr(name, "docs")) return 0;
    if (strstr(name, "api-")) return 0;
    return 1;
}

/* Find the root device from boot.fileSystems["/"].device. Falls back to
 * top-level fileSystems["/"].device (the fstab module's location), then
 * to FALLBACK_ROOT_DEVICE with a warning. */
static void get_root_device(cJSON *root, char *buf, size_t bufsize)
{
    cJSON *boot = cJSON_GetObjectItem(root, "boot");
    cJSON *fs = boot ? cJSON_GetObjectItem(boot, "fileSystems") : NULL;
    if (!fs) fs = cJSON_GetObjectItem(root, "fileSystems");
    if (fs) {
        cJSON *rootfs = cJSON_GetObjectItem(fs, "/");
        if (rootfs) {
            cJSON *dev = cJSON_GetObjectItem(rootfs, "device");
            if (cJSON_IsString(dev) && dev->valuestring[0] != '\0') {
                strncpy(buf, dev->valuestring, bufsize - 1);
                buf[bufsize - 1] = '\0';
                return;
            }
        }
    }
    fprintf(stderr, "bootloader: WARNING boot.fileSystems[\"/\"] missing; "
            "using %s as root device fallback\n", FALLBACK_ROOT_DEVICE);
    strncpy(buf, FALLBACK_ROOT_DEVICE, bufsize - 1);
    buf[bufsize - 1] = '\0';
}

/* Get the declared kernel name from boot.kernel. Default "linux". */
static const char *get_declared_kernel(cJSON *boot)
{
    cJSON *k = cJSON_GetObjectItem(boot, "kernel");
    if (cJSON_IsString(k) && k->valuestring[0] != '\0')
        return k->valuestring;
    return "linux";
}

/* Parse "set timeout=N" from extraConfig. Returns the last match's N,
 * or -1 if not found. The last match wins in GRUB, so we mirror that. */
static int parse_timeout(const char *extra_config)
{
    if (!extra_config) return -1;
    const char *p = extra_config;
    const char *last = NULL;
    while ((p = strstr(p, "set timeout=")) != NULL) {
        last = p;
        p += strlen("set timeout=");
    }
    if (!last) return -1;
    last += strlen("set timeout=");
    while (*last == ' ' || *last == '\t') last++;
    return atoi(last);
}

/* ── Generation enumeration ────────────────────────────────────────── */

typedef struct {
    int    id;
    time_t timestamp;
    char  *kernel_pkg;       /* kernel name for vmlinuz-<kernel>, or NULL */
    char  *hoshizora_store;  /* /nix/store/<hash>-hoshizora-<ver>, or NULL */
    int    has_declared_kernel;
    int    is_current;
} gen_info_t;

static void gen_info_free(gen_info_t *gens, size_t n)
{
    if (!gens) return;
    for (size_t i = 0; i < n; i++) {
        free(gens[i].kernel_pkg);
        free(gens[i].hoshizora_store);
    }
    free(gens);
}

/* Read one generation's manifest.json. Populates out->timestamp (from
 * the file mtime), out->kernel_pkg (the declared kernel if found, else
 * the first linux* package found, else NULL), out->hoshizora_store
 * (the store_path of the hoshizora package, or NULL), and
 * out->has_declared_kernel. Returns 0 on success, -1 on read/parse
 * failure. */
static int read_gen_manifest(const char *db_root, int id,
                             const char *declared_kernel, gen_info_t *out)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/generations/%d/manifest.json", db_root, id);

    struct stat st;
    if (stat(path, &st) != 0) return -1;
    out->timestamp = st.st_mtime;

    char *content = read_file(path);
    if (!content) return -1;

    cJSON *root = cJSON_Parse(content);
    free(content);
    if (!root) return -1;

    cJSON *pkgs = cJSON_GetObjectItem(root, "packages");
    if (cJSON_IsArray(pkgs)) {
        cJSON *item;
        cJSON_ArrayForEach(item, pkgs) {
            cJSON *jname = cJSON_GetObjectItem(item, "name");
            cJSON *jstore = cJSON_GetObjectItem(item, "store_path");
            if (!cJSON_IsString(jname)) continue;
            const char *name = jname->valuestring;

            if (strcmp(name, declared_kernel) == 0) {
                out->has_declared_kernel = 1;
                if (!out->kernel_pkg)
                    out->kernel_pkg = strdup(name);
            }

            if (!out->kernel_pkg && is_kernel_package(name))
                out->kernel_pkg = strdup(name);

            if (!out->hoshizora_store && strcmp(name, "hoshizora") == 0) {
                if (cJSON_IsString(jstore) && jstore->valuestring[0] != '\0')
                    out->hoshizora_store = strdup(jstore->valuestring);
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}

/* Get current generation ID from <db_root>/current symlink. Returns 0
 * if no current symlink exists. */
static int get_current_gen(const char *db_root)
{
    char link[PATH_MAX];
    snprintf(link, sizeof(link), "%s/current", db_root);
    char target[PATH_MAX];
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n < 0) return 0;
    target[n] = '\0';
    char *slash = strrchr(target, '/');
    if (!slash) return 0;
    return atoi(slash + 1);
}

/* Walk <db_root>/generations/, collect all generations sorted ascending
 * by ID. Returns 0 on success (even if the directory is missing or
 * empty), -1 on OOM. */
static int enum_generations(const char *db_root,
                            const char *declared_kernel,
                            gen_info_t **out_gens, size_t *out_n)
{
    *out_gens = NULL;
    *out_n = 0;
    if (!db_root) return 0;

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/generations", db_root);
    DIR *d = opendir(dir);
    if (!d) return 0;

    size_t cap = 16, n = 0;
    gen_info_t *gens = calloc(cap, sizeof(*gens));
    if (!gens) { closedir(d); return -1; }

    int current = get_current_gen(db_root);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        int id = atoi(ent->d_name);
        if (id <= 0) continue;

        if (n == cap) {
            cap *= 2;
            gen_info_t *grown = realloc(gens, cap * sizeof(*gens));
            if (!grown) { closedir(d); gen_info_free(gens, n); return -1; }
            gens = grown;
        }

        memset(&gens[n], 0, sizeof(gens[n]));
        gens[n].id = id;
        gens[n].is_current = (id == current);
        if (read_gen_manifest(db_root, id, declared_kernel, &gens[n]) != 0) {
            /* unreadable manifest: keep the entry with empty fields so
             * the generation still appears in the menu as a stub. */
        }
        n++;
    }
    closedir(d);

    /* Sort ascending by ID (simple selection sort; n is small). */
    for (size_t i = 0; i + 1 < n; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < n; j++)
            if (gens[j].id < gens[best].id) best = j;
        if (best != i) {
            gen_info_t tmp = gens[i];
            gens[i] = gens[best];
            gens[best] = tmp;
        }
    }

    *out_gens = gens;
    *out_n = n;
    return 0;
}

/* Format a generation's timestamp as YYYY-MM-DD into buf. Writes
 * "unknown" if timestamp is 0 or invalid. */
static void format_gen_date(time_t ts, char *buf, size_t bufsize)
{
    if (ts <= 0) { snprintf(buf, bufsize, "unknown"); return; }
    struct tm tm;
    if (!localtime_r(&ts, &tm)) { snprintf(buf, bufsize, "unknown"); return; }
    strftime(buf, bufsize, "%Y-%m-%d", &tm);
}

/* ── Initrd regeneration ───────────────────────────────────────────── */

/* Run mkinitcpio -P. If initrd_modules is a non-empty JSON array, write
 * a temporary mkinitcpio.conf with those MODULES and pass it via -c.
 * mkinitcpio exit 127 (binary not found) is logged and treated as
 * non-fatal. Returns 0 on success or graceful skip, non-zero on real
 * failure. */
static int regenerate_initrd(cJSON *initrd_modules, int dry_run)
{
    char tmp_path[] = "/tmp/2O9-mkinitcpio.XXXXXX";
    const char *tmp_conf = NULL;
    int tmp_fd = -1;

    if (initrd_modules && cJSON_IsArray(initrd_modules) &&
        cJSON_GetArraySize(initrd_modules) > 0) {
        tmp_fd = mkstemp(tmp_path);
        if (tmp_fd < 0) {
            fprintf(stderr, "bootloader: mkstemp for mkinitcpio.conf failed: %s\n",
                    strerror(errno));
            return -1;
        }
        FILE *f = fdopen(tmp_fd, "w");
        if (!f) {
            close(tmp_fd);
            unlink(tmp_path);
            return -1;
        }
        fprintf(f, "MODULES=(");
        cJSON *m;
        cJSON_ArrayForEach(m, initrd_modules) {
            if (cJSON_IsString(m)) fprintf(f, " %s", m->valuestring);
        }
        fprintf(f, " )\n");
        fprintf(f, "BINARIES=()\n");
        fprintf(f, "FILES=()\n");
        fprintf(f, "HOOKS=(base udev autodetect modconf block filesystems keyboard fsck)\n");
        fclose(f);
        tmp_conf = tmp_path;
    }

    int rc;
    if (dry_run) {
        if (tmp_conf)
            printf("bootloader: [dry-run] would run mkinitcpio -c %s -P\n", tmp_conf);
        else
            printf("bootloader: [dry-run] would run mkinitcpio -P\n");
        rc = 0;
    } else if (tmp_conf) {
        const char *argv[] = {"mkinitcpio", "-c", tmp_conf, "-P", NULL};
        rc = run_cmd(argv);
        if (rc == 127) {
            fprintf(stderr, "bootloader: mkinitcpio not installed; skipping initrd\n");
            rc = 0;
        } else if (rc != 0) {
            fprintf(stderr, "bootloader: mkinitcpio exited %d\n", rc);
        }
    } else {
        const char *argv[] = {"mkinitcpio", "-P", NULL};
        rc = run_cmd(argv);
        if (rc == 127) {
            fprintf(stderr, "bootloader: mkinitcpio not installed; skipping initrd\n");
            rc = 0;
        } else if (rc != 0) {
            fprintf(stderr, "bootloader: mkinitcpio exited %d\n", rc);
        }
    }

    if (tmp_conf) unlink(tmp_conf);
    return rc;
}

/* ── grub ──────────────────────────────────────────────────────────── */

/* Run grub-install for BIOS or UEFI. Returns 0 on success, non-zero on
 * failure (including binary not found). */
static int install_grub(int is_uefi, const char *device,
                        int efi_removable, int dry_run)
{
    if (is_uefi) {
        const char *argv[6] = {"grub-install", "--target=x86_64-efi",
                               "--efi-directory=/boot/EFI",
                               "--bootloader-id=2O9", NULL, NULL};
        if (efi_removable) argv[4] = "--removable";
        if (dry_run) {
            printf("bootloader: [dry-run] would run grub-install "
                   "--target=x86_64-efi --efi-directory=/boot/EFI "
                   "--bootloader-id=2O9%s\n", efi_removable ? " --removable" : "");
            return 0;
        }
        int rc = run_cmd(argv);
        if (rc == 127) {
            fprintf(stderr, "bootloader: grub-install not installed\n");
            return 1;
        }
        if (rc != 0) {
            fprintf(stderr, "bootloader: grub-install exited %d\n", rc);
            return rc;
        }
        return 0;
    }

    /* BIOS */
    if (dry_run) {
        printf("bootloader: [dry-run] would run grub-install --target=i386-pc %s\n",
               device);
        return 0;
    }
    const char *argv[] = {"grub-install", "--target=i386-pc", device, NULL};
    int rc = run_cmd(argv);
    if (rc == 127) {
        fprintf(stderr, "bootloader: grub-install not installed\n");
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "bootloader: grub-install exited %d\n", rc);
        return rc;
    }
    return 0;
}

/* Write one menuentry block for a generation to out. */
static void grub_emit_entry(FILE *out, const gen_info_t *g,
                            const char *root_device)
{
    const char *kern = g->kernel_pkg ? g->kernel_pkg : "linux";
    const char *init = g->hoshizora_store ? g->hoshizora_store
                                          : FALLBACK_HOSHIZORA;
    char datebuf[32];
    format_gen_date(g->timestamp, datebuf, sizeof(datebuf));
    fprintf(out, "menuentry \"2O9 Generation #%d (%s)\" {\n", g->id, datebuf);
    fprintf(out, "    linux /boot/vmlinuz-%s root=%s rw init=%s/init 2O9_GENERATION=%d\n",
            kern, root_device, init, g->id);
    fprintf(out, "    initrd /boot/initramfs-%s.img\n", kern);
    fprintf(out, "}\n");
}

/* Generate /boot/grub/grub.cfg. If use_osprober is true, run grub-mkconfig
 * first and append 2O9 entries; otherwise write the whole config from
 * scratch. extra_entries and extra_config are appended verbatim if present. */
static int generate_grub_cfg(const gen_info_t *gens, size_t n_gens,
                             const char *root_device,
                             int use_osprober,
                             const char *extra_entries,
                             const char *extra_config,
                             int dry_run)
{
    /* Step A: optionally run grub-mkconfig. */
    int mkconfig_rc = 0;
    if (use_osprober) {
        const char *argv[] = {"grub-mkconfig", "-o", GRUB_CFG_PATH, NULL};
        if (dry_run) {
            printf("bootloader: [dry-run] would run "
                   "GRUB_DISABLE_OS_PROBER=false grub-mkconfig -o %s\n",
                   GRUB_CFG_PATH);
        } else {
            mkconfig_rc = run_cmd_env(argv, "GRUB_DISABLE_OS_PROBER", "false");
            if (mkconfig_rc == 127) {
                fprintf(stderr, "bootloader: grub-mkconfig not installed; "
                        "writing custom grub.cfg instead\n");
                mkconfig_rc = 0;
                use_osprober = 0;
            } else if (mkconfig_rc != 0) {
                fprintf(stderr, "bootloader: grub-mkconfig exited %d; "
                        "continuing with custom grub.cfg\n", mkconfig_rc);
                mkconfig_rc = 0;
                use_osprober = 0;
            }
        }
    }

    /* Step B: build the 2O9 entries content. */
    char *entries = NULL;
    size_t entries_len = 0;
    FILE *mem = open_memstream(&entries, &entries_len);
    if (!mem) {
        fprintf(stderr, "bootloader: open_memstream failed\n");
        return -1;
    }

    fprintf(mem, "# 2O9 generation entries. Do not edit; run `209 apply` to regenerate.\n");
    if (n_gens == 0) {
        fprintf(mem, "# (no generations found; menu will be empty)\n");
    } else {
        /* Current generation first (default=0), then others descending. */
        for (size_t i = 0; i < n_gens; i++) {
            if (gens[i].is_current) grub_emit_entry(mem, &gens[i], root_device);
        }
        for (size_t i = n_gens; i > 0; i--) {
            if (gens[i - 1].is_current) continue;
            grub_emit_entry(mem, &gens[i - 1], root_device);
        }
    }

    if (extra_entries && extra_entries[0] != '\0')
        fprintf(mem, "\n# extraEntries from 2O9.nix\n%s\n", extra_entries);
    if (extra_config && extra_config[0] != '\0')
        fprintf(mem, "\n# extraConfig from 2O9.nix\n%s\n", extra_config);
    fclose(mem);

    /* Step C: write the config. */
    if (dry_run) {
        printf("bootloader: [dry-run] would %s %s (%zu bytes of 2O9 entries):\n",
               use_osprober ? "append to" : "write", GRUB_CFG_PATH, entries_len);
        printf("%s", entries);
        free(entries);
        return 0;
    }

    if (use_osprober) {
        /* Append to the file grub-mkconfig just wrote. */
        FILE *f = fopen(GRUB_CFG_PATH, "a");
        if (!f) {
            fprintf(stderr, "bootloader: cannot append to %s: %s\n",
                    GRUB_CFG_PATH, strerror(errno));
            free(entries);
            return -1;
        }
        fwrite(entries, 1, entries_len, f);
        fclose(f);
        printf("bootloader: appended %zu bytes of 2O9 entries to %s\n",
               entries_len, GRUB_CFG_PATH);
    } else {
        /* Write the whole file from scratch. */
        int timeout = parse_timeout(extra_config);
        if (timeout < 0) timeout = DEFAULT_TIMEOUT;

        char *content = NULL;
        size_t content_len = 0;
        FILE *full = open_memstream(&content, &content_len);
        if (!full) {
            fprintf(stderr, "bootloader: open_memstream failed\n");
            free(entries);
            return -1;
        }
        fprintf(full, "# Generated by 2O9. Do not edit; run `209 apply` to regenerate.\n");
        fprintf(full, "set default=0\n");
        fprintf(full, "set timeout=%d\n", timeout);
        fwrite(entries, 1, entries_len, full);
        fclose(full);

        int rc = write_file_atomic(GRUB_CFG_PATH, content, content_len, 0644);
        if (rc == 0)
            printf("bootloader: wrote %s (%zu bytes)\n",
                   GRUB_CFG_PATH, content_len);
        free(content);
        free(entries);
        return rc;
    }

    free(entries);
    return 0;
}

/* Apply grub configuration. Returns 0 on success, non-zero on error. */
static int apply_grub(cJSON *root, const char *db_root, int dry_run)
{
    cJSON *boot = cJSON_GetObjectItem(root, "boot");
    if (!boot) return 0;
    cJSON *loader = cJSON_GetObjectItem(boot, "loader");
    if (!loader) return 0;
    cJSON *grub = cJSON_GetObjectItem(loader, "grub");
    if (!grub) return 0;
    if (!cJSON_IsTrue(cJSON_GetObjectItem(grub, "enable"))) return 0;

    cJSON *device = cJSON_GetObjectItem(grub, "device");
    cJSON *efi_support = cJSON_GetObjectItem(grub, "efiSupport");
    cJSON *efi_removable = cJSON_GetObjectItem(grub, "efiInstallAsRemovable");
    cJSON *extra_entries = cJSON_GetObjectItem(grub, "extraEntries");
    cJSON *extra_config = cJSON_GetObjectItem(grub, "extraConfig");
    cJSON *use_osprober = cJSON_GetObjectItem(grub, "useOSProber");
    cJSON *initrd_modules = cJSON_GetObjectItem(boot, "initrdModules");
    const char *declared_kernel = get_declared_kernel(boot);

    int is_uefi = cJSON_IsTrue(efi_support) ||
                  (cJSON_IsString(device) &&
                   strcmp(device->valuestring, "nodev") == 0);

    if (!is_uefi) {
        if (!cJSON_IsString(device) || device->valuestring[0] == '\0') {
            fprintf(stderr, "bootloader: grub.device required for BIOS install\n");
            return 1;
        }
    }

    /* 1. grub-install. */
    int rc = install_grub(is_uefi,
                          cJSON_IsString(device) ? device->valuestring : NULL,
                          cJSON_IsTrue(efi_removable), dry_run);
    if (rc != 0) return rc;

    /* 2. Ensure /boot/grub/ exists. */
    if (!dry_run && mkdir_p(GRUB_DIR) != 0) {
        fprintf(stderr, "bootloader: cannot create %s: %s\n",
                GRUB_DIR, strerror(errno));
        return 1;
    }

    /* 3. Enumerate generations. */
    gen_info_t *gens = NULL;
    size_t n_gens = 0;
    if (enum_generations(db_root, declared_kernel, &gens, &n_gens) != 0) {
        fprintf(stderr, "bootloader: out of memory enumerating generations\n");
        return 1;
    }

    /* 4. Root device. */
    char root_device[PATH_MAX];
    get_root_device(root, root_device, sizeof(root_device));

    /* 5. Generate grub.cfg. */
    rc = generate_grub_cfg(gens, n_gens, root_device,
                           cJSON_IsTrue(use_osprober),
                           cJSON_IsString(extra_entries) ? extra_entries->valuestring : NULL,
                           cJSON_IsString(extra_config) ? extra_config->valuestring : NULL,
                           dry_run);
    if (rc != 0) {
        gen_info_free(gens, n_gens);
        return rc;
    }

    /* 6. Kernel check + initrd regeneration. */
    int kernel_present = 0;
    for (size_t i = 0; i < n_gens; i++) {
        if (gens[i].is_current) {
            kernel_present = gens[i].has_declared_kernel;
            break;
        }
    }
    if (!kernel_present) {
        printf("bootloader: declared kernel '%s' not in current generation's "
               "package list; run `209 -S %s` first. Skipping initrd.\n",
               declared_kernel, declared_kernel);
        gen_info_free(gens, n_gens);
        return 0;
    }

    rc = regenerate_initrd(initrd_modules, dry_run);
    gen_info_free(gens, n_gens);
    return rc == 0 ? 0 : 1;
}

/* ── systemd-boot ──────────────────────────────────────────────────── */

/* Run bootctl install or bootctl update depending on whether the EFI
 * binary already exists. Returns 0 on success, non-zero on failure. */
static int install_systemd_boot(int dry_run)
{
    struct stat st;
    int needs_install = (stat(SYSTEMD_BOOT_EFI, &st) != 0);

    const char *argv[] = {"bootctl", needs_install ? "install" : "update", NULL};
    if (dry_run) {
        printf("bootloader: [dry-run] would run bootctl %s\n",
               needs_install ? "install" : "update");
        return 0;
    }

    if (needs_install && mkdir_p(EFI_DIR) != 0) {
        fprintf(stderr, "bootloader: cannot create %s: %s\n",
                EFI_DIR, strerror(errno));
        return 1;
    }

    int rc = run_cmd(argv);
    if (rc == 127) {
        fprintf(stderr, "bootloader: bootctl not installed\n");
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "bootloader: bootctl %s exited %d\n",
                needs_install ? "install" : "update", rc);
        return rc;
    }
    return 0;
}

/* Write /boot/loader/loader.conf. console_mode and editor come from the
 * manifest; default points at the current generation's entry. */
static int write_loader_conf(const char *console_mode, int editor,
                             int current_gen, int dry_run)
{
    char *content = NULL;
    size_t content_len = 0;
    FILE *mem = open_memstream(&content, &content_len);
    if (!mem) {
        fprintf(stderr, "bootloader: open_memstream failed\n");
        return -1;
    }
    fprintf(mem, "# Generated by 2O9. Do not edit; run `209 apply` to regenerate.\n");
    fprintf(mem, "default 2O9-%d\n", current_gen);
    if (console_mode && console_mode[0] != '\0')
        fprintf(mem, "console-mode %s\n", console_mode);
    fprintf(mem, "editor %s\n", editor ? "yes" : "no");
    fprintf(mem, "timeout 5\n");
    fclose(mem);

    if (dry_run) {
        printf("bootloader: [dry-run] would write %s (%zu bytes):\n%s",
               SYSTEMD_LOADER_CONF, content_len, content);
        free(content);
        return 0;
    }

    if (mkdir_p("/boot/loader") != 0) {
        fprintf(stderr, "bootloader: cannot create /boot/loader: %s\n",
                strerror(errno));
        free(content);
        return -1;
    }

    int rc = write_file_atomic(SYSTEMD_LOADER_CONF, content, content_len, 0644);
    if (rc == 0)
        printf("bootloader: wrote %s (%zu bytes)\n",
               SYSTEMD_LOADER_CONF, content_len);
    free(content);
    return rc;
}

/* Write one entry file per generation under /boot/loader/entries/. */
static int write_systemd_entries(const gen_info_t *gens, size_t n_gens,
                                 const char *root_device, int dry_run)
{
    if (!dry_run && mkdir_p(SYSTEMD_ENTRIES_DIR) != 0) {
        fprintf(stderr, "bootloader: cannot create %s: %s\n",
                SYSTEMD_ENTRIES_DIR, strerror(errno));
        return 1;
    }

    int errors = 0;
    for (size_t i = 0; i < n_gens; i++) {
        const gen_info_t *g = &gens[i];
        const char *kern = g->kernel_pkg ? g->kernel_pkg : "linux";
        const char *init = g->hoshizora_store ? g->hoshizora_store
                                              : FALLBACK_HOSHIZORA;

        char *content = NULL;
        size_t content_len = 0;
        FILE *mem = open_memstream(&content, &content_len);
        if (!mem) { errors++; continue; }
        fprintf(mem, "title   2O9 Generation #%d\n", g->id);
        fprintf(mem, "linux   /vmlinuz-%s\n", kern);
        fprintf(mem, "initrd  /initramfs-%s.img\n", kern);
        fprintf(mem, "options root=%s rw init=%s/init 2O9_GENERATION=%d\n",
                root_device, init, g->id);
        fclose(mem);

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/2O9-%d.conf", SYSTEMD_ENTRIES_DIR, g->id);

        if (dry_run) {
            printf("bootloader: [dry-run] would write %s (%zu bytes)\n",
                   path, content_len);
            free(content);
            continue;
        }

        if (write_file_atomic(path, content, content_len, 0644) != 0)
            errors++;
        else
            printf("bootloader: wrote %s\n", path);
        free(content);
    }
    return errors == 0 ? 0 : 1;
}

/* Apply systemd-boot configuration. Returns 0 on success, non-zero on error. */
static int apply_systemd_boot(cJSON *root, const char *db_root, int dry_run)
{
    cJSON *boot = cJSON_GetObjectItem(root, "boot");
    if (!boot) return 0;
    cJSON *loader = cJSON_GetObjectItem(boot, "loader");
    if (!loader) return 0;
    cJSON *sd = cJSON_GetObjectItem(loader, "systemd-boot");
    if (!sd) return 0;
    if (!cJSON_IsTrue(cJSON_GetObjectItem(sd, "enable"))) return 0;

    cJSON *console_mode = cJSON_GetObjectItem(sd, "consoleMode");
    cJSON *editor = cJSON_GetObjectItem(sd, "editor");
    cJSON *initrd_modules = cJSON_GetObjectItem(boot, "initrdModules");
    const char *declared_kernel = get_declared_kernel(boot);

    /* 1. bootctl install / update. */
    int rc = install_systemd_boot(dry_run);
    if (rc != 0) return rc;

    /* 2. Enumerate generations. */
    gen_info_t *gens = NULL;
    size_t n_gens = 0;
    if (enum_generations(db_root, declared_kernel, &gens, &n_gens) != 0) {
        fprintf(stderr, "bootloader: out of memory enumerating generations\n");
        return 1;
    }

    /* 3. Root device. */
    char root_device[PATH_MAX];
    get_root_device(root, root_device, sizeof(root_device));

    /* 4. Write loader.conf with the current generation as default. */
    int current_gen = 0;
    for (size_t i = 0; i < n_gens; i++) {
        if (gens[i].is_current) { current_gen = gens[i].id; break; }
    }
    if (current_gen == 0 && n_gens > 0)
        current_gen = gens[n_gens - 1].id;  /* newest as fallback */
    if (current_gen == 0) {
        fprintf(stderr, "bootloader: no generations found; writing loader.conf "
                "with no default\n");
    }

    rc = write_loader_conf(cJSON_IsString(console_mode) ? console_mode->valuestring : NULL,
                           cJSON_IsTrue(editor), current_gen, dry_run);
    if (rc != 0) {
        gen_info_free(gens, n_gens);
        return rc;
    }

    /* 5. Write per-generation entry files. */
    rc = write_systemd_entries(gens, n_gens, root_device, dry_run);
    if (rc != 0) {
        gen_info_free(gens, n_gens);
        return rc;
    }

    /* 6. Kernel check + initrd regeneration. */
    int kernel_present = 0;
    for (size_t i = 0; i < n_gens; i++) {
        if (gens[i].is_current) {
            kernel_present = gens[i].has_declared_kernel;
            break;
        }
    }
    if (!kernel_present) {
        printf("bootloader: declared kernel '%s' not in current generation's "
               "package list; run `209 -S %s` first. Skipping initrd.\n",
               declared_kernel, declared_kernel);
        gen_info_free(gens, n_gens);
        return 0;
    }

    rc = regenerate_initrd(initrd_modules, dry_run);
    gen_info_free(gens, n_gens);
    return rc == 0 ? 0 : 1;
}

/* ── Loader type detection (for the prev-vs-current warning) ───────── */

/* Return 1 if the given manifest enables grub, 0 otherwise. */
static int manifest_enables_grub(const char *manifest_json)
{
    if (!manifest_json) return 0;
    cJSON *root = cJSON_Parse(manifest_json);
    if (!root) return 0;
    cJSON *boot = cJSON_GetObjectItem(root, "boot");
    cJSON *loader = boot ? cJSON_GetObjectItem(boot, "loader") : NULL;
    cJSON *grub = loader ? cJSON_GetObjectItem(loader, "grub") : NULL;
    int enabled = grub ? cJSON_IsTrue(cJSON_GetObjectItem(grub, "enable")) : 0;
    cJSON_Delete(root);
    return enabled;
}

/* Return 1 if the given manifest enables systemd-boot, 0 otherwise. */
static int manifest_enables_sdboot(const char *manifest_json)
{
    if (!manifest_json) return 0;
    cJSON *root = cJSON_Parse(manifest_json);
    if (!root) return 0;
    cJSON *boot = cJSON_GetObjectItem(root, "boot");
    cJSON *loader = boot ? cJSON_GetObjectItem(boot, "loader") : NULL;
    cJSON *sd = loader ? cJSON_GetObjectItem(loader, "systemd-boot") : NULL;
    int enabled = sd ? cJSON_IsTrue(cJSON_GetObjectItem(sd, "enable")) : 0;
    cJSON_Delete(root);
    return enabled;
}

/* ── Main entry point ──────────────────────────────────────────────── */

int bootloader_apply(const char *manifest_json,
                     const char *prev_manifest_json,
                     const char *db_root, int dry_run)
{
    if (getuid() != 0) {
        fprintf(stderr, "bootloader: must run as root (writes /boot, runs "
                "grub-install/bootctl/mkinitcpio)\n");
        return 1;
    }
    if (!manifest_json) {
        fprintf(stderr, "bootloader: manifest_json is NULL\n");
        return 1;
    }

    cJSON *root = cJSON_Parse(manifest_json);
    if (!root) {
        fprintf(stderr, "bootloader: failed to parse manifest JSON\n");
        return 1;
    }

    cJSON *boot = cJSON_GetObjectItem(root, "boot");
    cJSON *loader = boot ? cJSON_GetObjectItem(boot, "loader") : NULL;
    cJSON *grub = loader ? cJSON_GetObjectItem(loader, "grub") : NULL;
    cJSON *sd = loader ? cJSON_GetObjectItem(loader, "systemd-boot") : NULL;

    int grub_enabled = grub ? cJSON_IsTrue(cJSON_GetObjectItem(grub, "enable")) : 0;
    int sd_enabled = sd ? cJSON_IsTrue(cJSON_GetObjectItem(sd, "enable")) : 0;

    if (grub_enabled && sd_enabled) {
        fprintf(stderr, "bootloader: cannot enable both grub and systemd-boot\n");
        cJSON_Delete(root);
        return 1;
    }

    if (!grub_enabled && !sd_enabled) {
        printf("bootloader: no bootloader configured\n");
        cJSON_Delete(root);
        return 0;
    }

    /* Warn if the bootloader type changed between generations. Cleanup
     * of the old bootloader's files (/boot/grub, /boot/loader/entries,
     * /boot/EFI/systemd) is the user's job. */
    if (prev_manifest_json) {
        int prev_grub = manifest_enables_grub(prev_manifest_json);
        int prev_sd = manifest_enables_sdboot(prev_manifest_json);
        if (grub_enabled && prev_sd) {
            fprintf(stderr, "bootloader: WARNING switched from systemd-boot to "
                    "grub; old /boot/loader/entries and /boot/EFI/systemd left "
                    "in place (clean up manually if desired)\n");
        }
        if (sd_enabled && prev_grub) {
            fprintf(stderr, "bootloader: WARNING switched from grub to "
                    "systemd-boot; old /boot/grub left in place (clean up "
                    "manually if desired)\n");
        }
    }

    int rc;
    if (grub_enabled)
        rc = apply_grub(root, db_root, dry_run);
    else
        rc = apply_systemd_boot(root, db_root, dry_run);

    cJSON_Delete(root);
    return rc;
}
