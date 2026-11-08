#!/usr/bin/env python3
"""Build the Lyrion ZIP and repository manifest for an immutable release."""
import argparse
import hashlib
from pathlib import Path
import re
import xml.etree.ElementTree as ET
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

parser = argparse.ArgumentParser()
parser.add_argument("binary", type=Path)
parser.add_argument("release_url", help="Release asset directory, without the filename")
parser.add_argument("--output", type=Path, default=Path("dist"))
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
manifest = ET.parse(root / "plugin" / "install.xml")
version = manifest.findtext("version")
if not re.fullmatch(r"\d+\.\d+\.\d+", version or ""):
    raise SystemExit("Invalid install.xml version")
if not args.binary.is_file():
    raise SystemExit("Missing DSP executable")
args.output.mkdir(parents=True, exist_ok=True)
archive = args.output / f"NatAmbioAlpha-{version}.zip"

files = {
    str(path.relative_to(root / "plugin")): path
    for path in (root / "plugin").rglob("*")
    if path.is_file() and not path.name.startswith(".")
}
files["LICENSE"] = root / "LICENSE"
files["Bin/natambio-alpha"] = args.binary
with ZipFile(archive, "w", compression=ZIP_DEFLATED) as package:
    for name, path in sorted(files.items()):
        info = ZipInfo(f"NatAmbioAlpha/{name}")
        info.create_system = 3
        info.external_attr = (0o100755 if name.startswith("Bin/") else 0o100644) << 16
        package.writestr(info, path.read_bytes(), compress_type=ZIP_DEFLATED)

extensions = ET.Element("extensions")
details = ET.SubElement(extensions, "details")
ET.SubElement(details, "title", lang="EN").text = "NatAmbio Alpha for Lyrion"
plugins = ET.SubElement(extensions, "plugins")
plugin = ET.SubElement(plugins, "plugin", name="NatAmbioAlpha", version=version,
                       minTarget=manifest.findtext("targetApplication/minVersion"),
                       maxTarget=manifest.findtext("targetApplication/maxVersion"))
ET.SubElement(plugin, "title", lang="EN").text = "NatAmbio Alpha"
ET.SubElement(plugin, "desc", lang="EN").text = "NatAmbio stereo DSP for Lyrion 9 on Linux x86-64"
ET.SubElement(plugin, "url").text = f"{args.release_url.rstrip('/')}/{archive.name}"
ET.SubElement(plugin, "sha").text = hashlib.sha1(archive.read_bytes()).hexdigest()
ET.indent(extensions)
ET.ElementTree(extensions).write(args.output / "repo.xml", encoding="utf-8", xml_declaration=True)
