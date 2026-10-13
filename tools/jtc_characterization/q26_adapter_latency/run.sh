#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 3 || $# -gt 4 ]]; then
 echo 'Usage: bash run.sh ABSOLUTE_NEW_EVIDENCE_DIR ABSOLUTE_SOURCE_ROOT ABSOLUTE_RUNTIME_BUILD [sha256:IMAGE_ID]' >&2
 exit 2
fi
output=$1
source_root=$2
build_root=$3
image=${4:-${MANYFORGE_DEV_IMAGE:-}}
if [[ "$output" != /* || -e "$output" || "$source_root" != /* || "$build_root" != /* ||
      ! -f "$source_root/manyforge_core/CMakeLists.txt" || ! -f "$build_root/CMakeCache.txt" ||
      ! "$image" =~ ^sha256:[0-9a-f]{64}$ ]]; then
 echo 'Use a new absolute output, current checkout, existing ROS runtime build, and immutable image ID (or MANYFORGE_DEV_IMAGE).' >&2
 exit 2
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
name="manyforge-q26-real-adapter-$$"
cleanup() { docker stop --time 5 "$name" >/dev/null 2>&1 || true; docker rm "$name" >/dev/null 2>&1 || true; }
trap cleanup EXIT
mkdir -p "$output"
docker image inspect "$image" > "$output/image.json"
cp "$build_root/CMakeCache.txt" "$output/runtime-build-cache.txt"
git -C "$source_root" rev-parse HEAD > "$output/source-base.txt"
docker run --rm --name "$name" --init --network none --read-only \
 --tmpfs /tmp:rw,nosuid,nodev --cap-drop ALL --cpus 4 --memory 6g \
 --user "$(id -u):$(id -g)" -e ROS_DOMAIN_ID=197 -e ROS_LOCALHOST_ONLY=1 \
 -e ROS_LOG_DIR=/out/ros-logs -e PYTHONDONTWRITEBYTECODE=1 \
 -e CCACHE_DISABLE=1 -e PYTHONPATH=/support \
 -v "$script_dir:/probe:ro" -v "$source_root:/workspace:ro" \
 -v "$build_root:/runtime-build:ro" -v "$script_dir/..:/support:ro" \
 -v "$output:/out:rw" --entrypoint /bin/bash "$image" -c '
set -eo pipefail
source "/opt/ros/${ROS_DISTRO}/setup.bash"
python3 --version > /out/python-version.txt
dpkg-query -W "ros-${ROS_DISTRO}-joint-trajectory-controller" "ros-${ROS_DISTRO}-controller-manager" "ros-${ROS_DISTRO}-hardware-interface" "ros-${ROS_DISTRO}-rclcpp" "ros-${ROS_DISTRO}-rclpy" > /out/runtime-packages.txt
cmake -S /probe -B /out/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER= -DMANYFORGE_SOURCE_ROOT=/workspace -DMANYFORGE_ROBOPLAN_BUILD_DIR=/runtime-build > /out/configure.log 2>&1
cmake --build /out/build --target q26_adapter_probe --parallel 2 > /out/build.log 2>&1
sha256sum /out/build/q26_adapter_probe > /out/probe-binary.sha256
ldd /out/build/q26_adapter_probe > /out/probe-ldd.txt
python3 /probe/fixture.py --out /out --binary /out/build/q26_adapter_probe --compile-commands /out/build/compile_commands.json
python3 /probe/analyze.py /out
' 2>&1 | tee "$output/run.log"
