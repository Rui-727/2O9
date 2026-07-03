/* test_aur_rpc.c - quick smoke test for AUR RPC client */

#include <stdio.h>
#include <stdlib.h>
#include "aur_rpc.h"

int main(int argc, char *argv[])
{
	const char *query = argc > 1 ? argv[1] : "neovim";

	printf("=== AUR Search: %s ===\n", query);

	aur_cache_t *cache = aur_cache_open(NULL);
	if (!cache) {
		fprintf(stderr, "Failed to open AUR cache\n");
		return 1;
	}

	aur_rpc_result_t result = aur_search(cache, query, NULL);

	if (!result.success) {
		fprintf(stderr, "AUR RPC error: %s\n",
		        result.error ? result.error : "unknown");
		aur_rpc_result_free(&result);
		aur_cache_close(cache);
		return 1;
	}

	printf("Found %zu results:\n\n", result.count);

	aur_pkg_t *pkg = result.packages;
	int shown = 0;
	while (pkg && shown < 20) {
		printf("  %-30s %-20s %s\n",
		       pkg->name ? pkg->name : "?",
		       pkg->version ? pkg->version : "?",
		       pkg->description ? pkg->description : "");
		if (pkg->depends_count > 0)
			printf("    depends: %zu  makedepends: %zu\n",
			       pkg->depends_count, pkg->makedepends_count);
		pkg = pkg->next;
		shown++;
	}

	if (result.count > 20)
		printf("  ... and %zu more\n", result.count - 20);

	/* Test batch info */
	if (result.packages && result.packages->name) {
		printf("\n=== AUR Info: %s ===\n", result.packages->name);
		aur_rpc_result_t info = aur_info(cache, result.packages->name);
		if (info.success && info.packages) {
			aur_pkg_t *p = info.packages;
			printf("  Name:        %s\n", p->name);
			printf("  Version:     %s\n", p->version);
			printf("  Description: %s\n", p->description);
			printf("  URL:         %s\n", p->url);
			printf("  Votes:       %d\n", p->num_votes);
			printf("  Maintainer:  %s\n",
			       p->maintainer ? p->maintainer : "(orphan)");
			printf("  OutOfDate:   %s\n", p->out_of_date ? "YES" : "no");
		}
		aur_rpc_result_free(&info);
	}

	aur_rpc_result_free(&result);
	aur_cache_close(cache);
	return 0;
}
