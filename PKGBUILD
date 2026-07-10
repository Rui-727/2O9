# PKGBUILD for 2O9 - self-hosting packaging
#
# 2O9 is a unified package manager for Arch Linux that puts files in
# /nix/store/ instead of /usr/, combines libalpm (repo), AUR, and
# binary-cache substitution, and is configured declaratively via
# 2O9.nix (a Nix expression evaluated by 2O9's own C Nix evaluator).
#
# To build:
#   makepkg -f
#   sudo pacman -U 2O9-<ver>-1-x86_64.pkg.tar.zst
#
# Or with an AUR helper (if submitted to AUR):
#   paru -S 2O9
#
# Maintainer: Rui-727 <a192.47.72x@gmail.com>

pkgname=2O9
pkgver=0.1.0
pkgrel=1
pkgdesc="Arch Linux package manager combining libalpm, AUR, and /nix/store/ with declarative Nix config"
arch=('x86_64')
url="https://github.com/Rui-727/2O9"
license=('GPL-2.0-only')

# Direct shared-library deps - every -lfoo in the Makefile's LIB2O9_LIBS
# line gets a package here so the installed `209` binary can dlopen its
# libs at runtime. Transitive deps (e.g. libgpgme -> libgpg-error) are
# listed explicitly because the Makefile links them directly rather than
# relying on DT_NEEDED to pull them in.
depends=(
    'curl'           # -lcurl        AUR RPC + libalpm downloads
    'libarchive'     # -larchive     .pkg.tar.zst extraction (add.c, be_package.c)
    'libgpgme'       # -lgpgme       PGP signature verification in libalpm signing.c
    'libassuan'      # -lassuan      gpgme IPC transport
    'libgpg-error'   # -lgpg-error   gpgme runtime
    'openssl'        # -lcrypto      SHA-256, sig verify, Ed25519 fallback
    'sqlite'         # -lsqlite3     store DB / refs graph for GC (Phase 2)
    'libxml2'        # -lxml2        libalpm metadata parsing
    'libseccomp'     # -lseccomp     debag syscall filter
    'nettle'         # -lnettle -lhogweed  libarchive crypto backend
    'gmp'            # -lgmp         nettle/hogweed big-num dep
    'acl'            # -lacl         libarchive ACL support
    'zlib'           # -lz           libarchive compression
    'xz'             # -llzma        libarchive compression
    'bzip2'          # -lbz2         libarchive compression
    'lz4'            # -llz4         libarchive compression
    'zstd'           # -lzstd        libarchive compression (.pkg.tar.zst)
)

makedepends=('git' 'gcc' 'make' 'pkgconf')

# Optional features. libsodium and libcapstone are auto-detected by the
# Makefile via pkg-config / header probe; devtools and gpg are runtime
# tools that 209 shells out to.
optdepends=(
    'libcapstone: disassembly in debag static-db (auto-detected at build)'
    'libsodium: Ed25519 signing for binary caches (falls back to OpenSSL)'
    'devtools: chroot AUR builds (mkarchroot, arch-nspawn, makechrootpkg)'
    'gpg: PGP key import for AUR packages with validpgpkeys'
)

provides=('2O9')
conflicts=('2O9')

source=("git+https://github.com/Rui-727/2O9.git")
sha256sums=('SKIP')

build() {
    cd "$srcdir/2O9"
    make PREFIX=/usr
}

check() {
    cd "$srcdir/2O9"
    # `make test` builds the unit tests + runs the test/*.sh integration
    # suite. test-aur-rpc needs network and is allowed to fail inside the
    # Makefile recipe.
    make test
}

package() {
    cd "$srcdir/2O9"
    make DESTDIR="$pkgdir" PREFIX=/usr install

    # The Makefile installs:
    #   /usr/bin/209
    #   /usr/share/bash-completion/completions/209

    # Install profile script so ~/.local/bin is on PATH for the per-user
    # profile that `209 apply` populates with symlinks.
    install -Dm644 /dev/stdin "$pkgdir/etc/profile.d/2O9.sh" <<'EOF'
# Add 2O9 user bin to PATH (populated by `209 apply` / symlink farm)
export PATH="$HOME/.local/bin:$PATH"
EOF
}

# vim:set ts=4 sw=4 et:
