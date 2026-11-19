param([switch]$Test, [switch]$TestShaders)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (!(Test-Path build/meson-private/coredata.dat)) {
        if (!$env:CC) { $env:CC = (Get-Command gcc -ErrorAction Stop).Source }
        if (!$env:CXX) { $env:CXX = (Get-Command g++ -ErrorAction Stop).Source }
        & meson setup build
        if ($LASTEXITCODE) { throw 'Configure failed' }
    }
    & meson compile -C build
    if ($LASTEXITCODE) { throw 'Build failed' }
    if ($Test) {
        & meson test -C build --no-rebuild --print-errorlogs --num-processes 1
        if ($LASTEXITCODE) { throw 'Tests failed' }
    }
    if ($TestShaders) {
        & ./build/shader-tests.exe ./build/shader-gpu-test ./tests/shader_fixture.slang gpu
        if ($LASTEXITCODE) { throw 'OpenGL shader tests failed' }
        & ./build/einweggerat.exe --context vulkan --core ./build/tests/fixture-core.dll --hidden --frames 120 --shader ./tests/shader_fixture.slang --verify-video
        if ($LASTEXITCODE) { throw 'Vulkan shader tests failed' }
    }
} finally { Pop-Location }
