"""Package both built variants; never connect to or flash a device."""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    packages = core / "packages"
    esptool = packages / "tool-esptoolpy" / "esptool.py"
    boot_app = packages / "framework-arduinoespressif32/tools/partitions/boot_app0.bin"
    dist = root / "dist"
    dist.mkdir(exist_ok=True)
    outputs = []
    for variant, environment in (
        ("simulation", "esp32-s3-2"),
        ("hardware", "esp32-s3-2-hardware"),
    ):
        build = root / ".pio/build" / environment
        app = dist / f"esp32-2-{variant}.bin"
        factory = dist / f"esp32-2-{variant}-factory.bin"
        shutil.copyfile(build / "firmware.bin", app)
        subprocess.run(
            [sys.executable, str(esptool), "--chip", "esp32s3", "merge_bin",
             "-o", str(factory), "--flash_mode", "dio", "--flash_freq", "80m",
             "--flash_size", "16MB",
             "0x0", str(build / "bootloader.bin"),
             "0x8000", str(build / "partitions.bin"),
             "0xe000", str(boot_app), "0x10000", str(app)],
            check=True,
        )
        outputs.extend([app, factory])
    shutil.copyfile(root / "docs/FLASHING.md", dist / "FLASHING.md")
    outputs.append(dist / "FLASHING.md")
    (dist / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
        for path in outputs
    ))


if __name__ == "__main__":
    main()
