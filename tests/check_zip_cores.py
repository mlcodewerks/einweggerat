"""Opt-in compatibility test against user-supplied cores and ROMs (never modifies them)."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--cores', type=Path, required=True)
    parser.add_argument('--dependencies', type=Path, required=True)
    parser.add_argument('--roms', type=Path)
    args = parser.parse_args()
    build = args.build.resolve()
    output = build / 'zip-compatibility'
    output.mkdir(exist_ok=True)
    for dll in args.dependencies.glob('*.dll'):
        shutil.copy2(dll, build / dll.name)
    bios = args.dependencies / 'system' / 'lynxboot.img'
    if bios.exists():
        (build / 'system').mkdir(exist_ok=True)
        shutil.copy2(bios, build / 'system' / bios.name)
    results = []
    for core in sorted(args.cores.glob('*.dll')):
        archive = output / (core.stem + '.zip')
        if not archive.exists() or archive.stat().st_mtime < core.stat().st_mtime:
            with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=1) as z:
                z.write(core, 'nested/' + core.name)
        for path in (core, archive):
            try:
                result = subprocess.run([str(build / 'memory-loader-tests.exe'), str(path.resolve()), str(args.dependencies.resolve())],
                                        capture_output=True, text=True, timeout=45)
                item = dict(core=core.name, mode=path.suffix, code=result.returncode, output=result.stdout + result.stderr)
            except subprocess.TimeoutExpired:
                item = dict(core=core.name, mode=path.suffix, code='timeout', output='')
            results.append(item)
            print(f"{item['code']} {core.name} {path.suffix}: {item['output'].strip()[:300]}", flush=True)
            (output / 'results.json').write_text(json.dumps(results, indent=2))
    if args.roms:
        pairs = [('snes9x', 'Mega Man X (USA).sfc'), ('mgba', 'pocket.gb'),
                 ('mednafen_ngp', 'FANDF.NGP'), ('gearcoleco', 'Donkey Kong (USA, Europe).col'),
                 ('gearlynx', 'xor_scroll.lnx'), ('geargrafx', 'Bomberman (USA).pce')]
        for core, rom in pairs:
            archive = output / (core + '_libretro.zip')
            content = args.roms / rom
            if not archive.exists() or not content.exists(): continue
            for path in (args.cores / (core + '_libretro.dll'), archive):
                try:
                    result = subprocess.run([str(build / 'einweggerat.exe'), '--core', str(path.resolve()), '--rom', str(content.resolve()),
                                         '--hidden', '--no-audio', '--frames', '120', '--verify-video'],
                                        capture_output=True, text=True, timeout=60)
                    item = dict(core=core, rom=rom, mode='gameplay' + path.suffix, code=result.returncode, output=result.stdout + result.stderr)
                except subprocess.TimeoutExpired:
                    item = dict(core=core, rom=rom, mode='gameplay' + path.suffix, code='timeout', output='')
                results.append(item)
                print(f"{item['code']} {item['mode']} {core}: {item['output'][-500:]}", flush=True)
                (output / 'results.json').write_text(json.dumps(results, indent=2))
    # Report baseline failures, but fail this comparison only for ZIP regressions.
    failures = []
    for native, zipped in zip(results[::2], results[1::2]):
        if native['code'] != zipped['code'] or (zipped['mode'] == '.zip' and zipped['code'] != 0):
            failures.append(zipped)
    print(f"Compared {len(results) // 2} DLL/ZIP pairs; {len(failures)} ZIP regressions", flush=True)
    return bool(failures)

if __name__ == '__main__':
    raise SystemExit(main())
