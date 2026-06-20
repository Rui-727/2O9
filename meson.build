project('2O9',
        'c',
        version : '0.0.1',
        license : 'GPL-2.0-only',
        default_options : [
          'c_std=gnu11',
          'prefix=/usr',
          'sysconfdir=/etc',
          'localstatedir=/var',
        ],
        meson_version : '>= 0.51')

cc = meson.get_compiler('c')

# ── Version ────────────────────────────────────────────────────────
if get_option('use-git-version')
  git = find_program('git', required : false)
  if git.found()
    PACKAGE_VERSION = run_command(git, 'describe', '--abbrev=4', '--dirty',
                                  check : false).stdout().strip()
    if PACKAGE_VERSION == ''
      PACKAGE_VERSION = meson.project_version()
    endif
  else
    PACKAGE_VERSION = meson.project_version()
  endif
else
  PACKAGE_VERSION = meson.project_version()
endif

message('2O9 version: ' + PACKAGE_VERSION)

# ── Configuration ──────────────────────────────────────────────────
conf = configuration_data()
conf.set('_GNU_SOURCE', true)
conf.set_quoted('PACKAGE', '2O9')
conf.set_quoted('PACKAGE_VERSION', PACKAGE_VERSION)
conf.set_quoted('SYSCONFDIR', join_paths(get_option('prefix'), get_option('sysconfdir')))
conf.set_quoted('LOCALSTATEDIR', join_paths(get_option('prefix'), get_option('localstatedir')))
conf.set_quoted('DBPATH', '/var/lib/2O9/')
conf.set_quoted('CACHEDIR', '/var/cache/2O9/pkg/')
conf.set_quoted('CONFIG_PATH', '/etc/2O9/2O9.nix')
conf.set_quoted('STORE_ROOT', '/nix/store')
conf.set_quoted('PROFILE_DIR', '/nix/var/nix/profiles/per-user/2O9-system')
conf.set_quoted('USER_PROFILE_DIR', '/.local/state/2O9/profile')
conf.set_quoted('BIN_DIR', '/.local/bin')
conf.set_quoted('LIB_DIR', '/.local/lib')

config_h = configure_file(
  output : 'config.h',
  configuration : conf)

# ── External dependencies ─────────────────────────────────────────
# libarchive: system package. pacman -S libarchive / apt install libarchive-dev
# libzstd:    system package. needed for .pkg.tar.zst support
libarchive_dep = dependency('libarchive', version : '>=3.0.0', required : false)
libzstd_dep = dependency('libzstd', version : '>=1.4.0', required : false)

if libarchive_dep.found()
  message('libarchive: found')
else
  message('libarchive: NOT found — install libarchive-dev to build lib209')
endif

# ── lib209 (vendored libalpm) ─────────────────────────────────────
# Build libalpm from the vendored pacman source as lib209.a.
# Requires libarchive-dev.
if get_option('build-lib209') and libarchive_dep.found()
  subdir('subprojects/pacman')
endif

# ── Internal libraries ────────────────────────────────────────────

# Store adapter sources (not a separate lib, compiled into 209)
subdir('src/store')

# Declarative engine sources (generation DB)
subdir('src/declarative')

# ── 209 binary ────────────────────────────────────────────────────
cli_sources = files('src/cli/main.c')

# All internal sources linked into 209
two9_sources = cli_sources + store_sources + declarative_sources

two9_includes = include_directories(
  'src',
  'src/store',
  'src/declarative',
)

executable(
  '209',
  two9_sources,
  include_directories : two9_includes,
  install : true,
)
