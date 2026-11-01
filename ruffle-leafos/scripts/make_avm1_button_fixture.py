"""Build an original native AVM1 button/trackAsMenu drag regression fixture."""
from pathlib import Path
import struct
import sys

from make_keyboard_fixtures import ROOT, action, push, write_movie

sys.path.insert(0, str(ROOT / 'adapter/scripts'))
from make_demo import matrix, shape, tag


def button(identifier, menu=False):
    # DefineButton2: distinct artwork in Up/Over/Down states, plus a hit region.
    records = b''
    for state, art in [(1, 1), (2, 2), (4, 3), (8, 1)]:
        records += bytes([state]) + struct.pack('<HH', art, 1) + matrix(0, 0) + b'\0'
    return tag(34, struct.pack('<HBH', identifier, menu, 0) + records + b'\0')


def set_tracking(name, value):
    return (push('_root') + action(0x1c) + push(name) + action(0x4e)
            + push('trackAsMenu') + action(0x96, bytes([5, value])) + action(0x4f))


def main():
    tags = b''.join(shape(i, 100, 100, color) for i, color in [
        (1, (60, 70, 80)), (2, (230, 200, 50)), (3, (230, 60, 60))])
    tags += button(10) + button(11, menu=True)
    for depth, (identifier, name, x, y) in enumerate([
            (10, 'origin', 100, 100), (10, 'push', 300, 100),
            (11, 'menu', 500, 100), (10, 'dynamicMenu', 700, 100),
            (11, 'disabledMenu', 100, 300)], 1):
        tags += tag(26, b'\x26' + struct.pack('<HH', depth, identifier)
                    + matrix(x * 20, y * 20) + name.encode('ascii') + b'\0')
    script = set_tracking('dynamicMenu', True) + set_tracking('disabledMenu', False)
    script += action(0x07) + b'\0'
    tags += tag(12, script)
    write_movie(ROOT / 'player/core/src/player/tests/button-drag.swf', tags)


if __name__ == '__main__':
    main()
