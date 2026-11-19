# Test DLLs stay outside the two production core-discovery directories.
fixture = shared_library('fixture-core', 'fixture_core.cpp', include_directories: inc, name_prefix: '')
hardware_fixtures = []
foreach mode : [1, 2]
  hardware_fixtures += shared_library('hardware-fixture-' + mode.to_string(), ['fixture_core.cpp', 'hardware_fixture.cpp'],
    include_directories: inc, dependencies: vulkan_headers, cpp_args: ['-DFIXTURE_HARDWARE=' + mode.to_string(), '-DVK_NO_PROTOTYPES'], name_prefix: '')
endforeach
