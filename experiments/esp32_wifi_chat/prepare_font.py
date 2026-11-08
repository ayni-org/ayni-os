"""Build a fixed-cell BMP font from pinned GNU Unifont 16.0.04 (SIL OFL 1.1)."""
import gzip
import hashlib
from pathlib import Path
import sys
import struct
import zlib

SOURCE_SHA256 = 'f9c8c7802453f47be02677176aeac2342ee96d354fad7a26cedcce48e68e1d9f'
RANGES = ((0, 0x100), (0x2000, 0x2070), (0x3000, 0x3040),
          (0x3400, 0xA000), (0xFF00, 0xFFF0))


def convert(source, destination):
    raw = Path(source).read_bytes()
    if not SOURCE_SHA256 or hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        raise ValueError('Pinned Unifont source digest mismatch')
    glyphs = {}
    for line in gzip.decompress(raw).decode('ascii').splitlines():
        code, glyph = line.split(':')
        code, bits = int(code, 16), bytes.fromhex(glyph)
        if len(bits) not in (16, 32):
            continue
        width = 8 if len(bits) == 16 else 16
        if len(bits) == 16:
            bits = b''.join(bytes((row, 0)) for row in bits)
        glyphs[code] = bytes((width,)) + bits
    cells = b''.join(glyphs.get(code, glyphs[0x3f])
                     for start, end in RANGES for code in range(start, end))
    # Independent blocks keep random glyph lookup bounded to 2,112 bytes of
    # scratch while retaining full selected-range coverage. XiP copies rodata
    # into PSRAM, so embedding the uncompressed font wastes working memory.
    block_cells = 64
    blocks = [zlib.compress(cells[at:at + block_cells * 33], 9)
              for at in range(0, len(cells), block_cells * 33)]
    header = b'AYNF' + struct.pack('<HHII', 2, len(RANGES), len(cells) // 33, block_cells)
    index = 0
    for start, end in RANGES:
        header += struct.pack('<III', start, end, index)
        index += end - start
    offset = len(header) + len(blocks) * 8
    for block in blocks:
        header += struct.pack('<II', offset, len(block))
        offset += len(block)
    output = header + b''.join(blocks)
    target = Path(destination)
    if not target.exists() or target.read_bytes() != output:
        target.write_bytes(output)


if __name__ == '__main__':
    convert(*sys.argv[1:])
