#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="$repo_dir/build"

if [[ ! -f "$build_dir/bin/okular_generators/okularGenerator_mupdfng.so" ||
      ! -x "$build_dir/bin/mupdfng-worker" ]]; then
    echo "Build the plugin and worker first: ./scripts/build.sh dev" >&2
    exit 1
fi

# Installed workers take precedence over PATH unless build-tree lookup is enabled.
if ! grep -qx 'ENABLE_BUILD_TREE_WORKER:BOOL=ON' "$build_dir/CMakeCache.txt"; then
    echo "Enable the build-tree worker and rebuild:" >&2
    echo "  cmake -B \"$build_dir\" -DENABLE_BUILD_TREE_WORKER=ON" >&2
    echo "  cmake --build \"$build_dir\"" >&2
    exit 1
fi

export QT_PLUGIN_PATH="$build_dir/bin${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
# Prevent Okular from forwarding documents to an existing, installed instance.
exec dbus-run-session -- okular "$@"
