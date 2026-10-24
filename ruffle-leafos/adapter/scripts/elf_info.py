from pathlib import Path
import re
import struct

def inspect(path):
    data = Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x02\x01': raise ValueError(f'not ELF64 little-endian: {path}')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    kind, machine = header[1:3]
    shoff, shsize, count, names_index = header[6], header[11], header[12], header[13]
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i*shsize) for i in range(count)]
    def raw(section): return data[section[4]:section[4]+section[5]]
    def cstring(blob, offset): return blob[offset:blob.index(0, offset)].decode('utf8', 'replace')
    names = raw(sections[names_index])
    by_name = {cstring(names, s[0]): s for s in sections}
    strings = raw(by_name['.dynstr'])
    needed = []
    dynamic = by_name['.dynamic']
    for offset in range(dynamic[4], dynamic[4]+dynamic[5], 16):
        key, value = struct.unpack_from('<qQ', data, offset)
        if key == 1: needed.append(cstring(strings, value))
    exports, imports = [], []
    symbols = by_name['.dynsym']
    for offset in range(symbols[4], symbols[4]+symbols[5], 24):
        name, info, other, index, value, size = struct.unpack_from('<IBBHQQ', data, offset)
        if name: (exports if index else imports).append(cstring(strings, name))
    versions = sorted({s.decode() for s in re.findall(rb'GLIBC_\d+\.\d+(?:\.\d+)?', strings)}, key=lambda s: tuple(map(int, s[6:].split('.'))))
    return {'machine': machine, 'type': kind, 'needed': needed, 'exports': exports, 'imports': imports, 'glibc_versions': versions}
