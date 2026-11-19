"""Exercise content-first startup from a different cwd with root/cores discovery."""
import ctypes
from ctypes import wintypes
import pathlib
import re
import time
import shutil
import subprocess
import sys
app, core, scratch = (pathlib.Path(value).resolve() for value in sys.argv[1:])
for location in ('root', 'cores'):
    case = scratch / location
    case.mkdir(parents=True, exist_ok=True)
    exe = case / app.name
    shutil.copy2(app, exe)
    destination = case if location == 'root' else case / 'cores'
    destination.mkdir(exist_ok=True)
    shutil.copy2(core, destination / 'test-core.DLL')
    game = case / 'GAME.BIN'
    game.write_bytes(b'fixture content')
    # Use a positional path too, for Windows file-association/drag-to-executable use.
    args = [str(game)] if location == 'root' else ['--rom', str(game)]
    result = subprocess.run([str(exe), '--hidden', '--frames', '3', '--no-audio', '--verify-video', *args],
                            cwd=scratch, capture_output=True, text=True, timeout=20)
    if result.returncode or 'Loaded Native fixture' not in result.stdout or 'verified=3' not in result.stdout:
        raise SystemExit(result.stdout + result.stderr)
    assert (case / 'system').is_dir()
    assert (case / 'saves/test-core/GAME.srm').is_file()
    assert (case / 'config/test-core.options').is_file()

    # A separate process must restore the entire save, then save its latest changes
    # before the periodic 600-frame autosave can run.
    save = case / 'saves/test-core/GAME.srm'
    seed = bytes((index * 37 + 19) & 255 for index in range(save.stat().st_size))
    save.write_bytes(seed)
    result = subprocess.run([str(exe), '--hidden', '--frames', '3', '--no-audio', *args],
                            cwd=scratch, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'Loaded SRAM:' in result.stdout and 'Saved SRAM:' in result.stdout, result.stdout
    assert save.read_bytes() == bytes([3]) + seed[1:], 'SRAM did not survive process restart'

    # Exercise the window close button's WM_CLOSE path too, without a frame limit.
    save.write_bytes(seed)
    process = subprocess.Popen([str(exe), '--hidden', '--no-audio', *args], cwd=scratch,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        user32 = ctypes.WinDLL('user32', use_last_error=True)
        user32.WaitForInputIdle.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        user32.WaitForInputIdle.restype = wintypes.DWORD
        assert user32.WaitForInputIdle(int(process._handle), 10000) == 0, 'GUI did not become idle'
        time.sleep(0.25)
        windows = []
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
        user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

        @callback_type
        def find_window(window, unused):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
            name = ctypes.create_unicode_buffer(128)
            user32.GetClassNameW(window, name, len(name))
            if pid.value == process.pid and name.value == 'einweggerat':
                windows.append(window)
            return True

        user32.EnumWindows(find_window, 0)
        assert len(windows) == 1, 'Cannot locate test application window'
        assert user32.PostMessageW(windows[0], 0x0010, 0, 0), 'WM_CLOSE failed'
        output, errors = process.communicate(timeout=20)
        assert process.returncode == 0, output + errors
        frames = re.search(r'RESULT frames=(\d+)', output)
        assert frames and 0 < int(frames[1]) < 600, output
        assert 'Loaded SRAM:' in output and 'Saved SRAM:' in output, output
        assert save.read_bytes() == bytes([int(frames[1]) & 255]) + seed[1:], 'Window close lost SRAM'
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
print('PASS content-first startup, core scanning, executable-relative paths, SRAM restoration across processes and save on WM_CLOSE')
