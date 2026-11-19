project('einweggerat', 'c', 'cpp', version: '0.1.0', license: 'Unlicense', default_options: ['cpp_std=c++17', 'c_std=gnu11', 'buildtype=release', 'warning_level=2'])
if host_machine.system() != 'windows' or host_machine.cpu_family() != 'x86_64'
  error('einweggerat supports Windows x64 only.')
endif
cpp = meson.get_compiler('cpp')
add_project_arguments('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00', '-DVK_USE_PLATFORM_WIN32_KHR', language: ['c', 'cpp'])
if cpp.get_id() == 'gcc'
  add_project_link_arguments('-static', '-static-libgcc', '-static-libstdc++', language: 'cpp')
endif
inc = include_directories('src', 'third_party/imgui', 'third_party/ImGuiColorTextEdit', 'third_party/libretro', 'third_party/graphics', 'third_party/librashader', 'third_party/fonts')
vulkan_headers = declare_dependency(include_directories: include_directories('third_party/vulkan-headers/include'))
deps = [dependency('threads'), vulkan_headers]
foreach lib : ['dinput8', 'dxguid', 'xinput1_4', 'ole32', 'uuid', 'user32', 'gdi32', 'shell32', 'dwmapi', 'imm32', 'avrt']
  deps += cpp.find_library(lib)
endforeach
if cpp.get_id() == 'gcc'
  deps += cpp.find_library('winpthread', static: true)
endif
subdir('third_party/spessasynth')
spessa_inc = include_directories('third_party/spessasynth/spessasynth_core/include', 'third_party/spessasynth/spessasynth_core/src',
  'third_party/spessasynth/spessasynth_core/include/spessasynth/sflist', 'third_party/spessasynth/spessasynth_core/extern/json')
math = meson.get_compiler('c').find_library('m', required: false)
spessa_lib = static_library('spessasynth', spessa_sources, include_directories: spessa_inc, dependencies: [dependency('threads'), math])
deps += declare_dependency(link_with: spessa_lib, include_directories: spessa_inc)
subdir('third_party/libretro-dsp')
deps += dsp_dep
platform = static_library('platform', ['src/common.cpp', 'src/command_line.cpp', 'src/log.cpp', 'src/log_window.cpp', 'src/core_catalog.cpp', 'src/core_options_window.cpp', 'src/rom_browser.cpp', 'src/ui_fonts.cpp', 'src/video.cpp', 'src/out_vulkan.cpp', 'src/audio.cpp', 'src/input.cpp', 'src/input_bindings.cpp', 'src/input_model.cpp', 'src/input_window.cpp', 'src/vfs.cpp', 'src/midi.cpp',
  'src/shader_chain.cpp', 'src/shader_chain_window.cpp', 'src/shader_source_editor.cpp', 'third_party/ImGuiColorTextEdit/TextEditor.cpp',
  'src/shader_library.cpp', 'src/shader_runtime.cpp', 'src/shader_gl.cpp', 'src/shader_diagnostics.cpp', 'src/shader_source_io.cpp', 'src/dsp_chain.cpp', 'src/dsp_chain_window.cpp',
  'third_party/graphics/glad.cpp', 'third_party/graphics/volk.c',
  'third_party/imgui/imgui.cpp', 'third_party/imgui/imgui_draw.cpp', 'third_party/imgui/imgui_tables.cpp', 'third_party/imgui/imgui_widgets.cpp',
  'third_party/imgui/imgui_impl_win32.cpp', 'third_party/imgui/imgui_impl_opengl3.cpp', 'third_party/imgui/imgui_impl_vulkan.cpp'],
  include_directories: inc, dependencies: deps)
# Meson's strip option applies at install time. Strip the release executable at
# link time too, so the runnable build/einweggerat.exe is ready for distribution.
release_link_args = []
if get_option('buildtype') == 'release' and cpp.get_id() == 'gcc'
  release_link_args = cpp.get_supported_link_arguments('-s', '-Wl,--gc-sections')
endif
app = executable('einweggerat', ['src/main.cpp', 'src/core.cpp'], include_directories: inc,
  link_with: platform, dependencies: deps, link_args: release_link_args,
  win_subsystem: 'windows', install: true)
install_data('third_party/fonts/Roboto-LICENSE.txt', 'third_party/fonts/ForkAwesome-LICENSES.txt',
  install_dir: get_option('datadir') / 'licenses/einweggerat/fonts')
font_tests = executable('font-tests', 'tests/font_tests.cpp', include_directories: inc, link_with: platform, dependencies: deps)
test('embedded-fonts', font_tests, timeout: 15)
soundfont = custom_target('test-soundfont', output: 'fixture.sf2',
  command: [find_program('python3'), files('tests/make_test_soundfont.py'), '@OUTPUT@'], build_by_default: true)
checks = executable('native-tests', 'tests/native_tests.cpp', include_directories: inc, link_with: platform, dependencies: deps)
input_tests = executable('input-tests', 'tests/input_tests.cpp', include_directories: inc, link_with: platform, dependencies: deps)
test('native-input-bindings', input_tests, args: [meson.current_build_dir() / 'input-test'], timeout: 30)
subdir('tests')
catalog_tests = executable('catalog-tests', 'tests/catalog_tests.cpp', include_directories: inc, link_with: platform, dependencies: deps)
test('core-discovery', catalog_tests, args: [fixture.full_path(), meson.current_build_dir() / 'catalog-test'], timeout: 30)
test('content-first-startup', find_program('python3'),
  args: [files('tests/run_content_startup.py'), app, fixture, meson.current_build_dir() / 'content-startup'], timeout: 45)
test('gui-command-line', find_program('python3'), args: [files('tests/gui_cli_tests.py'), app], timeout: 35)
browser_tests = executable('browser-tests', 'tests/rom_browser_smoke.cpp', include_directories: inc, link_with: platform, dependencies: deps)
test('rom-browser-open-save', browser_tests, args: [meson.current_build_dir() / 'browser-test'], timeout: 30)
if cpp.get_id() == 'gcc'
  test('static-compiler-runtimes', find_program('python3'),
    args: [files('tests/verify_static_runtime.py'), find_program('objdump'), app], timeout: 15)
endif
lifecycle = executable('lifecycle-tests', ['tests/lifecycle_tests.cpp', 'src/core.cpp'], include_directories: inc, link_with: platform, dependencies: deps)
test('state-options-context-lifecycle', lifecycle, args: [fixture.full_path(), meson.current_build_dir() / 'lifecycle-run'], timeout: 30)
test('native-vfs-input-audio-midi', checks, args: [soundfont], timeout: 30)
dsp_tests = executable('dsp-tests', 'tests/dsp_smoke.cpp', include_directories: inc, link_with: platform, dependencies: deps)
test('dsp', dsp_tests, args: [meson.current_source_dir() / 'third_party/libretro-dsp/audio/dsp_filters', meson.current_build_dir() / 'dsp-test-output'], timeout: 90)
shader_tests = executable('shader-tests', 'tests/shader_tests.cpp', include_directories: inc, link_with: platform, dependencies: deps)
# Passing a third argument enables GPU checks. Requires optional librashader.dll beside the executable.
test('shader-import-export', shader_tests, args: [meson.current_build_dir() / 'shader-test-output', meson.current_source_dir() / 'tests/shader_fixture.slang'], timeout: 30)

# Instrument the actual shared editors for device-independent interaction tests.
ui_test_support = static_library('ui-test-support',
  ['src/common.cpp', 'src/log.cpp', 'src/log_window.cpp', 'src/rom_browser.cpp', 'src/dsp_chain.cpp', 'src/dsp_chain_window.cpp',
   'src/shader_chain.cpp', 'src/shader_chain_window.cpp', 'src/shader_source_editor.cpp',
   'src/shader_library.cpp', 'src/shader_source_io.cpp', 'src/shader_diagnostics.cpp',
   'third_party/ImGuiColorTextEdit/TextEditor.cpp', 'third_party/imgui/imgui.cpp',
   'third_party/imgui/imgui_draw.cpp', 'third_party/imgui/imgui_tables.cpp', 'third_party/imgui/imgui_widgets.cpp'],
  include_directories: inc, cpp_args: ['-DIMGUI_ENABLE_TEST_ENGINE'], dependencies: deps)
dsp_ui_tests = executable('dsp-ui-tests', 'tests/dsp_ui_smoke.cpp', include_directories: inc,
  cpp_args: ['-DIMGUI_ENABLE_TEST_ENGINE'], link_with: ui_test_support, dependencies: deps)
test('dsp-editor-ui', dsp_ui_tests, args: [meson.current_build_dir() / 'dsp-ui-test'], timeout: 60)
shader_ui_tests = executable('shader-ui-tests', 'tests/shader_ui_smoke.cpp', include_directories: inc,
  cpp_args: ['-DIMGUI_ENABLE_TEST_ENGINE'], link_with: ui_test_support, dependencies: deps)
test('shader-editor-ui', shader_ui_tests,
  args: [meson.current_build_dir() / 'shader-ui-test', meson.current_source_dir() / 'tests/shader_fixture.slang'], timeout: 60)
log_tests = executable('log-tests', 'tests/log_tests.cpp', include_directories: inc,
  cpp_args: ['-DIMGUI_ENABLE_TEST_ENGINE'], link_with: ui_test_support, dependencies: deps)
test('colored-log-dialog', log_tests, timeout: 30)
test('core-lifecycle-gl', app, args: ['--core', fixture.full_path(), '--frames', '180', '--hidden', '--verify-video'], timeout: 30)
test('core-lifecycle-vulkan', app, args: ['--context', 'vulkan', '--core', fixture.full_path(), '--frames', '180', '--hidden', '--verify-video'], timeout: 30)
foreach mode : [1, 2]
  hardware_fixture = hardware_fixtures[mode - 1]
  test('hardware-context-' + mode.to_string(), app,
    args: ['--core', hardware_fixture.full_path(), '--frames', '60', '--hidden', '--require-hardware'], timeout: 30)
endforeach
