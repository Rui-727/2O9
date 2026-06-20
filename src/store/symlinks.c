/* symlinks.c — 2O9 symlink farm builder implementation
 *
 * Walks a generation's package manifest, creates symlinks from
 * store paths to user-visible locations.
 *
 * From DESIGN.md §7:
 *   binaries:    ~/.local/bin/    →  store  (per user, no root needed)
 *   libraries:   ~/.local/lib/    →  store  (per user, no root needed)
 *   share:       ~/.local/share/  →  store  (per user, no root needed)
 *   config:      /etc/            →  store  (system, root needed)
 *   system bins: /usr/bin/        →  store  (system, root needed)
 *
 * Classification rules:
 *   store_path/bin/*        → ~/.local/bin/<basename>
 *   store_path/usr/bin/*    → ~/.local/bin/<basename>
 *   store_path/lib/*        → ~/.local/lib/<basename>
 *   store_path/usr/lib/*    → ~/.local/lib/<basename>
 *   store_path/etc/*        → /etc/<rest-of-path>
 *   store_path/usr/share/*  → ~/.local/share/<rest-of-path>
 *   store_path/share/*      → ~/.local/share/<rest-of-path>
 *
 * No daemon. Symlinks written directly on 209 apply / 209 install.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <dirent.h>

#include "symlinks.h"
#include "gen.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

static int mkdirs(const char *path)
{
	char tmp[PATH_MAX];
	char *p = NULL;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
		return -1;

	return 0;
}

static char *get_home_dir(void)
{
	const char *home = getenv("HOME");
	if (home) return strdup(home);

	struct passwd *pw = getpwuid(getuid());
	if (pw) return strdup(pw->pw_dir);

	return strdup("/root");
}

/* Get the basename of a path (after last /) */
static const char *my_basename(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/* ── Symlink primitives ──────────────────────────────────────────── */

int symlink_create(const char *target, const char *link_path)
{
	/* Create parent directory */
	char parent[PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", link_path);
	char *slash = strrchr(parent, '/');
	if (slash) {
		*slash = '\0';
		if (mkdirs(parent) < 0)
			return -1;
	}

	/* Remove existing file/symlink at link_path */
	unlink(link_path);

	/* Create symlink */
	if (symlink(target, link_path) < 0)
		return -1;

	return 0;
}

int symlink_remove(const char *link_path)
{
	/* Only remove if it's a symlink — don't nuke real files */
	struct stat st;
	if (lstat(link_path, &st) < 0)
		return 0;  /* already gone */
	if (!S_ISLNK(st.st_mode))
		return 0;  /* not a symlink, leave it alone */

	return unlink(link_path);
}

/* ── Classification and symlink creation ─────────────────────────── */

/* Classify a single file and create its symlink.
 * rel is the relative path from store_path root.
 * abs_path is the absolute path in the store. */
int symlink_file(const char *store_path, const char *rel,
                 const char *home, const char *abs_path)
{
	char link_path[PATH_MAX];
	link_path[0] = '\0';

	(void)store_path;

	/* Classification rules:
	 *   bin/*       → ~/.local/bin/<basename>
	 *   usr/bin/*   → ~/.local/bin/<basename>
	 *   sbin/*      → ~/.local/bin/<basename>
	 *   usr/sbin/*  → ~/.local/bin/<basename>
	 *   lib/*       → ~/.local/lib/<basename>
	 *   usr/lib/*   → ~/.local/lib/<basename>
	 *   lib64/*     → ~/.local/lib/<basename>
	 *   etc/*       → /etc/<rest>
	 *   share/*     → ~/.local/share/<rest>
	 *   usr/share/* → ~/.local/share/<rest>
	 *   include/*   → ~/.local/include/<rest>
	 */

	if (strncmp(rel, "bin/", 4) == 0 ||
	    strncmp(rel, "sbin/", 5) == 0) {
		/* ~/.local/bin/<basename> */
		snprintf(link_path, sizeof(link_path), "%s/.local/bin/%s",
		         home, my_basename(rel));
	} else if (strncmp(rel, "usr/bin/", 8) == 0 ||
	           strncmp(rel, "usr/sbin/", 9) == 0) {
		/* ~/.local/bin/<basename> */
		snprintf(link_path, sizeof(link_path), "%s/.local/bin/%s",
		         home, my_basename(rel));
	} else if (strncmp(rel, "lib/", 4) == 0 ||
	           strncmp(rel, "lib64/", 6) == 0) {
		/* ~/.local/lib/<basename> */
		snprintf(link_path, sizeof(link_path), "%s/.local/lib/%s",
		         home, my_basename(rel));
	} else if (strncmp(rel, "usr/lib/", 8) == 0) {
		/* ~/.local/lib/<basename> */
		snprintf(link_path, sizeof(link_path), "%s/.local/lib/%s",
		         home, my_basename(rel));
	} else if (strncmp(rel, "etc/", 4) == 0) {
		/* /etc/<rest> — needs root, skip if not root */
		if (geteuid() == 0) {
			snprintf(link_path, sizeof(link_path), "/etc/%s", rel + 4);
		}
		/* Non-root: skip /etc symlinks */
	} else if (strncmp(rel, "share/", 6) == 0) {
		/* ~/.local/share/<rest> */
		snprintf(link_path, sizeof(link_path), "%s/.local/share/%s",
		         home, rel + 6);
	} else if (strncmp(rel, "usr/share/", 10) == 0) {
		/* ~/.local/share/<rest> */
		snprintf(link_path, sizeof(link_path), "%s/.local/share/%s",
		         home, rel + 10);
	} else if (strncmp(rel, "include/", 8) == 0) {
		/* ~/.local/include/<rest> */
		snprintf(link_path, sizeof(link_path), "%s/.local/include/%s",
		         home, rel + 8);
	}

	if (link_path[0] == '\0')
		return 0;  /* unclassified file, skip */

	/* Create the symlink */
	if (symlink_create(abs_path, link_path) < 0) {
		/* Not fatal — might be a permissions issue or conflict */
		fprintf(stderr, "  warning: could not create symlink %s → %s: %s\n",
		        link_path, abs_path, strerror(errno));
	}

	return 0;
}

/* Recursively create symlinks for files under a store subdirectory.
 * dir_rel is the relative path from store_path root (e.g. "bin", "etc/ssh"). */
int symlink_package_dir(const char *store_path, const char *dir_rel,
                        const char *home)
{
	char abs_dir[PATH_MAX];
	snprintf(abs_dir, sizeof(abs_dir), "%s/%s", store_path, dir_rel);

	DIR *d = opendir(abs_dir);
	if (!d) return -1;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		char rel[PATH_MAX];
		snprintf(rel, sizeof(rel), "%s/%s", dir_rel, ent->d_name);

		char abs_path[PATH_MAX];
		snprintf(abs_path, sizeof(abs_path), "%s/%s", store_path, rel);

		struct stat st;
		if (lstat(abs_path, &st) < 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			symlink_package_dir(store_path, rel, home);
		} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
			symlink_file(store_path, rel, home, abs_path);
		}
	}

	closedir(d);
	return 0;
}

/* ── Symlink farm for one package ────────────────────────────────── */

/* Create symlinks for all files in a store path.
 * Walks the store directory and creates symlinks according to
 * the classification rules above. */
static int symlink_package(const char *store_path, const char *pkg_name)
{
	char *home = get_home_dir();
	if (!home) return -1;

	/* Walk the store directory */
	DIR *d = opendir(store_path);
	if (!d) {
		/* Store path doesn't exist — maybe not extracted yet.
		 * Not an error for test mode. */
		free(home);
		return 0;
	}

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		/* Skip package metadata files at root */
		if (strcmp(ent->d_name, ".PKGINFO") == 0 ||
		    strcmp(ent->d_name, ".INSTALL") == 0 ||
		    strcmp(ent->d_name, ".MTREE") == 0 ||
		    strcmp(ent->d_name, ".BUILDINFO") == 0)
			continue;

		char abs_path[PATH_MAX];
		snprintf(abs_path, sizeof(abs_path), "%s/%s", store_path, ent->d_name);

		struct stat st;
		if (lstat(abs_path, &st) < 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			/* Recurse into this directory */
			symlink_package_dir(store_path, ent->d_name, home);
		} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
			/* Classify and create symlink */
			symlink_file(store_path, ent->d_name, home, abs_path);
		}
	}

	closedir(d);
	free(home);
	(void)pkg_name;
	return 0;
}

/* ── Teardown helpers ────────────────────────────────────────────── */

/* Remove the user-visible symlink for a classified file. */
int teardown_file(const char *rel, const char *home)
{
	char link_path[PATH_MAX];
	link_path[0] = '\0';

	if (strncmp(rel, "bin/", 4) == 0 ||
	    strncmp(rel, "sbin/", 5) == 0) {
		snprintf(link_path, sizeof(link_path), "%s/.local/bin/%s",
		         home, my_basename(rel));
	} else if (strncmp(rel, "usr/bin/", 8) == 0 ||
	           strncmp(rel, "usr/sbin/", 9) == 0) {
		snprintf(link_path, sizeof(link_path), "%s/.local/bin/%s",
		         home, my_basename(rel));
	} else if (strncmp(rel, "lib/", 4) == 0 ||
	           strncmp(rel, "lib64/", 6) == 0) {
		snprintf(link_path, sizeof(link_path), "%s/.local/lib/%s",
		         home, my_basename(rel));
	} else if (strncmp(rel, "usr/lib/", 8) == 0) {
		snprintf(link_path, sizeof(link_path), "%s/.local/lib/%s",
		         home, my_basename(rel));
	} else if (strncmp(rel, "etc/", 4) == 0) {
		if (geteuid() == 0) {
			snprintf(link_path, sizeof(link_path), "/etc/%s", rel + 4);
		}
	} else if (strncmp(rel, "share/", 6) == 0) {
		snprintf(link_path, sizeof(link_path), "%s/.local/share/%s",
		         home, rel + 6);
	} else if (strncmp(rel, "usr/share/", 10) == 0) {
		snprintf(link_path, sizeof(link_path), "%s/.local/share/%s",
		         home, rel + 10);
	} else if (strncmp(rel, "include/", 8) == 0) {
		snprintf(link_path, sizeof(link_path), "%s/.local/include/%s",
		         home, rel + 8);
	}

	if (link_path[0] == '\0')
		return 0;

	/* Only remove if it's a symlink pointing into /nix/store */
	struct stat st;
	if (lstat(link_path, &st) == 0 && S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t n = readlink(link_path, target, sizeof(target) - 1);
		if (n > 0) {
			target[n] = '\0';
			if (strncmp(target, "/nix/store/", 11) == 0) {
				unlink(link_path);
			}
		}
	}

	return 0;
}

/* Recursively remove symlinks for files under a store subdirectory. */
int teardown_dir(const char *store_path, const char *dir_rel,
                 const char *home)
{
	char abs_dir[PATH_MAX];
	snprintf(abs_dir, sizeof(abs_dir), "%s/%s", store_path, dir_rel);

	DIR *d = opendir(abs_dir);
	if (!d) return -1;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		char rel[PATH_MAX];
		snprintf(rel, sizeof(rel), "%s/%s", dir_rel, ent->d_name);

		char abs_path[PATH_MAX];
		snprintf(abs_path, sizeof(abs_path), "%s/%s", store_path, rel);

		struct stat st;
		if (lstat(abs_path, &st) < 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			teardown_dir(store_path, rel, home);
		} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
			teardown_file(rel, home);
		}
	}

	closedir(d);
	return 0;
}

/* Remove symlinks for one package by walking its store path. */
static int teardown_package(const char *store_path, const char *pkg_name,
                            const char *home)
{
	DIR *d = opendir(store_path);
	if (!d) return 0;  /* already gone */

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		if (strcmp(ent->d_name, ".PKGINFO") == 0 ||
		    strcmp(ent->d_name, ".INSTALL") == 0 ||
		    strcmp(ent->d_name, ".MTREE") == 0 ||
		    strcmp(ent->d_name, ".BUILDINFO") == 0)
			continue;

		char abs_path[PATH_MAX];
		snprintf(abs_path, sizeof(abs_path), "%s/%s", store_path, ent->d_name);

		struct stat st;
		if (lstat(abs_path, &st) < 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			teardown_dir(store_path, ent->d_name, home);
		} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
			teardown_file(ent->d_name, home);
		}
	}

	closedir(d);
	(void)pkg_name;
	return 0;
}

/* ── Build symlink farm for a generation ─────────────────────────── */

int symlink_farm_build(gen_db_t *db, gen_t *gen, gen_t *prev_gen)
{
	if (!gen) return -1;

	/* If we have a previous generation, tear down its symlinks first.
	 * This is the simple approach — remove all, then rebuild.
	 * A smarter diff-based approach would only touch changed files,
	 * but that's an optimization for later. */
	if (prev_gen) {
		symlink_farm_teardown(prev_gen);
	}

	/* Build symlinks for each package in the new generation */
	gen_pkg_t *pkg = gen->packages;
	while (pkg) {
		if (pkg->store_path) {
			symlink_package(pkg->store_path, pkg->name);
		}
		pkg = pkg->next;
	}

	(void)db;
	return 0;
}

/* ── Teardown ────────────────────────────────────────────────────── */

int symlink_farm_teardown(gen_t *gen)
{
	if (!gen) return -1;

	char *home = get_home_dir();
	if (!home) return -1;

	/* Walk packages and remove their symlinks */
	gen_pkg_t *pkg = gen->packages;
	while (pkg) {
		if (pkg->store_path) {
			teardown_package(pkg->store_path, pkg->name, home);
		}
		pkg = pkg->next;
	}

	free(home);
	return 0;
}
