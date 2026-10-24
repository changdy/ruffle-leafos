"""Generate an original vector-only Flash movie and a small PNG system icon."""
from pathlib import Path
import struct
import zlib

class Bits:
    def __init__(self): self.bits = []
    def put(self, value, count):
        self.bits.extend((value >> n) & 1 for n in range(count - 1, -1, -1))
    def data(self):
        padded = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(sum(padded[i+j] << (7-j) for j in range(8)) for i in range(0, len(padded), 8))

def signed_bits(*values): return max(1, *(abs(x).bit_length() + 1 for x in values))
def rect(width, height):
    b = Bits(); n = signed_bits(width, height)
    b.put(n, 5)
    for x in (0, width, 0, height): b.put(x, n)
    return b.data()

def matrix(x, y):
    b = Bits(); b.put(0, 1); b.put(0, 1)
    n = signed_bits(x, y); b.put(n, 5); b.put(x, n); b.put(y, n)
    return b.data()

def tag(kind, payload=b''):
    length = len(payload)
    return struct.pack('<H', kind << 6 | min(length, 63)) + (struct.pack('<I', length) if length >= 63 else b'') + payload

def shape(identifier, width, height, rgb):
    w, h = width * 20, height * 20
    b = Bits(); b.put(1, 4); b.put(0, 4)
    b.put(0, 1); b.put(0b00101, 5) # MoveTo and FillStyle1.
    b.put(1, 5); b.put(0, 1); b.put(0, 1); b.put(1, 1)
    for dx, dy in ((w, 0), (0, h), (-w, 0), (0, -h)):
        n = max(2, signed_bits(dx, dy))
        b.put(1, 1); b.put(1, 1); b.put(n - 2, 4); b.put(1, 1)
        b.put(dx, n); b.put(dy, n)
    b.put(0, 6)
    body = struct.pack('<H', identifier) + rect(w, h)
    body += bytes([1, 0, *rgb, 0]) + b.data()
    return tag(2, body)

def make_movie(path: Path):
    frames = 72
    body = rect(640*20, 480*20) + struct.pack('<HH', 24*256, frames)
    body += tag(69, struct.pack('<I', 0)) # AVM1, no networking flags.
    body += tag(9, bytes([20, 25, 32]))
    body += shape(1, 60, 40, (245, 205, 55))
    colors = [(235, 70, 70), (60, 205, 100), (70, 130, 245), (240, 240, 240)]
    for identifier, color in enumerate(colors, 2): body += shape(identifier, 120, 80, color)
    for depth, (identifier, x, y) in enumerate([(2, 40, 40), (3, 480, 40), (4, 40, 360), (5, 480, 360)], 1):
        body += tag(26, bytes([6]) + struct.pack('<HH', depth, identifier) + matrix(x*20, y*20))
    for frame in range(frames):
        x = 40 + round(480 * frame / (frames - 1))
        if frame == 0:
            payload = bytes([6]) + struct.pack('<HH', 5, 1) + matrix(x*20, 220*20)
        else:
            payload = bytes([5]) + struct.pack('<H', 5) + matrix(x*20, 220*20)
        body += tag(26, payload) + tag(1)
    body += tag(0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'FWS' + bytes([8]) + struct.pack('<I', len(body)+8) + body)

def make_icon(path: Path):
    size = 96
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            letter = (26 <= x < 39 and 20 <= y < 76) or (26 <= x < 72 and 20 <= y < 32) or (26 <= x < 63 and 42 <= y < 54)
            rows.extend((245, 248, 243, 255) if letter else (58, 94, 66, 255))
    def chunk(kind, value):
        return struct.pack('>I', len(value)) + kind + value + struct.pack('>I', zlib.crc32(kind+value) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(png)
