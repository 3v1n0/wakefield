api_version = '0.1'

top_inc = include_directories('.')

wakefield_sources = [
  'wakefield-private.h',
  'wakefield-compositor.c',
  'wakefield-surface.c',
  'wakefield-data-device.c'
]

protocol_sources = [
  xdg_shell_client_protocol_h,
  xdg_shell_server_protocol_h,
  xdg_shell_protocol_c
]

wakefield_headers = [
  'wakefield-compositor.h',
]

wakefield_deps = [
  dependency('glib-2.0', version: glib_req),
  dependency('gtk+-3.0'),
  dependency('wayland-server'),
  dependency('wayland-client'),
  dependency('xkbcommon'),
# FIXME: These two are only needed if gdk targets x11
  dependency('xkbcommon-x11'),
  dependency('x11-xcb')
]

wakefield_lib = shared_library('wakefield-' + api_version,
  wakefield_sources + protocol_sources,
  dependencies: wakefield_deps,
  install: true,
)

install_headers(wakefield_headers, subdir: 'wakefield')

pkg = import('pkgconfig')

pkg.generate(
  description: 'A Gtk Wayland compositor library',
    libraries: wakefield_lib,
         name: 'wakefield',
     filebase: 'wakefield-' + api_version,
      version: meson.project_version(),
      subdirs: 'wakefield',
     requires: 'glib-2.0',
  install_dir: join_paths(get_option('libdir'), 'pkgconfig')
)
