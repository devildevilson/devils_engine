#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd "${script_dir}/../../.." && pwd)"
build_dir="${1:-${repository_root}/build-release}"
executable="${build_dir}/subprojects/playgrounds/PF07_party_environment/bin/PF07_party_environment"
output_dir="${script_dir}/reference_frames"

if [[ ! -x "${executable}" ]]; then
  echo "PF07 executable not found: ${executable}" >&2
  echo "Build it first or pass the build directory as the first argument." >&2
  exit 1
fi
if ! command -v magick >/dev/null 2>&1; then
  echo "ImageMagick 'magick' is required to convert deterministic PPM dumps to PNG." >&2
  exit 1
fi

mkdir -p "${output_dir}"
for preset in noon double_sunset night eclipse; do
  ppm="${output_dir}/${preset}.ppm"
  png="${output_dir}/${preset}.png"
  "${executable}" --preset="${preset}" --frames=8 --width=1280 --height=720 --no-overlay --dump="${ppm}"
  # `-strip` убирает timestamps ImageMagick: иначе пиксели совпадают, а SHA-256 файла меняется.
  magick "${ppm}" -strip -define png:compression-level=9 "${png}"
  rm -f "${ppm}"
done

echo "PF07 reference frames written to ${output_dir}"
