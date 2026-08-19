project(
  'daqiri_bf3_controller',
  'c',
  version: '1.0.0',
  default_options: ['buildtype=release', 'c_std=c11', 'warning_level=3'],
  meson_version: '>=0.61.2',
)

add_project_arguments('-D_GNU_SOURCE', '-D DOCA_ALLOW_EXPERIMENTAL_API', language: 'c')

doca_dependencies = [
  dependency('doca-common'),
  dependency('doca-devemu'),
  dependency('doca-dma'),
]

executable(
  'daqiri_bf3_controller',
  'daqiri_bf3_controller.c',
  dependencies: doca_dependencies,
  include_directories: include_directories('..', '../../../include'),
  install: false,
)
