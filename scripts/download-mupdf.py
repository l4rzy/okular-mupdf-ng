#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later
from __future__ import annotations

import hashlib
import os
import shutil
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

def _load_version_file() -> tuple[str, str]:
    p = (Path(__file__).resolve().parent / "../cmake/mupdf.version").resolve()
    lines = [ln.strip() for ln in p.read_text().splitlines() if ln.strip()]
    if len(lines) < 2 or not lines[0] or len(lines[1]) != 64:
        print(f"invalid {p}: expected 'version\\nsha256'", file=sys.stderr)
        sys.exit(1)
    return lines[0], lines[1].lower()


_MUPDF_DEFAULT_VERSION, _MUPDF_DEFAULT_SHA256 = _load_version_file()
MUPDF_VERSION = _MUPDF_DEFAULT_VERSION
MUPDF_SHA256 = _MUPDF_DEFAULT_SHA256

if sys.version_info < (3, 12):
    print("Python 3.12 or newer is required", file=sys.stderr)
    sys.exit(1)

version = os.environ.get("MUPDF_VERSION", MUPDF_VERSION).strip() or MUPDF_VERSION
source_name = f"mupdf-{version}-source"
url = f"https://github.com/ArtifexSoftware/mupdf-downloads/releases/download/{version}/{source_name}.tar.gz"

script_dir = Path(__file__).resolve().parent
thirdparty_dir = (script_dir / "../thirdparty").resolve()
source_dir = thirdparty_dir / source_name


def fail(msg: str) -> None:
    print(msg, file=sys.stderr)
    sys.exit(1)


if source_dir.exists():
    if (source_dir / "Makefile").is_file():
        print(f"MuPDF source already exists at {source_dir}")
        sys.exit(0)
    fail(f"Refusing to overwrite existing {source_dir}")

work_dir = Path(tempfile.mkdtemp(prefix=".mupdf-download.", dir=thirdparty_dir))
archive_path = work_dir / f"{source_name}.tar.gz"

try:
    print(f"Downloading MuPDF {version}...")
    last_error: Exception | None = None
    for attempt in range(1, 4):
        try:
            if archive_path.exists():
                archive_path.unlink()
            h = hashlib.sha256()
            with urllib.request.urlopen(url, timeout=30) as resp:
                if resp.status != 200:
                    raise urllib.error.HTTPError(url, resp.status, f"HTTP {resp.status}", resp.headers, None)
                with archive_path.open("wb") as out:
                    while True:
                        chunk = resp.read(8192)
                        if not chunk:
                            break
                        out.write(chunk)
                        h.update(chunk)
            if version == MUPDF_VERSION:
                actual = h.hexdigest()
                if actual.lower() != MUPDF_SHA256.lower():
                    raise ValueError(f"sha256 mismatch: expected {MUPDF_SHA256} got {actual}")
                else:
                    print("Hash OK")
            else:
                print(f"warning: MUPDF_VERSION={version} != {MUPDF_VERSION}, skipping sha256 check", file=sys.stderr)
            last_error = None
            break
        except Exception as e:  # noqa: BLE001
            last_error = e
            if archive_path.exists():
                try:
                    archive_path.unlink()
                except OSError:
                    pass
            if attempt == 3:
                break
            time.sleep(attempt)

    if last_error is not None:
        fail(f"download failed: {last_error}")

    with tarfile.open(archive_path, "r:gz") as tf:
        tf.extractall(path=work_dir, filter="data")

    if not (work_dir / source_name / "Makefile").is_file():
        fail(f"Downloaded archive does not contain {source_name}")

    shutil.move(str(work_dir / source_name), str(source_dir))
    print(f"Extracted MuPDF source to {source_dir}")

finally:
    shutil.rmtree(work_dir, ignore_errors=True)
