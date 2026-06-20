/* 2O9 CLI — main entry point
 *
 * The `209` binary. Phase 0: just -V (version).
 * Later phases will add SOV command dispatch, lib209 linking, etc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <config.h>

#define VERSION_STR PACKAGE " " PACKAGE_VERSION

static void usage(void)
{
	printf("Usage: 209 [options] <subject> <verb>\n");
	printf("       209 [options] <command>\n\n");
	printf("Unified package manager: pacman + AUR + Nix store\n\n");
	printf("Commands:\n");
	printf("  apply              Apply declarative config (2O9.nix)\n");
	printf("  generations        List generations\n");
	printf("  sync               Sync repo databases\n");
	printf("  gc                 Garbage-collect unreferenced store paths\n");
	printf("  news               Show Arch Linux news\n\n");
	printf("SOV patterns:\n");
	printf("  209 <pkg> install  Install package from repo\n");
	printf("  209 <pkg> remove   Remove package\n");
	printf("  209 <pkg> aur build    Build from AUR\n");
	printf("  209 <pkg> trakker [flags]  Run in sandbox\n\n");
	printf("Options:\n");
	printf("  -V, --version      Show version\n");
	printf("  -h, --help         Show this help\n");
}

static void version(void)
{
	printf("%s\n", VERSION_STR);
	printf("License: GPL-2.0-only\n");
	printf("Store root: %s\n", STORE_ROOT);
}

int main(int argc, char *argv[])
{
	/* Phase 0: handle -V / --version / -h / --help.
	 * Everything else is "not yet implemented". */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-V") == 0 ||
		    strcmp(argv[i], "--version") == 0) {
			version();
			return 0;
		}
		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0) {
			usage();
			return 0;
		}
	}

	/* No arguments or unknown command */
	if (argc == 1) {
		usage();
		return 1;
	}

	/* TODO: SOV command dispatch (Phase 1+) */
	fprintf(stderr, "209: command not yet implemented: %s\n", argv[1]);
	fprintf(stderr, "    (Phase 0 — only -V and -h work for now)\n");
	return 1;
}
