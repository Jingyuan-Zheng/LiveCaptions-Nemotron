livecaptions_sources = [
  'main.c',
  'livecaptions-window.c',
  'livecaptions-welcome.c',
  'livecaptions-settings.c',
  'livecaptions-application.c',
  'audiocap.c',
  'audiocap-pa.c',
  'audiocap-pw.c',
  'asrproc.c',
  'caption-result.c',
  'line-gen.c',
  'profanity-filter.c',
  'window-helper.c',
  'history.c',
  'livecaptions-history-window.c',
  'dbus-interface.c',
  'translation.c'
]

cc = meson.get_compiler('c')
nemo_prefix = join_paths(get_option('prefix'), 'nemo-speech')
nemo_include = include_directories(join_paths(nemo_prefix, 'include'))
nemo_asr = cc.find_library('nemo_speech_asr_c', dirs: [join_paths(nemo_prefix, 'lib')], required: true)

livecaptions_deps = [
  dependency('libadwaita-1', version: '>= 1.0'),
  #dependency('libpipewire-0.3', version: '>=0.3.41'),
  dependency('libpulse'),
  dependency('x11'),
  dependency('libsoup-3.0'),
  dependency('json-glib-1.0'),
  nemo_asr,

  cc.find_library('m', required: false),
]

gnome = import('gnome')

livecaptions_sources += gnome.compile_resources('livecaptions-resources',
  'livecaptions.gresource.xml',
  c_name: 'livecaptions'
)

executable('livecaptions', livecaptions_sources,
  dependencies: livecaptions_deps,
  include_directories: nemo_include,
  install: true,
)
