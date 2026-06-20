# store adapter build

store_sources = files(
  'store.c',
  'symlinks.c',
)

# Phase 0 spike — standalone binary, not linked into 209
spike_sources = files('spike-nix-add.c')

executable(
  'spike-nix-add',
  spike_sources,
  install : false,
)
