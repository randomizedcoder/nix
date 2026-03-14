project(
  'nix-metrics',
  'cpp',
  version : files('.version'),
  default_options : [
    'cpp_std=c++23',
    'warning_level=1',
    'errorlogs=true',
  ],
  meson_version : '>= 1.1',
  license : 'LGPL-2.1-or-later',
)

cxx = meson.get_compiler('cpp')

subdir('nix-meson-build-support/deps-lists')

configdata_pub = configuration_data()
configdata_priv = configuration_data()

deps_private_maybe_subproject = []
deps_public_maybe_subproject = [
  dependency('nix-util'),
]
subdir('nix-meson-build-support/subprojects')

sources = files(
  'build-metrics.cc',
)

include_dirs = [include_directories('.', 'include')]

headers = files(
  'include/nix/metrics/build-metrics.hh',
)

this_library = library(
  'nixmetrics',
  sources,
  dependencies : deps_public + deps_private,
  include_directories : include_dirs,
  install : true,
)

install_headers(headers, subdir : 'nix/metrics', preserve_path : false)

libraries_private = []

import('pkgconfig').generate(
  this_library,
  name : 'Nix',
  subdirs : ['nix'],
  description : 'Nix Build Telemetry Metrics',
  libraries_private : libraries_private,
)

meson.override_dependency('nix-metrics', declare_dependency(
  include_directories : include_dirs,
  link_with : this_library,
))
