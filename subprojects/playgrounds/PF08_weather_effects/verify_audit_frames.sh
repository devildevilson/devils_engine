#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd "${script_dir}/../../.." && pwd)"
build_dir="${1:-${repository_root}/build-release}"
pf08="${build_dir}/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects"
reference_dir="${script_dir}/audit_frames"

if [[ ! -x "${pf08}" ]]; then
  echo "PF08 executable not found: ${pf08}" >&2
  echo "Build PF08_weather_effects first, or pass the build directory." >&2
  exit 1
fi
if ! command -v magick >/dev/null 2>&1; then
  echo "ImageMagick 'magick' is required for pixel comparison." >&2
  exit 1
fi

temporary_dir="$(mktemp -d)"
trap 'rm -rf "${temporary_dir}"' EXIT

verify_frame() {
  local name="$1"
  shift
  local current_frame="${temporary_dir}/${name}.ppm"
  local reference_frame="${reference_dir}/${name}.png"
  local metric="${temporary_dir}/${name}.metric"

  if [[ ! -f "${reference_frame}" ]]; then
    echo "Frozen PF08 audit frame not found: ${reference_frame}" >&2
    exit 1
  fi

  "${pf08}" --frames=80 --width=1280 --height=720 --no-overlay "$@" --dump="${current_frame}"
  if ! magick compare -metric AE "${reference_frame}" "${current_frame}" null: 2>"${metric}"; then
    echo "AUDIT MISMATCH ${name}: $(<"${metric}") pixels differ" >&2
    exit 1
  fi
  echo "AUDIT MATCH ${name}"
}

verify_frame clear_noon --preset=noon --weather=clear
verify_frame clear_sunset --preset=double_sunset --weather=clear
verify_frame clear_night --preset=night --weather=clear
verify_frame overcast_noon --preset=noon --weather=overcast
verify_frame rain_sunset --preset=double_sunset --weather=rain --surface-age=1
verify_frame snow_sunset --preset=snow_glint --weather=snow --surface-age=30
verify_frame lightning_magic --lightning=magic --lightning-phase=0.03
verify_frame aurora_night --preset=aurora --weather=clear

echo "PF08 closing-audit gallery is pixel-identical to its frozen frames."
