/* nar.h - NAR (Nix Archive) serialisation + content-addressed store paths
 *
 * Phase 2: 2O9 switches from /nix/store/<name>-<version>/ (collision-prone,
 * tamper-undetectable) to /nix/store/<base32-hash>-<name>-<version>/ where
 * the hash is computed from the canonical NAR serialisation of the
 * extracted tree.
 *
 * The NAR format is documented in nix/src/libstore/remote-store.cc
 * (dumpPath / restorePath). It walks a directory tree in sorted order and
 * emits a canonical byte stream:
 *
 *   nix-archive-1 <newline>
 *   (type:regular <newline>
 *     [executable <newline>]
 *     contents:<8 bytes big-endian length><file bytes>) |
 *   (type:symlink <newline>
 *     target:<target string without newline>) |
 *   (type:directory <newline>
 *     (entry <newline>
 *       name:<name without newline> <newline>
 *       <recursion>)*
 *   )
 *
 * The store path is then built Nix-style:
 *   sha256("output:out:sha256:<nar-hash-hex>:/nix/store:<name>-<version>")
 *   truncated to 20 bytes, Nix-base32-encoded (32 chars).
 *
 * Result: /nix/store/<base32>-<name>-<version>/
 */
#ifndef TWO9_NAR_H
#define TWO9_NAR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Compute the NAR hash of a directory tree.
 *   path       - root of the extracted tree (e.g. /nix/store/.tmp/foo-1.0.123)
 *   hash_out   - caller-allocated 65 bytes; receives 64-char hex string + NUL
 *   size_out   - if non-NULL, receives the total NAR serialised byte count
 * Returns 0 on success, -1 on error (errno set). */
int nar_hash_directory(const char *path, char *hash_out, size_t *size_out);

/* Serialise a directory tree to NAR format, writing to a FILE*.
 * Used by nar_hash_directory internally; also useful for Phase 3 binary
 * cache export. Returns 0 on success, -1 on error. */
int nar_dump(const char *path, FILE *out);

/* Build the final 2O9 store path: /nix/store/<base32>-<name>-<version>
 *   nar_hash_hex - 64-char lowercase hex NAR hash
 *   name         - package name (e.g. "neovim")
 *   version      - package version (e.g. "0.9.5")
 * Returns a malloc'd string (caller frees) or NULL on error. */
char *compute_store_path(const char *nar_hash_hex, const char *name,
                         const char *version);

#endif /* TWO9_NAR_H */
