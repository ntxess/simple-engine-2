#!/usr/bin/env bash
set -euo pipefail

# Build/stage Boost libraries from the vendored Boost tree.
#
# Usage:
#   ./scripts/build-boost.sh            # debug
#   ./scripts/build-boost.sh debug
#   ./scripts/build-boost.sh release
#
# Output layout (matches CMakeLists.txt):
#   dep/boost_1_90_0/stage/<CMAKE_SYSTEM_NAME>/<debug|release>/lib
#
# Notes:
#   - Requires a C++ toolchain + b2 prerequisites (python is NOT required).
#   - On Debian/Ubuntu you may need: build-essential gcc g++ (and optionally clang).

variant="${1:-debug}"
case "${variant}" in
  debug|release) ;;
  *)
    echo "usage: $0 [debug|release]" >&2
    exit 2
    ;;
esac

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
boost_dir="${root}/dep/boost_1_90_0"

if [[ ! -d "${boost_dir}" ]]; then
  echo "boost directory not found: ${boost_dir}" >&2
  exit 2
fi

sys_name="$(uname -s)" # Linux / Darwin
stage_dir="${boost_dir}/stage/${sys_name}/${variant}"

jobs="1"
if command -v nproc >/dev/null 2>&1; then
  jobs="$(nproc)"
elif [[ "${sys_name}" == "Darwin" ]] && command -v sysctl >/dev/null 2>&1; then
  jobs="$(sysctl -n hw.ncpu)"
fi

cd "${boost_dir}"

if [[ ! -x "./b2" ]]; then
  echo "bootstrapping boost build (./bootstrap.sh) ..."
  ./bootstrap.sh
fi

mkdir -p "${stage_dir}"

echo "staging boost (${variant}) to: ${stage_dir}"
./b2 "-j${jobs}" \
  variant="${variant}" \
  link=static \
  threading=multi \
  runtime-link=shared \
  --with-log \
  --with-log_setup \
  --with-filesystem \
  --with-system \
  --with-thread \
  stage \
  --stagedir="${stage_dir}"

echo "done."

