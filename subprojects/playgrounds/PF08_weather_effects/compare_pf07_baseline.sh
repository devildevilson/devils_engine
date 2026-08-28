#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd "${script_dir}/../../.." && pwd)"
build_dir="${1:-${repository_root}/build-release}"
pf07="${build_dir}/subprojects/playgrounds/PF07_party_environment/bin/PF07_party_environment"
pf08="${build_dir}/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects"

for executable in "${pf07}" "${pf08}"; do
  if [[ ! -x "${executable}" ]]; then
    echo "Executable not found: ${executable}" >&2
    echo "Build PF07_party_environment and PF08_weather_effects first, or pass the build directory." >&2
    exit 1
  fi
done

temporary_dir="$(mktemp -d)"
trap 'rm -rf "${temporary_dir}"' EXIT

for preset in noon double_sunset night eclipse; do
  pf07_frame="${temporary_dir}/pf07_${preset}.ppm"
  pf08_frame="${temporary_dir}/pf08_${preset}.ppm"
  arguments=(--preset="${preset}" --frames=8 --width=1280 --height=720 --no-overlay)
  "${pf07}" "${arguments[@]}" --dump="${pf07_frame}"
  "${pf08}" "${arguments[@]}" --dump="${pf08_frame}"
  if ! cmp --silent "${pf07_frame}" "${pf08_frame}"; then
    echo "Baseline mismatch: ${preset}" >&2
    exit 1
  fi
  echo "MATCH ${preset}"
done

echo "PF08 clear baseline is pixel-identical to PF07."
