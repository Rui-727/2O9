# lib209 — libalpm, modified in-tree
#
# This is pacman v6.0.0's libalpm, copied into 2O9 and modified directly.
# Everything not needed for the library (CLI, scripts, docs, tests, translations)
# has been removed. See MODIFICATIONS.md for what we changed.
#
# This builds a static lib209.a that gets linked into the 209 binary.

project('lib209',
        'c',
        version : '6.0.0',
        license : 'GPLv2+',
        default_options : [
          'c_std=gnu99',
        ],
        meson_version : '>= 0.51')

cc = meson.get_compiler('c')

# ── Dependencies ──────────────────────────────────────────────────
libarchive = dependency('libarchive', version : '>=3.0.0', required : true)
libcurl = dependency('libcurl', version : '>=7.55.0', required : false)
gpgme = dependency('gpgme', version : '>=1.3.0', required : false)
openssl = dependency('libcrypto', required : true)

# ── Feature detection ─────────────────────────────────────────────
conf = configuration_data()
conf.set('_GNU_SOURCE', true)
conf.set_quoted('PACKAGE', 'lib209')
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('LOCALEDIR', '/usr/share/locale')
conf.set_quoted('SCRIPTLET_SHELL', '/bin/sh')
conf.set_quoted('LDCONFIG', 'ldconfig')
conf.set_quoted('LIB_VERSION', '13.0.0')
conf.set_quoted('SYSHOOKDIR', '/usr/share/libalpm/hooks/')
conf.set_quoted('CONFFILE', '/etc/2O9/2O9.nix')
conf.set_quoted('DBPATH', '/var/lib/2O9/')
conf.set_quoted('GPGDIR', '/etc/pacman.d/gnupg/')
conf.set_quoted('LOGFILE', '/var/log/2O9.log')
conf.set_quoted('CACHEDIR', '/var/cache/2O9/pkg/')
conf.set_quoted('HOOKDIR', '/etc/pacman.d/hooks/')
conf.set_quoted('ROOTDIR', '/')

conf.set('HAVE_LIBCURL', libcurl.found())
conf.set('HAVE_LIBGPGME', gpgme.found())
conf.set10('HAVE_LIBSSL', true)

foreach header : ['mntent.h', 'sys/mnttab.h', 'sys/mount.h', 'sys/param.h',
                  'sys/statvfs.h', 'sys/types.h', 'sys/ucred.h', 'termios.h']
  if cc.has_header(header)
    conf.set('HAVE_' + header.underscorify().to_upper(), true)
  endif
endforeach

foreach sym : ['getmntent', 'getmntinfo', 'strndup', 'strnlen', 'strsep',
               'swprintf', 'tcflush']
  if cc.has_function(sym, args : '-D_GNU_SOURCE')
    conf.set10('HAVE_' + sym.to_upper(), true)
  endif
endforeach

foreach member : [
  ['struct stat', 'st_blksize', '#include <sys/stat.h>'],
  ['struct statvfs', 'f_flag', '#include <sys/statvfs.h>'],
  ['struct statfs', 'f_flags', '#include <sys/param.h>\n#include <sys/mount.h>'],
]
  if cc.has_member(member[0], member[1], prefix : member[2])
    conf.set('HAVE_' + '_'.join([member[0], member[1]]).underscorify().to_upper(), true)
  endif
endforeach

if conf.has('HAVE_STRUCT_STATVFS_F_FLAG')
  conf.set('FSSTATSTYPE', 'struct statvfs')
elif conf.has('HAVE_STRUCT_STATFS_F_FLAGS')
  conf.set('FSSTATSTYPE', 'struct statfs')
endif

config_h = configure_file(
  output : 'config.h',
  configuration : conf)
add_project_arguments('-include', 'config.h', language : 'c')

# ── Sources ───────────────────────────────────────────────────────
# Shared utils (ini.c, util-common.c) — compiled into libalpm
libcommon_sources = files(
  'src/common/ini.c',
  'src/common/util-common.c',
)

# libalpm proper
libalpm_sources = files(
  'lib/libalpm/add.c',
  'lib/libalpm/alpm.c',
  'lib/libalpm/alpm_list.c',
  'lib/libalpm/backup.c',
  'lib/libalpm/base64.c',
  'lib/libalpm/be_local.c',
  'lib/libalpm/be_package.c',
  'lib/libalpm/be_sync.c',
  'lib/libalpm/conflict.c',
  'lib/libalpm/db.c',
  'lib/libalpm/deps.c',
  'lib/libalpm/diskspace.c',
  'lib/libalpm/dload.c',
  'lib/libalpm/error.c',
  'lib/libalpm/filelist.c',
  'lib/libalpm/graph.c',
  'lib/libalpm/group.c',
  'lib/libalpm/handle.c',
  'lib/libalpm/hook.c',
  'lib/libalpm/log.c',
  'lib/libalpm/package.c',
  'lib/libalpm/pkghash.c',
  'lib/libalpm/rawstr.c',
  'lib/libalpm/remove.c',
  'lib/libalpm/signing.c',
  'lib/libalpm/sync.c',
  'lib/libalpm/trans.c',
  'lib/libalpm/util.c',
  'lib/libalpm/version.c',
)

includes = include_directories('src/common', 'lib/libalpm')

# ── Build lib209.a ────────────────────────────────────────────────
libcommon = static_library(
  'common',
  libcommon_sources,
  include_directories : includes,
  gnu_symbol_visibility : 'hidden',
  install : false)

alpm_deps = [openssl, libarchive, libcurl, gpgme]

libalpm_a = static_library(
  'alpm_objlib',
  libalpm_sources,
  objects : libcommon.extract_all_objects(),
  include_directories : includes,
  gnu_symbol_visibility : 'hidden',
  dependencies : alpm_deps)

# lib209.a — our static build of libalpm, linked into 209
lib209_a = static_library(
  '209',
  libalpm_sources,
  objects : libcommon.extract_all_objects(),
  include_directories : includes,
  gnu_symbol_visibility : 'hidden',
  dependencies : alpm_deps,
  install : false)

# Expose as dependency for the 209 binary
lib209_dep = declare_dependency(
  link_with : lib209_a,
  include_directories : includes,
  dependencies : alpm_deps,
)
