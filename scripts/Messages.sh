#!/bin/sh
# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

podir=${podir:-po}
domain=${domain:-okular_mupdfng}
extractrc=${EXTRACTRC:-extractrc}
rc_file=rc.cpp

cleanup()
{
    rm -f "$rc_file"
}
trap cleanup EXIT HUP INT TERM

# Qt Designer XML is not understood by xgettext. KDE's extractrc converts
# translatable <string> values into C++ translation calls first.
ui_files=$(find src -type f \( -name '*.ui' -o -name '*.rc' \))
if [ -n "$ui_files" ]; then
    if command -v "$extractrc" >/dev/null 2>&1; then
        "$extractrc" $ui_files > "$rc_file"
    elif command -v xmllint >/dev/null 2>&1 && command -v perl >/dev/null 2>&1; then
        # Some development installations provide xgettext but not KDE's
        # extractrc. Keep UI extraction functional for the simple Qt Designer
        # strings used here; extractrc remains preferred when available.
        rc_files=$(find src -type f -name '*.rc')
        if [ -n "$rc_files" ]; then
            echo "extractrc is required to extract .rc files" >&2
            exit 1
        fi
        for ui_file in $ui_files; do
            count=$(xmllint --xpath 'count(//string[not(@notr="true")])' "$ui_file")
            count=${count%.*}
            index=1
            while [ "$index" -le "$count" ]; do
                value=$(xmllint --xpath "string((//string[not(@notr=\"true\")])[$index])" "$ui_file")
                escaped=$(printf '%s' "$value" | perl -0pe 's/\\/\\\\/g; s/"/\\"/g; s/\r/\\r/g; s/\n/\\n/g')
                printf 'i18n("%s");\n' "$escaped"
                index=$((index + 1))
            done
        done > "$rc_file"
    else
        echo "extractrc is unavailable and the fallback requires xmllint and perl" >&2
        exit 1
    fi
else
    : > "$rc_file"
fi

source_files=$(find src -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \))
mkdir -p "$podir"
if [ -n "${XGETTEXT:-}" ]; then
    # KDE's build environment supplies XGETTEXT with its standard keyword
    # configuration and may include additional command-line options.
    $XGETTEXT $source_files "$rc_file" -o "$podir/$domain.pot"
else
    xgettext \
        --from-code=UTF-8 \
        --keyword=i18n \
        --keyword='i18nc:1c,2' \
        --keyword='i18np:1,2' \
        --keyword='i18ncp:1c,2' \
        $source_files "$rc_file" -o "$podir/$domain.pot"
fi
