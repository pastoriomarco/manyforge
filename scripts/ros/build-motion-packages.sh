#!/usr/bin/env bash
# Package-only Jazzy experiment build. Run through diagnostic-build.sh with a
# variant naming the selected packages, build type and BUILD_TESTING setting.
# The exact image owns the ROS underlay; no external overlays are discovered.
set -euo pipefail

if (( $# == 0 )); then
    echo "Usage: $0 PACKAGE [PACKAGE...] (manyforge_isaac_hardware, manyforge_motion_controller)" >&2
    exit 64
fi
workspace=${WORKSPACE:-/workspace}
build_root=${MANYFORGE_BUILD_DIR:?Run through the canonical diagnostic-build.sh launcher}/motion-ros-overlay
build_type=${MANYFORGE_MOTION_BUILD_TYPE:-Release}
build_testing=${MANYFORGE_MOTION_BUILD_TESTING:-ON}
[[ ${ROS_DISTRO:-jazzy} == jazzy ]] || { echo "Motion package diagnostics require Jazzy" >&2; exit 64; }
case "$build_type" in Release|RelWithDebInfo|Debug) ;; *) echo "Invalid motion build type: $build_type" >&2; exit 64 ;; esac
case "$build_testing" in ON|OFF) ;; *) echo "BUILD_TESTING must be ON or OFF" >&2; exit 64 ;; esac
[[ ${CMAKE_GENERATOR:-Ninja} == Ninja ]] || { echo "Motion package diagnostics require Ninja" >&2; exit 64; }
[[ ${CMAKE_BUILD_PARALLEL_LEVEL:-} =~ ^[1-9][0-9]*$ ]] || { echo "Build jobs must be a positive integer" >&2; exit 64; }
[[ ${CMAKE_C_COMPILER_LAUNCHER:-} == /usr/bin/ccache && ${CMAKE_CXX_COMPILER_LAUNCHER:-} == /usr/bin/ccache ]] || {
    echo "Run through the canonical diagnostic launcher with ccache enabled" >&2; exit 64;
}
[[ -f /opt/ros/jazzy/setup.bash ]] || { echo "Jazzy underlay is missing" >&2; exit 69; }

packages=()
base_paths=()
for package in "$@"; do
    case "$package" in
        manyforge_isaac_hardware|manyforge_motion_controller) ;;
        *) echo "Unsupported motion package: $package" >&2; exit 64 ;;
    esac
    for selected in "${packages[@]}"; do
        [[ "$selected" != "$package" ]] || { echo "Duplicate motion package: $package" >&2; exit 64; }
    done
    for file in CMakeLists.txt package.xml; do
        [[ -f "$workspace/$package/$file" ]] || { echo "Motion package source is incomplete: $workspace/$package/$file" >&2; exit 69; }
    done
    packages+=("$package")
    base_paths+=("$workspace/$package")
done
mkdir -p "$build_root"
[[ -w "$build_root" ]] || { echo "Motion build root is not writable: $build_root" >&2; exit 69; }

# Do not inherit cmeel or another workspace's ABI/dependency discovery paths.
unset AMENT_PREFIX_PATH CMAKE_PREFIX_PATH COLCON_PREFIX_PATH ROS_PACKAGE_PATH
unset PYTHONPATH LD_LIBRARY_PATH BOOST_ROOT BOOST_INCLUDEDIR BOOST_LIBRARYDIR
set +u
# shellcheck disable=SC1091
source /opt/ros/jazzy/setup.bash
set -u
# This graph is explicit. User defaults must not silently skip selected
# packages while an earlier install tree makes the build appear successful.
export COLCON_DEFAULTS_FILE=/dev/null

printf 'Motion ROS packages: %s\n' "${packages[*]}"
printf 'Motion ROS overlay: %s (Jazzy; %s; BUILD_TESTING=%s)\n' "$build_root" "$build_type" "$build_testing"
# A renamed or nested package must not silently turn an explicit selection into
# a green build of zero packages, or enlarge the selected source graph.
discovered=$(colcon --log-base "$build_root/log" list --base-paths "${base_paths[@]}" --names-only | LC_ALL=C sort)
expected=$(printf '%s\n' "${packages[@]}" | LC_ALL=C sort)
[[ "$discovered" == "$expected" ]] || {
    printf 'Motion package inventory mismatch: expected [%s], found [%s]\n' "$expected" "$discovered" >&2
    exit 69
}
colcon --log-base "$build_root/log" build \
    --base-paths "${base_paths[@]}" \
    --build-base "$build_root/build" \
    --install-base "$build_root/install" \
    --executor sequential \
    --packages-select "${packages[@]}" \
    --cmake-args \
        -G Ninja \
        "-DCMAKE_BUILD_TYPE=$build_type" \
        "-DBUILD_TESTING=$build_testing" \
        -DCMAKE_C_COMPILER_LAUNCHER=/usr/bin/ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=/usr/bin/ccache

test -f "$build_root/install/setup.bash"
printf 'Motion ROS package build ready: %s/install\n' "$build_root"
