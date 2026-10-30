#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create the OBS Rust dependency archive under src/worker/engine/kindle/."""

import argparse
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile


def createVendorArchive(root: Path, output: Path, offline: bool = False) -> None:
    manifest_directory = Path("src/worker/engine/kindle")
    manifest = root / manifest_directory / "Cargo.toml"
    # Stage beside the output so replacing an existing archive is atomic.
    with tempfile.TemporaryDirectory(prefix=".vendor-boko-", dir=output.parent) as temporary:
        staging = Path(temporary)
        command = ["cargo", "vendor", "--locked", "--versioned-dirs",
                   "--manifest-path", str(manifest)]
        if offline:
            command.append("--offline")
        # Cargo resolves "vendor" relative to the directory containing .cargo,
        # so keep both together under the bridge's manifest directory.
        command.append("vendor")
        result = subprocess.run(command, cwd=staging, check=True,
                                stdout=subprocess.PIPE, text=True)
        config = staging / ".cargo/config.toml"
        config.parent.mkdir()
        config.write_text(result.stdout, encoding="utf-8")
        archive = staging / "vendor.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(staging / "vendor", arcname=str(manifest_directory / "vendor"))
            tar.add(config, arcname=str(manifest_directory / ".cargo/config.toml"))
        archive.replace(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=Path("vendor.tar.gz"),
                        help="output archive (default: vendor.tar.gz in the current directory)")
    parser.add_argument("--offline", action="store_true",
                        help="use only dependencies already cached by Cargo")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    try:
        createVendorArchive(root, args.output.resolve(), args.offline)
    except (OSError, subprocess.CalledProcessError, tarfile.TarError) as error:
        print(f"Failed to create vendor archive: {error}", file=sys.stderr)
        return 1
    print(f"Created {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
