# PKGBUILD for 2O9 — self-hosting packaging
#
# Build 2O9 with makepkg. This PKGBUILD assumes the source is on GitHub
# at Rui-727/2O9. Adjust the source URL if you fork it.
#
# Build deps: libarchive-dev, openssl-dev, libgpgme-dev, libcurl-dev,
# libxml2-dev, libacl1-dev. These are listed in depends/makedepends below.
#
# To build:
#   makepkg -f
#   sudo pacman -U 2O9-<ver>-1-x86_64.pkg.tar.zst
#
# Or install to a custom prefix:
#   makepkg -f PREFIX=/usr/local

pkgname=2O9
pkgver=0.0.1
pkgrel=1
pkgdesc="Unified package manager for Arch Linux that puts files in /nix/store/"
arch=('x86_64')
url="https://github.com/Rui-727/2O9"
license=('GPL2')
depends=('libarchive' 'openssl' 'libgpgme' 'curl' 'libxml2' 'acl')
makedepends=('gcc' 'make' 'pkgconf')
source=("$pkgname::git+https://github.com/Rui-727/2O9.git")
sha256sums=('SKIP')

build() {
    cd "$pkgname"
    make PREFIX=/usr
}

package() {
    cd "$pkgname"
    make DESTDIR="$pkgdir/" PREFIX=/usr install

    # Install default config directory
    install -d "$pkgdir/etc/2O9"

    # Install man page (when it exists)
    # install -Dm644 docs/MANPAGE.md "$pkgdir/usr/share/man/man1/209.1"

    # Install shell completion (when it exists)
    # install -Dm644 scripts/completion.bash "$pkgdir/usr/share/bash-completion/completions/209"
}

# vim:set ts=4 sw=4 et:
