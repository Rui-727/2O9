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
libarchive_dep = dependency('libarchive', version : '>=3.0.0', required : false)
libcurl_dep = dependency('libcurl', version : '>=7.55.0', required : true)
libzstd_dep = dependency('libzstd', version : '>=1.4.0', required : false)

if libarchive_dep.found()
  message('libarchive: found')
else
  message('libarchive: NOT found — install libarchive-dev to build lib209')
endif

# ── lib209 (vendored libalpm) ─────────────────────────────────────
if get_option('build-lib209')
  subdir('subprojects/pacman')
endif

# ── Internal modules ──────────────────────────────────────────────
subdir('src/store')
subdir('src/declarative')
subdir('src/aur')

# ── 209 binary ────────────────────────────────────────────────────
cli_sources = files('src/cli/main.c')

two9_sources = cli_sources + store_sources + declarative_sources + aur_sources

two9_includes = include_directories(
  'src',
  'src/store',
  'src/declarative',
  'src/aur',
)

executable(
  '209',
  two9_sources,
  include_directories : two9_includes,
  dependencies : [libcurl_dep],
  install : true,
)

# ── Test binaries ─────────────────────────────────────────────────
aur_lib_files = files('src/aur/aur_rpc.c', 'src/aur/cJSON.c')

executable(
  'test-aur-rpc',
  files('src/aur/test_aur_rpc.c') + aur_lib_files,
  include_directories : include_directories('src/aur'),
  dependencies : [libcurl_dep],
  install : false,
)
