project(
  'phosphor-watchdog',
  'cpp',
  version: '0.1',
  meson_version: '>=1.1.1',
  default_options: [
    'warning_level=3',
    'cpp_std=c++23',
  ])

cpp = meson.get_compiler('cpp')

if get_option('tests').enabled()
    add_project_arguments('-DUNIT_TESTING', language: 'cpp')
    sdbusplus = subproject('sdbusplus').get_variable('sdbusplus_dep')
    meson.override_dependency('sdbusplus', sdbusplus)
    phosphor_dbus_interfaces_dep = subproject('phosphor-dbus-interfaces').get_variable('phosphor_dbus_interfaces_dep')
    phosphor_logging_dep = subproject('phosphor-logging').get_variable('phosphor_logging_dep')
    meson.override_dependency('phosphor-logging', phosphor_logging_dep)
endif

subdir('src')

if get_option('tests').allowed()
  subdir('test')
endif
