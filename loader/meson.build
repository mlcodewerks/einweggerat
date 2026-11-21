project('memorymodulepp', 'c', 'cpp', version: '0.1.0', default_options: ['c_std=gnu11', 'cpp_std=c++17', 'buildtype=release'])
if host_machine.system() != 'windows' or host_machine.cpu_family() not in ['x86', 'x86_64']
  error('MemoryModulePP requires Windows x86 or x64.')
endif
add_project_arguments('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00', language: ['c', 'cpp'])
if meson.get_compiler('cpp').get_id() == 'gcc'
  if host_machine.cpu_family() == 'x86'
    # DW2 unwinding across DLL boundaries requires one shared libgcc runtime.
    add_project_link_arguments('-shared-libgcc', '-static-libstdc++', language: 'cpp')
  else
    add_project_link_arguments('-static', '-static-libgcc', '-static-libstdc++', language: 'cpp')
  endif
endif
inc = include_directories('../src', '../third_party/libretro')
deps = [dependency('threads'), meson.get_compiler('cpp').find_library('shell32')]
loader = static_library('memorymodulepp', '../third_party/MemoryModulePP.c')
support = static_library('loader-support', ['../src/common.cpp', '../src/core_library.cpp', '../third_party/miniz.c'],
  include_directories: inc, dependencies: deps)
seh = host_machine.cpu_family() == 'x86' ? '../tests/memory_seh_x86_fixture.S' : '../tests/memory_seh_fixture.S'
fixture_sources = ['../tests/memory_fixture.cpp', seh]
if host_machine.cpu_family() == 'x86'
  fixture_sources += '../tests/memory_load_config_x86.c'
endif
fixture = shared_library('memory-fixture', fixture_sources, include_directories: inc,
  vs_module_defs: '../tests/memory_exports.def', name_prefix: '')
large_fixture = shared_library('memory-large-fixture', fixture_sources, include_directories: inc,
  cpp_args: '-DFIXTURE_LARGE_TLS', vs_module_defs: '../tests/memory_exports.def', name_prefix: '')
failed_fixture = shared_library('memory-failed-fixture', fixture_sources, include_directories: inc,
  cpp_args: '-DFIXTURE_FAIL_ATTACH', vs_module_defs: '../tests/memory_exports.def', name_prefix: '')
checks = executable('memory-loader-tests', '../tests/memory_loader_tests.cpp', include_directories: inc,
  link_with: [support, loader], dependencies: deps, link_args: '-municode')
test('tls-exceptions-seh', checks, args: ['--fixture', fixture.full_path()], timeout: 45)
test('large-tls', checks, args: ['--fixture', large_fixture.full_path()], depends: fixture, timeout: 45)
test('failed-attach', checks, args: ['--reject', failed_fixture.full_path()], depends: fixture, timeout: 45)
core = shared_library('fixture-core', '../tests/fixture_core.cpp', include_directories: inc, name_prefix: '')
dependency_fixture = shared_library('memory-dependency', '../tests/memory_dependency.cpp', name_prefix: '')
package = shared_library('memory-package', '../tests/memory_package.cpp', include_directories: inc, link_with: dependency_fixture, name_prefix: '')
test('zip-bound-delay-imports', find_program('python3'),
  args: [files('../tests/pe_loader_variants.py'), checks, fixture, core, '-', meson.current_build_dir() / 'variants', package, dependency_fixture], timeout: 90)
