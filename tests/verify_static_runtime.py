"""Keep the frontend independent of compiler runtime DLLs (cores are separate modules)."""
import subprocess
import sys
output = subprocess.check_output([sys.argv[1], '-p', sys.argv[2]], text=True)
imports = [line.split(':', 1)[1].strip().lower() for line in output.splitlines() if 'DLL Name:' in line]
for name in imports:
    if any(part in name for part in ('libstdc++', 'libgcc_s', 'libwinpthread', 'libc++', 'libunwind')):
        raise SystemExit('Compiler runtime must be statically linked: ' + name)
print('PASS compiler runtimes, including winpthread, are statically linked')
