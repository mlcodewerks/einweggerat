dsp_inc = include_directories('include')
dsp_sources = files(
  'audio/dsp_filters/chorus.c', 'audio/dsp_filters/crystalizer.c',
  'audio/dsp_filters/echo.c', 'audio/dsp_filters/eq.c', 'audio/dsp_filters/iir.c',
  'audio/dsp_filters/panning.c', 'audio/dsp_filters/phaser.c', 'audio/dsp_filters/reverb.c',
  'audio/dsp_filters/tremolo.c', 'audio/dsp_filters/vibrato.c', 'audio/dsp_filters/wahwah.c',
  'audio/dsp_filters/bitcrusher.c', 'audio/dsp_filters/overdrive.c',
  'audio/dsp_filters/overdrive_guitar.c',
  'audio/dsp_filters/reverb_early.c', 'audio/dsp_filters/wsolapitchtempo.c')
dsp_math = meson.get_compiler('c').find_library('m', required: false)
dsp_lib = static_library('libretro-dsp', dsp_sources, include_directories: dsp_inc,
  c_args: ['-DHAVE_FILTERS_BUILTIN'], override_options: ['c_std=gnu11'], dependencies: dsp_math)
dsp_dep = declare_dependency(link_with: dsp_lib, include_directories: dsp_inc, dependencies: dsp_math)
