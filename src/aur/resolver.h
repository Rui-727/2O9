/* resolver.h - AUR dependency resolution
 *
 * Rewrites paru's resolver.rs + aur_depends crate in C.
 * Resolves transitive AUR dependencies using libalpm + AUR RPC.
 */

#ifndef TWO9_RESOLVER_H
#define TWO9_RESOLVER_H

#include "aur_rpc.h"

/* A resolved build action */
typedef struct resolve_action {
	char *name;
	char *version;
	int is_aur;            /* 1 = AUR, 0 = repo */
	char *pkgbase;         /* AUR base package name */
	struct resolve_action *next;
} resolve_action_t;

/* Full resolution result */
typedef struct resolve_result {
	resolve_action_t *install;   /* packages to install from repo */
	resolve_action_t *build;     /* packages to build from AUR */
	resolve_action_t *missing;   /* unresolved dependencies */
} resolve_result_t;

/* Resolve dependencies for a list of targets.
 * Mixes libalpm (repo deps) with AUR RPC queries (AUR deps).
 * Returns a plan: what to install from repos, what to build from AUR. */
resolve_result_t *resolve_targets(aur_cache_t *cache,
                                  const char **targets, size_t count);

/* Free a resolve result */
void resolve_result_free(resolve_result_t *r);

#endif /* TWO9_RESOLVER_H */
