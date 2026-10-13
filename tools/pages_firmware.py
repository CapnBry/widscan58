#!/usr/bin/env python3
"""Stage PlatformIO firmware builds for the web flasher. GPL-3.0-or-later.

Copies each env's bootloader, partition table and app into <out>/<env>/ and
writes <out>/manifest.json in ESP Web Tools' manifest format, plus a sha256
per part that the web UI checks before flashing, and the commit time (the
one the firmware's INFO reports) as "date". Offsets come from the IDF
build's flasher_args.json (the bootloader is at 0x2000 on the C5, 0x0 on the S3).

    tools/pages_firmware.py --version "$(git describe --tags --always --dirty)" \\
        web/dist/firmware esp32c5 [esp32s3]

For `npm run dev`, stage into web/public/firmware instead.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "firmware" / ".pio" / "build"
# esptool-js reports these names (ROM CHIP_NAME); the web UI matches on them.
CHIP_FAMILY = {"esp32c5": "ESP32-C5", "esp32s3": "ESP32-S3"}
# flasher_args.json section -> file name in the PlatformIO build directory.
PARTS = {"bootloader": "bootloader.bin", "partition-table": "partitions.bin", "app": "firmware.bin"}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True)
    ap.add_argument("--date", help="commit time, ISO 8601 (default: HEAD's, as the firmware's INFO has it)")
    ap.add_argument("out", type=Path)
    ap.add_argument("envs", nargs="+", choices=sorted(CHIP_FAMILY))
    a = ap.parse_args()
    if a.date is None:
        a.date = subprocess.run(["git", "log", "-1", "--format=%cI"], cwd=ROOT, capture_output=True,
                                text=True).stdout.strip() or None

    a.out.mkdir(parents=True, exist_ok=True)
    builds = []
    for env in a.envs:
        build = BUILD / env
        args = json.loads((build / "flasher_args.json").read_text())
        (a.out / env).mkdir(exist_ok=True)
        parts = []
        for section, name in PARTS.items():
            data = (build / name).read_bytes()
            shutil.copyfile(build / name, a.out / env / name)
            parts.append({
                "path": f"{env}/{name}",
                "offset": int(args[section]["offset"], 16),
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            })
        builds.append({"chipFamily": CHIP_FAMILY[env], "parts": parts})
        print(f"{env}: " + ", ".join(f"{p['path']} @ {p['offset']:#x}" for p in parts))

    manifest = {"name": "Widscan58", "version": a.version, "date": a.date, "builds": builds}
    (a.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
