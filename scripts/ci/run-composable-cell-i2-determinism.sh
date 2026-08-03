#!/usr/bin/env bash
# Produce one clean set of real I2/I3 regression products for byte comparison.
set -euo pipefail
umask 022
export GIT_OPTIONAL_LOCKS=0
export LANG=C.UTF-8
export LC_ALL=C.UTF-8
export PYTHONNOUSERSITE=1
export PYTHONDONTWRITEBYTECODE=1
export TZ=UTC
unset PYTHONPATH PYTHONHOME 2>/dev/null || true

usage() {
  echo "usage: $0 --build-id <a|b> --artifact-root <path> --prepare-only" >&2
  exit 2
}

if [[ $# -ne 5 || "$1" != "--build-id" || \
      ( "$2" != "a" && "$2" != "b" ) || \
      "$3" != "--artifact-root" || "$5" != "--prepare-only" ]]; then
  usage
fi
BUILD_ID="$2"
ARTIFACT_ROOT="$4"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
if [[ ! "${PARALLEL}" =~ ^[1-9][0-9]*$ ]]; then
  echo "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer" >&2
  exit 2
fi
if [[ "${ARTIFACT_ROOT}" != /* || "${ARTIFACT_ROOT}" == "/" || \
      "$(realpath -m -- "${ARTIFACT_ROOT}")" != "${ARTIFACT_ROOT}" ]]; then
  echo "artifact root must be an absolute canonical non-root path" >&2
  exit 2
fi
if [[ "${ARTIFACT_ROOT}" == "${REPO_ROOT}" || \
      "${ARTIFACT_ROOT}" == "${REPO_ROOT}/"* ]]; then
  echo "artifact root must not modify the source snapshot" >&2
  exit 2
fi
if [[ "${ARTIFACT_ROOT##*/}" != "build-${BUILD_ID}" ]]; then
  echo "artifact root basename must match the independent build id" >&2
  exit 2
fi
if [[ -e "${ARTIFACT_ROOT}" || -L "${ARTIFACT_ROOT}" ]]; then
  echo "artifact root must be fresh: ${ARTIFACT_ROOT}" >&2
  exit 2
fi

mkdir -p "$(dirname "${ARTIFACT_ROOT}")"
mkdir "${ARTIFACT_ROOT}"
CONSUMER_BUILD="${ARTIFACT_ROOT}/consumer-build"
mkdir "${CONSUMER_BUILD}"

CMEEL_PREFIX="/usr/local/lib/python3.12/site-packages/cmeel.prefix"
if [[ ! -d "${CMEEL_PREFIX}" || -L "${CMEEL_PREFIX}" ]]; then
  echo "frozen cmeel dependency prefix is unavailable" >&2
  exit 2
fi
export CMAKE_PREFIX_PATH="/usr/local:${CMEEL_PREFIX}"
export LD_LIBRARY_PATH="/usr/local/lib:${CMEEL_PREFIX}/lib"
export BOOST_ROOT="${CMEEL_PREFIX}"
export BOOST_INCLUDEDIR="${CMEEL_PREFIX}/include"
export BOOST_LIBRARYDIR="${CMEEL_PREFIX}/lib"

/usr/bin/cmake \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS=60 \
  -S "${REPO_ROOT}/manyforge_roboplan" \
  -B "${CONSUMER_BUILD}"
/usr/bin/ninja -C "${CONSUMER_BUILD}" -j "${PARALLEL}" -d keeprsp \
  generate_canonical_empty_product_package \
  generate_lite6_retained_consumer_product

CANONICAL_SOURCE="${CONSUMER_BUILD}/canonical_empty_product_package"
LITE6_SOURCE="${CONSUMER_BUILD}/lite6_retained_consumer_fixture"
GENERATED_SOURCE="${CONSUMER_BUILD}/manyforge_deployment/generated"
for source in "${CANONICAL_SOURCE}" "${LITE6_SOURCE}" "${GENERATED_SOURCE}"; do
  if [[ ! -d "${source}" || -L "${source}" ]]; then
    echo "determinism build omitted an exact product directory: ${source}" >&2
    exit 1
  fi
done

mkdir \
  "${ARTIFACT_ROOT}/canonical_empty_product_package" \
  "${ARTIFACT_ROOT}/lite6_retained_consumer_fixture" \
  "${ARTIFACT_ROOT}/deployment-build" \
  "${ARTIFACT_ROOT}/deployment-build/generated"
/usr/bin/cp -R --preserve=mode "${CANONICAL_SOURCE}/." \
  "${ARTIFACT_ROOT}/canonical_empty_product_package/"
/usr/bin/cp -R --preserve=mode "${LITE6_SOURCE}/." \
  "${ARTIFACT_ROOT}/lite6_retained_consumer_fixture/"
/usr/bin/cp -R --preserve=mode "${GENERATED_SOURCE}/." \
  "${ARTIFACT_ROOT}/deployment-build/generated/"

/usr/bin/cmake -E remove_directory "${CONSUMER_BUILD}"
if [[ -e "${CONSUMER_BUILD}" || -L "${CONSUMER_BUILD}" ]]; then
  echo "temporary determinism build was not removed" >&2
  exit 1
fi
if [[ -n "$(find "${ARTIFACT_ROOT}" -type l -print -quit)" || \
      -n "$(find "${ARTIFACT_ROOT}" -type f -links +1 -print -quit)" ]]; then
  echo "determinism products contain an aliased file" >&2
  exit 1
fi
for required in \
  "${ARTIFACT_ROOT}/canonical_empty_product_package/canonical-empty-admission.json" \
  "${ARTIFACT_ROOT}/lite6_retained_consumer_fixture/manifest.json" \
  "${ARTIFACT_ROOT}/lite6_retained_consumer_fixture/retained-model.urdf" \
  "${ARTIFACT_ROOT}/lite6_retained_consumer_fixture/retained-model.srdf"; do
  if [[ ! -f "${required}" || ! -s "${required}" ]]; then
    echo "determinism product is missing or empty: ${required}" >&2
    exit 1
  fi
done
if [[ -z "$(find "${ARTIFACT_ROOT}/deployment-build/generated" \
    -mindepth 1 -maxdepth 1 -type f -name '*.hpp' -print -quit)" ]]; then
  echo "determinism build omitted deployment-generated headers" >&2
  exit 1
fi
