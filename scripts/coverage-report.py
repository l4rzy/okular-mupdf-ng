#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Generate LLVM/SonarQube coverage reports for the full project build."""

import json
import os
from pathlib import Path
import subprocess
import sys


def generate_reports(build_dir: Path) -> None:
    source_dir = Path(__file__).resolve().parent.parent
    report_dir = build_dir / "coverage"
    profiles = sorted((build_dir / "profiles").glob("*.profraw"))
    if not profiles:
        raise RuntimeError(f"No LLVM profiles found in {build_dir / 'profiles'}")
    llvm_cov = os.environ.get("LLVM_COV", "llvm-cov")
    llvm_profdata = os.environ.get("LLVM_PROFDATA", "llvm-profdata")
    llvm_readobj = os.environ.get("LLVM_READOBJ", "llvm-readobj")

    # Include every instrumented executable and plugin, including unexecuted
    # targets, so the report retains their uncovered functions.
    binaries = []
    for candidate in sorted((build_dir / "bin").rglob("*")):
        if not candidate.is_file():
            continue
        with candidate.open("rb") as binary:
            if binary.read(4) != b"\x7fELF":
                continue
        sections = subprocess.check_output([llvm_readobj, "--sections", str(candidate)], text=True)
        if "__llvm_covmap" in sections:
            binaries.append(candidate)
    if not binaries:
        raise RuntimeError(f"No instrumented binaries found in {build_dir / 'bin'}")

    report_dir.mkdir(parents=True, exist_ok=True)
    merged = report_dir / "coverage.profdata"
    subprocess.run([llvm_profdata, "merge", "-sparse", "-o", str(merged), "--", *map(str, profiles)], check=True)
    objects = [str(binaries[0])]
    for binary in binaries[1:]:
        objects.extend(["-object", str(binary)])
    objects.append(f"-instr-profile={merged}")
    # Pass source files explicitly to keep tests, generated files, system
    # headers, and vendor dependencies out of the coverage denominator.
    sources = [str(path) for path in sorted((source_dir / "src").rglob("*"))
               if path.suffix in {".cpp", ".hpp", ".c", ".h"}]
    sources = ["-sources", *sources]
    with (report_dir / "llvm-cov.txt").open("w") as report:
        subprocess.run([llvm_cov, "show", *objects, "-show-branches=count",
                        "-use-color=false", *sources], stdout=report, check=True)
    with (report_dir / "summary.txt").open("w") as summary:
        subprocess.run([llvm_cov, "report", *objects, *sources], stdout=summary, check=True)
    subprocess.run([llvm_cov, "show", *objects, "-show-branches=count", "-format=html",
                    f"-output-dir={report_dir / 'html'}", *sources], check=True)
    # Guard against silently reporting only in-process test coverage: both the
    # actual worker entry point and dynamically loaded generator must execute.
    summary = json.loads(subprocess.check_output(
        [llvm_cov, "export", *objects, "-summary-only", *sources], text=True))
    covered = {Path(file["filename"]).resolve(): file["summary"]["lines"]["covered"]
               for data in summary["data"] for file in data["files"]}
    for required in ["src/worker/main.cpp", "src/generator/mupdfng.cpp"]:
        if covered.get(source_dir / required, 0) == 0:
            raise RuntimeError(f"Missing executed coverage for {required}")
    print(f"Merged {len(profiles)} profiles from {len(binaries)} binaries")
    print((report_dir / "summary.txt").read_text())


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: coverage-report.py BUILD_DIR")
    try:
        generate_reports(Path(sys.argv[1]).resolve())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f"Coverage report failed: {error}")
