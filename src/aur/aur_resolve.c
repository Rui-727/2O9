/* aur_resolve.c - AUR dependency resolution with topological sort
 *
 * Rewrites paru's resolver.rs + aur_depends crate in C.
 * Resolves transitive AUR dependencies using AUR RPC.
 *
 * The resolver answers: "Given these target packages, what do I need to
 * install from repos, and what do I need to build from AUR?"
 *
 * Build order is topologically sorted: dependencies come before dependents.
 * Uses Kahn's algorithm (BFS-based topological sort).
 *
 * Algorithm:
 *   1. For each target, query AUR RPC for info
 *   2. Separate deps into: already installed (gen DB), repo (libalpm),
 *      AUR (needs recursive resolution), missing
 *   3. Recurse for AUR deps until no new AUR packages are found
 *   4. Build dependency graph (adjacency list) for AUR packages
 *   5. Topologically sort the build list
 *   6. Return the plan
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resolver.h"
#include "aur_rpc.h"
#include "pgp.h"

/* ── String set (simple linear, dedup) ────────────────────────────── */

typedef struct str_set {
        char **items;
        size_t count;
        size_t cap;
} str_set_t;

static str_set_t *str_set_new(void)
{
        str_set_t *s = calloc(1, sizeof(*s));
        s->cap = 32;
        s->items = calloc(s->cap, sizeof(char *));
        s->count = 0;
        return s;
}

static void str_set_free(str_set_t *s)
{
        if (!s) return;
        for (size_t i = 0; i < s->count; i++)
                free(s->items[i]);
        free(s->items);
        free(s);
}

static int str_set_contains(str_set_t *s, const char *item)
{
        for (size_t i = 0; i < s->count; i++) {
                if (strcmp(s->items[i], item) == 0)
                        return 1;
        }
        return 0;
}

static int str_set_add(str_set_t *s, const char *item)
{
        if (str_set_contains(s, item))
                return 0;

        if (s->count >= s->cap) {
                s->cap *= 2;
                s->items = realloc(s->items, s->cap * sizeof(char *));
                if (!s->items) return -1;
        }
        s->items[s->count++] = strdup(item);
        return 1;
}

/* ── Dependency graph ───────────────────────────────────────────────
 *
 * Adjacency list: for each AUR package, store the list of AUR packages
 * it depends on. Used for topological sort after resolution.
 *
 * edges[i] = { pkg_name, dep_names[], dep_count }
 * An edge from A → B means "A depends on B" (B must be built first).
 */

typedef struct dep_edge {
        char *pkg_name;        /* the package that has deps */
        char **dep_names;      /* AUR packages it depends on */
        size_t dep_count;
        size_t dep_cap;
} dep_edge_t;

typedef struct dep_graph {
        dep_edge_t *edges;
        size_t count;
        size_t cap;
} dep_graph_t;

static dep_graph_t *dep_graph_new(void)
{
        dep_graph_t *g = calloc(1, sizeof(*g));
        g->cap = 64;
        g->edges = calloc(g->cap, sizeof(dep_edge_t));
        g->count = 0;
        return g;
}

static void dep_graph_free(dep_graph_t *g)
{
        if (!g) return;
        for (size_t i = 0; i < g->count; i++) {
                free(g->edges[i].pkg_name);
                for (size_t j = 0; j < g->edges[i].dep_count; j++)
                        free(g->edges[i].dep_names[j]);
                free(g->edges[i].dep_names);
        }
        free(g->edges);
        free(g);
}

/* Find or create an edge entry for a package */
static dep_edge_t *dep_graph_get_or_create(dep_graph_t *g, const char *pkg_name)
{
        for (size_t i = 0; i < g->count; i++) {
                if (strcmp(g->edges[i].pkg_name, pkg_name) == 0)
                        return &g->edges[i];
        }

        if (g->count >= g->cap) {
                g->cap *= 2;
                g->edges = realloc(g->edges, g->cap * sizeof(dep_edge_t));
        }

        dep_edge_t *e = &g->edges[g->count++];
        e->pkg_name = strdup(pkg_name);
        e->dep_cap = 16;
        e->dep_names = calloc(e->dep_cap, sizeof(char *));
        e->dep_count = 0;
        return e;
}

/* Add a dependency edge: pkg depends on dep (both must be AUR packages) */
static void dep_graph_add_edge(dep_graph_t *g, const char *pkg,
                               const char *dep)
{
        dep_edge_t *e = dep_graph_get_or_create(g, pkg);

        /* Check for duplicates */
        for (size_t i = 0; i < e->dep_count; i++) {
                if (strcmp(e->dep_names[i], dep) == 0)
                        return;
        }

        if (e->dep_count >= e->dep_cap) {
                e->dep_cap *= 2;
                e->dep_names = realloc(e->dep_names, e->dep_cap * sizeof(char *));
        }
        e->dep_names[e->dep_count++] = strdup(dep);

        /* Ensure the dep also has an entry in the graph */
        dep_graph_get_or_create(g, dep);
}

/* ── Topological sort (Kahn's algorithm) ────────────────────────────
 *
 * Returns a str_set_t with packages in build order (deps first).
 * If there's a cycle, returns NULL. */

static str_set_t *dep_graph_topo_sort(dep_graph_t *g)
{
        if (g->count == 0)
                return str_set_new();

        /* Build in-degree map */
        size_t *in_degree = calloc(g->count, sizeof(size_t));

        /* Map pkg_name → index */
        for (size_t i = 0; i < g->count; i++) {
                for (size_t j = 0; j < g->edges[i].dep_count; j++) {
                        /* Find the dep's index */
                        for (size_t k = 0; k < g->count; k++) {
                                if (strcmp(g->edges[k].pkg_name,
                                           g->edges[i].dep_names[j]) == 0) {
                                        in_degree[k]++;
                                        break;
                                }
                        }
                }
        }

        /* Find all nodes with in_degree 0 (no AUR deps) */
        str_set_t *queue = str_set_new();
        for (size_t i = 0; i < g->count; i++) {
                if (in_degree[i] == 0)
                        str_set_add(queue, g->edges[i].pkg_name);
        }

        str_set_t *sorted = str_set_new();

        while (queue->count > 0) {
                /* Take the first item from the queue */
                char *pkg = strdup(queue->items[0]);

                /* Remove it from queue (shift everything) */
                free(queue->items[0]);
                for (size_t i = 1; i < queue->count; i++)
                        queue->items[i - 1] = queue->items[i];
                queue->count--;

                /* Add to sorted output */
                str_set_add(sorted, pkg);

                /* Decrement in-degrees of nodes that depend on this package */
                for (size_t i = 0; i < g->count; i++) {
                        for (size_t j = 0; j < g->edges[i].dep_count; j++) {
                                if (strcmp(g->edges[i].dep_names[j], pkg) == 0) {
                                        in_degree[i]--;
                                        if (in_degree[i] == 0)
                                                str_set_add(queue, g->edges[i].pkg_name);
                                }
                        }
                }

                free(pkg);
        }

        str_set_free(queue);
        free(in_degree);

        /* If not all nodes were sorted, there's a cycle */
        if (sorted->count < g->count) {
                str_set_free(sorted);
                return NULL;
        }

        return sorted;
}

/* ── Action list helpers ──────────────────────────────────────────── */

/* Forward declaration - needed by dep_graph_topo_sort */
static void action_list_free(resolve_action_t *a);

static resolve_action_t *action_new(const char *name, const char *version,
                                     int is_aur, const char *pkgbase)
{
        resolve_action_t *a = calloc(1, sizeof(*a));
        if (!a) return NULL;
        a->name = strdup(name);
        a->version = version ? strdup(version) : NULL;
        a->is_aur = is_aur;
        a->pkgbase = pkgbase ? strdup(pkgbase) : NULL;
        return a;
}

static void action_append(resolve_action_t **list, resolve_action_t *item)
{
        if (!*list) {
                *list = item;
                return;
        }
        resolve_action_t *cur = *list;
        while (cur->next)
                cur = cur->next;
        cur->next = item;
}

static int action_list_contains(resolve_action_t *list, const char *name)
{
        while (list) {
                if (strcmp(list->name, name) == 0)
                        return 1;
                list = list->next;
        }
        return 0;
}

/* ── Strip version from dependency spec ─────────────────────────────
 *
 * Arch deps can be: "foo", "foo>=1.0", "foo=1.0-2", "foo<2.0"
 * We need just the package name. */

static char *strip_dep_version(const char *dep)
{
        if (!dep) return NULL;

        size_t len = strcspn(dep, "<>=");
        char *name = calloc(1, len + 1);
        memcpy(name, dep, len);
        name[len] = '\0';
        return name;
}

/* ── Helper: check if a dep name is an AUR package ────────────────── */

static int is_aur_package(aur_cache_t *cache, const char *name)
{
        aur_rpc_result_t rpc = aur_info(cache, name);
        int found = rpc.success && rpc.count > 0;
        aur_rpc_result_free(&rpc);
        return found;
}

/* ── Process a dep list, classify into AUR/repo ──────────────────── */

static void process_deps(aur_cache_t *cache,
                         char **deps, size_t deps_count,
                         str_set_t *seen, str_set_t *next_queue,
                         resolve_result_t *result,
                         dep_graph_t *graph, const char *parent_pkg,
                         int *changed)
{
        for (size_t d = 0; d < deps_count; d++) {
                char *dep_name = strip_dep_version(deps[d]);
                if (!dep_name || !*dep_name) { free(dep_name); continue; }

                if (!str_set_contains(seen, dep_name)) {
                        str_set_add(seen, dep_name);

                        if (is_aur_package(cache, dep_name)) {
                                str_set_add(next_queue, dep_name);
                                *changed = 1;
                                /* Record dep graph edge: parent depends on dep */
                                if (parent_pkg)
                                        dep_graph_add_edge(graph, parent_pkg, dep_name);
                        } else {
                                /* Not in AUR - assume repo package */
                                if (!action_list_contains(result->install, dep_name)) {
                                        resolve_action_t *a = action_new(dep_name, NULL, 0, NULL);
                                        action_append(&result->install, a);
                                }
                        }
                } else if (parent_pkg) {
                        /* Already seen, but still record the graph edge */
                        /* Check if dep_name is an AUR package we've already discovered */
                        if (action_list_contains(result->build, dep_name))
                                dep_graph_add_edge(graph, parent_pkg, dep_name);
                }

                free(dep_name);
        }
}

/* ── Resolve ─────────────────────────────────────────────────────── */

resolve_result_t *resolve_targets(aur_cache_t *cache,
                                  const char **targets, size_t count)
{
        resolve_result_t *result = calloc(1, sizeof(*result));
        if (!result) return NULL;

        if (!cache || !targets || count == 0)
                return result;

        /* Track what we've already processed to avoid cycles */
        str_set_t *seen = str_set_new();
        str_set_t *aur_queue = str_set_new();
        dep_graph_t *graph = dep_graph_new();

        /* First pass: classify targets */
        for (size_t i = 0; i < count; i++) {
                str_set_add(seen, targets[i]);
                str_set_add(aur_queue, targets[i]);
                /* Ensure target has a graph entry */
                dep_graph_get_or_create(graph, targets[i]);
        }

        /* Iteratively resolve AUR deps until no new ones are found */
        int changed = 1;
        int max_iterations = 50;
        int iteration = 0;

        while (changed && iteration < max_iterations) {
                changed = 0;
                iteration++;

                if (aur_queue->count == 0)
                        break;

                /* Batch-query AUR for all packages in the queue */
                aur_rpc_result_t rpc = aur_info_batch(cache,
                                        (const char **)aur_queue->items,
                                        aur_queue->count);

                str_set_t *next_queue = str_set_new();

                if (rpc.success) {
                        aur_pkg_t *pkg = rpc.packages;
                        while (pkg) {
                                /* Add this package to the build list */
                                if (!action_list_contains(result->build, pkg->name)) {
                                        resolve_action_t *a = action_new(
                                                pkg->name, pkg->version,
                                                1, pkg->pkgbase);
                                        action_append(&result->build, a);
                                }

                                /* Process depends, makedepends, checkdepends */
                                process_deps(cache, pkg->depends,
                                             pkg->depends_count,
                                             seen, next_queue, result,
                                             graph, pkg->name, &changed);
                                process_deps(cache, pkg->makedepends,
                                             pkg->makedepends_count,
                                             seen, next_queue, result,
                                             graph, pkg->name, &changed);
                                process_deps(cache, pkg->checkdepends,
                                             pkg->checkdepends_count,
                                             seen, next_queue, result,
                                             graph, pkg->name, &changed);

                                pkg = pkg->next;
                        }
                }

                aur_rpc_result_free(&rpc);

                str_set_free(aur_queue);
                aur_queue = next_queue;
        }

        /* Anything left in the queue that wasn't resolved is missing */
        for (size_t i = 0; i < aur_queue->count; i++) {
                resolve_action_t *a = action_new(aur_queue->items[i], NULL, 0, NULL);
                action_append(&result->missing, a);
        }

        str_set_free(aur_queue);
        str_set_free(seen);

        /* ── Topological sort the build list ────────────────────────── */
        str_set_t *sorted = dep_graph_topo_sort(graph);

        if (sorted && sorted->count > 0) {
                /* Rebuild the build list in topo-sorted order */
                resolve_action_t *old_build = result->build;
                result->build = NULL;

                for (size_t i = 0; i < sorted->count; i++) {
                        /* Find the action in the old list */
                        resolve_action_t *a = old_build;
                        while (a) {
                                if (strcmp(a->name, sorted->items[i]) == 0) {
                                        resolve_action_t *copy = action_new(
                                                a->name, a->version,
                                                a->is_aur, a->pkgbase);
                                        action_append(&result->build, copy);
                                        break;
                                }
                                a = a->next;
                        }
                }

                /* Free the old unsorted list */
                action_list_free(old_build);
        } else if (!sorted && graph->count > 0) {
                /* Cycle detected - keep the unsorted list but warn */
                fprintf(stderr, "  warning: dependency cycle detected, "
                                "build order may be incorrect\n");
        }

        if (sorted) str_set_free(sorted);
        dep_graph_free(graph);

        return result;
}

/* ── Free ─────────────────────────────────────────────────────────── */

static void action_list_free(resolve_action_t *a)
{
        while (a) {
                resolve_action_t *next = a->next;
                free(a->name);
                free(a->version);
                free(a->pkgbase);
                free(a);
                a = next;
        }
}

void resolve_result_free(resolve_result_t *r)
{
        if (!r) return;
        action_list_free(r->install);
        action_list_free(r->build);
        action_list_free(r->missing);
        free(r);
}

/* ── Pre-build PGP key check ────────────────────────────────────────
 *
 * Reads validpgpkeys from .SRCINFO in clone_dir, finds any missing
 * from the local gpg keyring, and prompts the user to import via
 * gpg --recv-keys. Returns 0 if all keys are present (or none are
 * required), -1 on user decline or error.
 *
 * Called from aur_build() before the build step. Modeled on paru's
 * src/keys.rs.
 */
int aur_resolve_pgp_check(const char *clone_dir)
{
        if (!clone_dir) return -1;

        char **keys = pgp_read_valid_keys(clone_dir);
        if (!keys) return 0;  /* no validpgpkeys declared */

        char **missing = pgp_find_missing(keys);
        pgp_free_list(keys);

        if (!missing) return 0;  /* all keys already in keyring */

        int rc = pgp_import_missing(missing);
        pgp_free_list(missing);

        return rc;
}
