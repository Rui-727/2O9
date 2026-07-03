/* optimise.h - hardlink-based store dedup
 *
 * Phase 2: 2O9's store can grow large because many packages ship the
 * same file (e.g. /usr/share/licenses/common/GPL-3, COPYING, locale
 * files for shared deps). Hardlink dedup identifies regular files with
 * identical SHA-256 contents and replaces them with hardlinks into a
 * shared /nix/store/.links/<sha256> pool.
 *
 * After optimise, two store paths sharing a 5MB locale file consume
 * only 5MB on disk instead of 10MB. The hardlinks are transparent -
 * ls/stat see the file as before, but the inode is shared.
 *
 * Algorithm (mirrors Nix's nix-store --optimise):
 *   1. Create /nix/store/.links/ (mode 1777 like /tmp).
 *   2. Walk every regular file under /nix/store/ (skip .links, .tmp,
 *      any path starting with '.').
 *   3. For each file with st_nlink == 1: SHA-256 its contents, compute
 *      /nix/store/.links/<hex>. If target doesn't exist, link(original,
 *      target). Then unlink(original) + link(target, original). Now
 *      both point to the same inode. If target exists, just unlink +
 *      link (the original now shares the existing inode).
 *   4. Sum st.st_size for each file that was deduplicated.
 */
#ifndef TWO9_STORE_OPTIMISE_H
#define TWO9_STORE_OPTIMISE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Walk <store_root>, hardlink identical regular files into
 * <store_root>/.links/<sha256>. Returns total bytes saved (sum of
 * st.st_size for each deduplicated file). Returns -1 on hard error
 * (store_root missing, .links mkdir failed). */
int64_t store_optimise(const char *store_root);

/* Optimise a single store path (one package). Useful for testing and
 * for post-install dedup of just-installed packages. Returns bytes
 * saved or -1 on error. */
int64_t store_optimise_path(const char *store_root, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* TWO9_STORE_OPTIMISE_H */
