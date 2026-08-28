#!/usr/bin/env bash
set -euo pipefail

readonly version=1.28.2
readonly source_dir_name="mupdf-${version}-source"
readonly thirdparty_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../thirdparty" && pwd)"
readonly source_dir="${thirdparty_dir}/${source_dir_name}"
readonly archive_url="https://github.com/ArtifexSoftware/mupdf-downloads/releases/download/${version}/${source_dir_name}.tar.gz"

if [[ -e "${source_dir}" ]]; then
    if [[ -f "${source_dir}/Makefile" ]]; then
        echo "MuPDF source already exists at ${source_dir}"
        exit 0
    fi

    echo "Refusing to overwrite existing ${source_dir}" >&2
    exit 1
fi

work_dir="$(mktemp -d "${source_dir%/*}/.mupdf-download.XXXXXX")"
trap 'rm -rf "${work_dir}"' EXIT

archive_path="${work_dir}/${source_dir_name}.tar.gz"
echo "Downloading MuPDF ${version}..."
curl --fail --location --retry 3 --output "${archive_path}" "${archive_url}"

tar -xzf "${archive_path}" -C "${work_dir}"
if [[ ! -f "${work_dir}/${source_dir_name}/Makefile" ]]; then
    echo "Downloaded archive does not contain ${source_dir_name}" >&2
    exit 1
fi

mv "${work_dir}/${source_dir_name}" "${source_dir}"
echo "Extracted MuPDF source to ${source_dir}"
