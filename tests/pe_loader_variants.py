"""Exercise rebinding, eager delay imports, and ZIP validation using real DLL fixtures."""
from pathlib import Path
import struct
import subprocess
import sys
import zipfile

def run(command, success=True):
    result = subprocess.run([str(v) for v in command], capture_output=True, text=True, timeout=30)
    if (success and result.returncode != 0) or (not success and result.returncode != 1):
        raise RuntimeError(f'{command}: {result.returncode}\n{result.stdout}\n{result.stderr}')

def main():
    tester, fixture, core, app, output, package, dependency = map(Path, sys.argv[1:])
    output.mkdir(parents=True, exist_ok=True)
    original = fixture.read_bytes()
    nt = struct.unpack_from('<I', original, 60)[0]
    optional = nt + 24
    x64 = struct.unpack_from('<H', original, optional)[0] == 0x20B
    directories = optional + (112 if x64 else 96)
    pointer = '<Q' if x64 else '<I'
    pointer_size = 8 if x64 else 4
    sections = optional + struct.unpack_from('<H', original, nt + 20)[0]
    count = struct.unpack_from('<H', original, nt + 6)[0]
    def offset(rva):
        for n in range(count):
            size, va, raw, ptr = struct.unpack_from('<IIII', original, sections + n * 40 + 8)
            if va <= rva < va + max(size, raw): return ptr + rva - va
        raise ValueError(f'Invalid RVA {rva:x}')
    imports = struct.unpack_from('<I', original, directories + 8)[0]
    bound = bytearray(original)
    cursor = offset(imports)
    while struct.unpack_from('<I', bound, cursor + 12)[0]:
        names, _, _, _, iat = struct.unpack_from('<IIIII', bound, cursor)
        struct.pack_into('<I', bound, cursor + 4, 0x12345678)
        a = offset(iat)
        while struct.unpack_from(pointer, bound, a)[0]:
            struct.pack_into(pointer, bound, a, 0xDEADBEEF12345678 if x64 else 0xDEADBEEF)
            a += pointer_size
        cursor += 20
    path = output / 'memory-bound.dll'
    path.write_bytes(bound)
    run([tester, '--fixture', path])

    delayed = bytearray(original)
    first = offset(imports)
    names, _, _, name, iat = struct.unpack_from('<IIIII', original, first)
    cursor = first
    while any(delayed[cursor:cursor + 20]):
        delayed[cursor:cursor + 20] = delayed[cursor + 20:cursor + 40]
        cursor += 20
    # Append an initialized data section containing a PE32+ delay descriptor.
    rva = (struct.unpack_from('<I', delayed, optional + 56)[0] + 4095) & ~4095
    raw = (len(delayed) + 511) & ~511
    if sections + (count + 1) * 40 > struct.unpack_from('<I', delayed, optional + 60)[0]:
        raise RuntimeError('Fixture has no section-header space')
    delayed.extend(bytes(raw + 512 - len(delayed)))
    header = sections + count * 40
    struct.pack_into('<8sIIIIIIHHI', delayed, header, b'.delay\0\0', 512, rva, 512, raw, 0, 0, 0, 0, 0xC0000040)
    struct.pack_into('<H', delayed, nt + 6, count + 1)
    struct.pack_into('<I', delayed, optional + 56, rva + 4096)
    struct.pack_into('<II', delayed, directories + 13 * 8, rva, 64)
    struct.pack_into('<IIIIIIII', delayed, raw, 1, name, rva + 64, iat, names, 0, 0, 0x12345678)
    path = output / 'memory-delayed.dll'
    path.write_bytes(delayed)
    run([tester, '--fixture', path])
    if not x64:
        legacy = bytearray(delayed)
        image_base = struct.unpack_from('<I', legacy, optional + 28)[0]
        struct.pack_into('<I', legacy, raw, 0)
        fields = []
        for index in range(1, 7):
            value = struct.unpack_from('<I', legacy, raw + index * 4)[0]
            if value:
                struct.pack_into('<I', legacy, raw + index * 4, image_base + value)
                fields.append((3 << 12) | index * 4)
        if len(fields) % 2: fields.append(0)
        block = struct.pack('<II', rva, 8 + len(fields) * 2) + struct.pack('<' + 'H' * len(fields), *fields)
        reloc_rva, reloc_size = struct.unpack_from('<II', original, directories + 5 * 8)
        relocations = original[offset(reloc_rva):offset(reloc_rva) + reloc_size] + block
        allocation = (256 + len(relocations) + 511) & ~511
        legacy.extend(bytes(raw + allocation - len(legacy)))
        legacy[raw + 256:raw + 256 + len(relocations)] = relocations
        struct.pack_into('<I', legacy, header + 8, allocation)
        struct.pack_into('<I', legacy, header + 16, allocation)
        struct.pack_into('<I', legacy, optional + 56, (rva + allocation + 4095) & ~4095)
        struct.pack_into('<II', legacy, directories + 5 * 8, rva + 256, len(relocations))
        path = output / 'memory-legacy-delay.dll'
        path.write_bytes(legacy)
        run([tester, '--fixture', path])

    zero_fill = bytearray(original)
    tls_rva = struct.unpack_from('<I', original, directories + 9 * 8)[0]
    struct.pack_into('<I', zero_fill, offset(tls_rva) + pointer_size * 4, 4096)
    path = output / 'memory-zero-fill.dll'
    path.write_bytes(zero_fill)
    run([tester, '--fixture', path])

    for mode in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
        archive = output / f'core-{mode}-\u65e5\u672c.zip'
        with zipfile.ZipFile(archive, 'w', mode) as z: z.write(core, '\u65e5\u672c/nested/fixture-core.dll')
        run([tester, archive])
        if app.name != '-':
            run([app, '--core', archive, '--hidden', '--no-audio', '--frames', '30', '--verify-video'])
    # A real import dependency must map from ZIP even when its DLL exists on disk.
    archive = output / 'memory-package.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        z.write(package, 'memory-package.dll')
        z.write(dependency, 'helpers/memory-dependency.dll')
    run([tester, archive])
    for name, entries in [('empty', []), ('invalid', [('bad.dll', b'not a DLL')]),
                          ('duplicates', [('a/core.dll', original), ('b/CORE.DLL', original)]),
                          ('ambiguous', [('a.dll', original), ('b.dll', original)])]:
        archive = output / (name + '.zip')
        with zipfile.ZipFile(archive, 'w') as z:
            for entry, data in entries: z.writestr(entry, data)
        run([tester, archive], success=False)
    archive = output / 'corrupt.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_STORED) as z: z.write(core, 'core.dll')
    data = bytearray(archive.read_bytes()); data[100] ^= 0xFF; archive.write_bytes(data)
    run([tester, archive], success=False)
    print('PASS bound IAT rebinding, eager delay imports, stored/deflated Unicode ZIPs, playback, archive validation')

if __name__ == '__main__': main()
