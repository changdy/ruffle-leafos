"""Install the pinned Windows-to-AArch64 GNU/Linux compiler inside this project."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import ssl
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--version', default='14.2.rel1')
parser.add_argument('--host', default='mingw-w64-x86_64')
args = parser.parse_args()
NAME = f'arm-gnu-toolchain-{args.version}-{args.host}-aarch64-none-linux-gnu.zip'
URL = f'https://developer.arm.com/-/media/Files/downloads/gnu/{args.version}/binrel/' + NAME
TOOLS = ROOT / '.tools'
DOWNLOADS = TOOLS / 'downloads'
DOWNLOADS.mkdir(parents=True, exist_ok=True)
from pip._vendor import certifi
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
    urllib.request.HTTPSHandler(context=ssl.create_default_context(cafile=certifi.where())))

def fetch(url):
    return opener.open(urllib.request.Request(url, headers={'User-Agent': 'Ruffle-LeafOS-build/1'}), timeout=40)

signature = fetch(URL + '.sha256asc').read().decode('ascii')
(DOWNLOADS / (NAME + '.sha256asc')).write_text(signature, encoding='ascii')
match = re.search(r'\b([0-9a-fA-F]{64})\b', signature)
if not match:
    raise SystemExit('Official SHA-256 document did not contain a digest')
expected = match.group(1).lower()
archive = DOWNLOADS / NAME
if not archive.exists():
    temporary = archive.with_suffix('.zip.part')
    with fetch(URL) as response, temporary.open('wb') as output:
        total = int(response.headers.get('Content-Length', 0))
        size = 0
        while chunk := response.read(1024 * 1024):
            output.write(chunk)
            size += len(chunk)
            if size % (8 * 1024 * 1024) == 0:
                print(f'downloaded={size} total={total}', flush=True)
    temporary.replace(archive)
actual = hashlib.sha256(archive.read_bytes()).hexdigest()
if actual != expected:
    raise SystemExit(f'SHA-256 mismatch: expected {expected}, got {actual}')
print(f'official_sha256={actual}', flush=True)
destination = TOOLS / ('arm-gnu-' + args.version)
destination.mkdir(exist_ok=True)
with zipfile.ZipFile(archive) as source:
    # Strip the vendor's long enclosing directory to keep Windows paths short.
    prefixes = {item.filename.replace('\\', '/').split('/')[0] for item in source.infolist()}
    strip_prefix = next(iter(prefixes)) + '/' if len(prefixes) == 1 else ''
    for item in source.infolist():
        name = item.filename.replace('\\', '/')
        if strip_prefix and name.startswith(strip_prefix):
            name = name[len(strip_prefix):]
        if not name:
            continue
        target = (destination / name).resolve()
        if not target.is_relative_to(destination.resolve()):
            raise SystemExit('Unsafe archive path: ' + item.filename)
        if item.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open(item) as data, target.open('wb') as output:
                shutil.copyfileobj(data, output)
compilers = list(destination.rglob('aarch64-none-linux-gnu-gcc.exe'))
if len(compilers) != 1:
    raise SystemExit(f'Expected one compiler, found {compilers}')
record = {'version': args.version, 'host': args.host, 'url': URL, 'sha256': actual,
          'checksum_source': URL + '.sha256asc',
          'compiler': str(compilers[0].relative_to(ROOT)),
          'root': str(compilers[0].parent.parent.relative_to(ROOT))}
(ROOT / 'arm-gnu.lock.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf8')
print(json.dumps(record, indent=2), flush=True)
