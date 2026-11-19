"""Verify GUI subsystem, command-line parsing and redirected standard streams."""
import pathlib
import struct
import subprocess
import sys

app = pathlib.Path(sys.argv[1]).resolve()
assert app.name == 'einweggerat.exe', 'Frontend executable must use the einweggerat name'
data = app.read_bytes()
pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[pe:pe + 4] == b'PE\0\0'
assert struct.unpack_from('<H', data, pe + 24 + 68)[0] == 2, 'Application must use Windows GUI subsystem'
# No parent console is available in this detached launch. Help must still reach the inherited pipe.
result = subprocess.run([str(app), '--help'], capture_output=True, text=True,
                        creationflags=subprocess.DETACHED_PROCESS, timeout=15)
assert result.returncode == 0 and result.stdout.startswith('einweggerat ') and '--core CORE.dll' in result.stdout, result
result = subprocess.run([str(app), '--hidden', '--invalid-option'], capture_output=True, text=True,
                        creationflags=subprocess.DETACHED_PROCESS, timeout=15)
assert result.returncode == 1 and 'Unknown argument: --invalid-option' in result.stderr, result
print('PASS GUI subsystem without console allocation, command-line options, redirected stdout/stderr and exit status')
