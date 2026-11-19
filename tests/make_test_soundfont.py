"""Generate a tiny SF2 with a looping sine wave; no third-party sound assets."""
import math
import pathlib
import struct
import sys


def chunk(name, data):
    return name.encode() + struct.pack('<I', len(data)) + data + b'\0' * (len(data) % 2)


def listing(name, data):
    return chunk('LIST', name.encode() + data)


info = listing('INFO', chunk('ifil', struct.pack('<HH', 2, 1))
               + chunk('INAM', b'einweggerat MIDI test\0'))
samples = [int(16000 * math.sin(2 * math.pi * i / 480)) for i in range(480)] + [0] * 46
sdta = listing('sdta', chunk('smpl', struct.pack('<526h', *samples)))
phdr = struct.pack('<20sHHHIII', b'Sine', 0, 0, 0, 0, 0, 0)
phdr += struct.pack('<20sHHHIII', b'EOP', 0, 0, 1, 0, 0, 0)
pbag = struct.pack('<4H', 0, 0, 1, 0)
pgen = struct.pack('<4H', 41, 0, 0, 0)
inst = struct.pack('<20sH20sH', b'Sine', 0, b'EOI', 1)
ibag = struct.pack('<4H', 0, 0, 2, 0)
igen = struct.pack('<6H', 54, 1, 53, 0, 0, 0)
shdr = struct.pack('<20sIIIIIBbHH', b'Sine', 0, 480, 0, 480, 48000, 60, 0, 0, 1)
shdr += struct.pack('<20sIIIIIBbHH', b'EOS', 480, 480, 480, 480, 48000, 0, 0, 0, 1)
pdta = listing('pdta', b''.join(chunk(name, data) for name, data in [
    ('phdr', phdr), ('pbag', pbag), ('pmod', bytes(10)), ('pgen', pgen),
    ('inst', inst), ('ibag', ibag), ('imod', bytes(10)), ('igen', igen), ('shdr', shdr)]))
pathlib.Path(sys.argv[1]).write_bytes(chunk('RIFF', b'sfbk' + info + sdta + pdta))
