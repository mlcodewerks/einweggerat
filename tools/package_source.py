"""Package the standalone source tree using only Python's standard library."""
import argparse
import hashlib
from pathlib import Path
import zipfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=root / 'dist/einweggerat-source.zip')
    args = parser.parse_args()
    files = [root / name for name in (
        '.clang-format', '.gitignore', 'LICENSE', 'README.md', 'THIRD_PARTY.md',
        'meson.build', 'build.ps1')]
    for name in ('src', 'third_party', 'tests', 'tools'):
        directory = root / name
        for path in [directory, *directory.rglob('*')]:
            if path.is_symlink() or getattr(path, 'is_junction', lambda: False)():
                raise RuntimeError(f'Linked source is not self-contained: {path}')
            if '__pycache__' in path.parts or path.suffix == '.pyc':
                continue
            if path.is_file():
                files.append(path)
    output = args.output.resolve()
    if output.exists():
        raise FileExistsError(f'Refusing to overwrite {output}; choose another --output path.')
    output.parent.mkdir(parents=True, exist_ok=True)
    hashes = []
    with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(files):
            relative = path.relative_to(root).as_posix()
            data = path.read_bytes()
            archive.writestr('einweggerat/' + relative, data)
            hashes.append(f'{hashlib.sha256(data).hexdigest()}  {relative}\n')
        archive.writestr('einweggerat/SHA256SUMS', ''.join(hashes))
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise RuntimeError('Archive verification failed')
    print(f'{output}\n{len(files)} source files; {output.stat().st_size:,} bytes')


if __name__ == '__main__':
    main()
