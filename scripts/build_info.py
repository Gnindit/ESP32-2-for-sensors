"""Embed a public version only; Wi-Fi credentials are provisioned through USB."""
import json
import os
from pathlib import Path
import subprocess

Import("env")

version = os.environ.get("RELEASE_TAG", "")
if not version:
    result = subprocess.run(
        ["git", "describe", "--tags", "--always", "--dirty"],
        cwd=env.subst("$PROJECT_DIR"), capture_output=True, text=True,
    )
    version = result.stdout.strip() if result.returncode == 0 else "development"
build = Path(env.subst("$BUILD_DIR"))
build.mkdir(parents=True, exist_ok=True)
header = build / "build_info.h"
content = "#pragma once\n#define FIRMWARE_VERSION " + json.dumps(version) + "\n"
if not header.exists() or header.read_text() != content:
    header.write_text(content)
env.Append(CPPPATH=[str(build)])
