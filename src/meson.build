watchdog_headers = include_directories('.')

if cpp.has_header('CLI/CLI.hpp')
    CLI11_dep = declare_dependency()
else
    CLI11_dep = dependency('CLI11')
endif

watchdog_deps = [
  CLI11_dep,
  dependency('phosphor-dbus-interfaces'),
  dependency('phosphor-logging'),
  dependency('sdbusplus'),
  dependency('sdeventplus'),
  dependency('stdplus'),
]

if get_option('tests').enabled()
  pdkhook_deps = [dependency('libpdkhook', fallback: ['libpdkhook', 'libpdkhook_dep'])]
else
  pdkhook_deps = [declare_dependency(link_args: '-lpdkhook')]
endif

watchdog_lib = static_library(
  'watchdog',
  'watchdog.cpp',
  implicit_include_directories: false,
  include_directories: watchdog_headers,
  dependencies: [watchdog_deps, pdkhook_deps],)

watchdog_dep = declare_dependency(
  dependencies: watchdog_deps,
  include_directories: watchdog_headers,
  link_with: watchdog_lib)

executable(
  'phosphor-watchdog',
  'mainapp.cpp',
  implicit_include_directories: false,
  dependencies: watchdog_dep,
  install: true,
  install_dir: get_option('bindir'))
