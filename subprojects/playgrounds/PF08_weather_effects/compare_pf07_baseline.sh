#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd "${script_dir}/../../.." && pwd)"
build_dir="${1:-${repository_root}/build-release}"
pf08="${build_dir}/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects"
reference_dir="${repository_root}/subprojects/playgrounds/PF07_party_environment/reference_frames"

if [[ ! -x "${pf08}" ]]; then
  echo "PF08 executable not found: ${pf08}" >&2
  echo "Build PF08_weather_effects first, or pass the build directory." >&2
  exit 1
fi
if ! command -v magick >/dev/null 2>&1; then
  echo "ImageMagick 'magick' is required for pixel comparison with the frozen PNG references." >&2
  exit 1
fi

temporary_dir="$(mktemp -d)"
trap 'rm -rf "${temporary_dir}"' EXIT

for preset in noon double_sunset night eclipse; do
  pf08_frame="${temporary_dir}/pf08_${preset}.ppm"
  reference_frame="${reference_dir}/${preset}.png"
  if [[ ! -f "${reference_frame}" ]]; then
    echo "Frozen PF07 reference not found: ${reference_frame}" >&2
    exit 1
  fi
  arguments=(--preset="${preset}" --frames=8 --width=1280 --height=720 --no-overlay)
  "${pf08}" "${arguments[@]}" --dump="${pf08_frame}"
  metric="${temporary_dir}/${preset}.metric"
  if ! magick compare -metric AE "${reference_frame}" "${pf08_frame}" null: 2>"${metric}"; then
    pixels="$(<"${metric}")"
    echo "Baseline mismatch: ${preset} (${pixels} pixels differ)" >&2
    exit 1
  fi
  echo "MATCH ${preset}"
done

echo "PF08 clear baseline is pixel-identical to the frozen PF07 reference frames."
