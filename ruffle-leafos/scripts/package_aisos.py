"""Build an RGBox app ZIP from the verified 0.8.6 release and PySDL2 wheel."""
import argparse
import hashlib
from io import BytesIO
from pathlib import Path
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

PROJECT = Path(__file__).resolve().parents[1]
LEAF_SHA = "93111dc654be29f01247548b2dcc9243cfcf0a878848723e1a185f60d8b5883c"
WHEEL_SHA = "fe923dbf5c7b27bbc1eb2bf58abfa793f8f13fd7ae8b27b1bc2de49920bcbd41"
PAK = "Apps/mlp1/RuffleProbe.pak/"
CORE = PAK + "emulators/ruffle-probe/"
APP = "applyCenter/.ruffle-flash/"
SCREEN_COLOR = "applyCenter/.screen-color/"


def verified(path, expected):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"Unexpected SHA-256 for {path}")
    return data


def member(archive, name, data, mode=0o644):
    info = ZipInfo(name)
    info.create_system = 3
    info.external_attr = (0o100000 | mode) << 16
    archive.writestr(info, data, compress_type=ZIP_DEFLATED)


def package_screen_color(leaf, binding, out):
    source = PROJECT / "aisos"
    for name in ("screen_color.py", "screen_color_ui.py"):
        member(out, SCREEN_COLOR + name, (source / name).read_bytes())
    installer = (source / "screen-color-install.sh").read_bytes().replace(b"\r\n", b"\n")
    member(out, SCREEN_COLOR + "install.sh", installer, 0o755)
    entry = (source / "Screen Color.app").read_bytes().replace(b"\r\n", b"\n")
    member(out, "applyCenter/Screen Color.app", entry, 0o755)
    member(out, "applyCenter/images/Screen Color.png", (source / "screen-color.png").read_bytes())
    member(out, SCREEN_COLOR + "panel-font.ttf", leaf.read(CORE + "res/menu-font.ttf"))
    member(out, SCREEN_COLOR + "pysdl2.whl", binding)
    license_name = "licenses/Source Han Sans OFL.txt"
    member(out, SCREEN_COLOR + license_name, (source / license_name).read_bytes())


def package(leaf_zip, wheel, output, screen_color=False):
    if output.resolve() in (leaf_zip.resolve(), wheel.resolve()):
        raise ValueError("Output must be distinct from its inputs")
    source = verified(leaf_zip, LEAF_SHA)
    binding = verified(wheel, WHEEL_SHA)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        staged = Path(tmp) / "aisos.zip"
        with ZipFile(BytesIO(source)) as leaf, ZipFile(staged, "w") as out:
            if screen_color:
                package_screen_color(leaf, binding, out)
            else:
                package_ruffle(leaf, binding, out)
        with ZipFile(staged) as archive:
            if failed := archive.testzip():
                raise ValueError(f"Bad ZIP member: {failed}")
        staged.replace(output)


def package_ruffle(leaf, binding, out):
    member(out, APP + "ruffle", leaf.read(CORE + "bin/ruffle"), 0o755)
    member(out, APP + "MiSans-Normal.ttf", leaf.read(CORE + "res/menu-font.ttf"))
    member(out, "applyCenter/images/Ruffle Flash.png", leaf.read(PAK + "art/FLASH.png"))
    member(out, APP + "pysdl2.whl", binding)
    member(out, APP + "launcher.py", (PROJECT / "aisos/launcher.py").read_bytes())
    entry = (PROJECT / "aisos/Ruffle Flash.app").read_bytes().replace(b"\r\n", b"\n")
    member(out, "applyCenter/Ruffle Flash.app", entry, 0o755)
    sample = "Bloxorz-翻滚方块.swf"
    member(out, "flash/" + sample, leaf.read("Roms/flash/" + sample))
    probe = BytesIO()
    with ZipFile(probe, "w") as game:
        member(game, "Leaf-GPU-Probe.swf", leaf.read("Roms/flash/Leaf-GPU-Probe.swf"))
    member(out, "flash/Leaf-GPU-Probe.zip", probe.getvalue())
    # Keep hundreds of small notices in one file on large-cluster SD cards.
    notices = BytesIO()
    with ZipFile(notices, "w") as licenses:
        for name in leaf.namelist():
            if name.startswith(PAK + "licenses/") and not name.endswith("/"):
                member(licenses, name[len(PAK):], leaf.read(name))
        for name in ("THIRD_PARTY.md", "provenance/sample-games.json", "build-manifest.json"):
            member(licenses, name, leaf.read(PAK + name))
    member(out, APP + "notices.zip", notices.getvalue())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("leaf_zip", type=Path)
    parser.add_argument("pysdl2_wheel", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--screen-color", action="store_true", help="Build only the Screen Color app")
    args = parser.parse_args()
    package(args.leaf_zip, args.pysdl2_wheel, args.output, args.screen_color)
