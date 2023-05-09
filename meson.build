project('wakefield', 'c',
          version: '0.1.0',
    meson_version: '>= 0.50.0',
  default_options: [ 'warning_level=2',
                     'c_std=gnu11',
                   ],
)

cc = meson.get_compiler('c')

add_project_arguments(
  '-Wno-unused-parameter',
  language: 'c',
)

required_headers = [
  'linux/input-event-codes.h',
]

foreach header: required_headers
  cc.check_header(header,
    required: true,
  )
endforeach

config_h = configuration_data()
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', 'wakefield')
configure_file(
  output: 'config.h',
  configuration: config_h,
)
add_project_arguments([
  '-I' + meson.build_root(),
], language: 'c')

subdir('protocol')
subdir('src')
subdir('tests')
