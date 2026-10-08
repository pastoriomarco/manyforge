#!/usr/bin/env bash
# Persistent ROS-free diagnostic builds. VARIANT names the exact package graph,
# build type, sanitizer and CMake options; use a different name when these change.
# Source is read-only. Commands must write generated output under /runner.
# This is not a clean qualification entrypoint.
set -euo pipefail
if (( $# < 3 )) || [[ "$2" != -- || ! "$1" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]]; then
    echo "Usage: $0 VARIANT -- COMMAND [ARGS...]" >&2
    exit 64
fi
variant=$1
shift 2
script_repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
repo_root=$(cd "${MANYFORGE_DIAGNOSTIC_SOURCE:-$script_repo_root}" && pwd -P)
image=${MANYFORGE_DIAGNOSTIC_IMAGE:-manyforge-dev-cache:local}
cache_base=${MANYFORGE_DIAGNOSTIC_CACHE_BASE:-"${XDG_CACHE_HOME:-$HOME/.cache}/manyforge/diagnostic"}
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
generator=${CMAKE_GENERATOR:-Ninja}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "Build jobs must be a positive integer" >&2; exit 64; }
[[ "$generator" == Ninja ]] || { echo "Diagnostic builds require Ninja" >&2; exit 64; }
image_id=$(docker image inspect --format '{{.Id}}' "$image")
architecture=$(docker image inspect --format '{{.Architecture}}' "$image_id")
# The image is immutable; fix the compiler selection and fingerprint its actual
# binaries. Reject an image without the declared cache tool before creating state.
toolchain=$(docker run --rm --network none --entrypoint /bin/sh "$image_id" -c \
    'set -eu; ccache --version; sha256sum /usr/bin/gcc /usr/bin/g++; gcc -dumpmachine')
# Ninja decides staleness by timestamps, so a tree must never be shared by two
# source checkouts: include the resolved source root in the tree identity.
configuration=$(printf '%s\n' "$variant" "SOURCE=$repo_root" "$generator" \
    "CFLAGS=${CFLAGS:-}" "CXXFLAGS=${CXXFLAGS:-}" "LDFLAGS=${LDFLAGS:-}" "$toolchain")
config_key=$(printf '%s' "$configuration" | sha256sum)
config_key=${config_key%% *}
cache_root="$cache_base/$architecture/${image_id#sha256:}/$variant-$config_key"
mkdir -p "$cache_root" "$cache_root/build" "$cache_root/ccache" "$cache_root/home"
cache_root=$(cd "$cache_root" && pwd)
for directory in "$cache_root" "$cache_root/build" "$cache_root/ccache" "$cache_root/home"; do
    [[ -w "$directory" ]] || { echo "Diagnostic directory is not writable: $directory" >&2; exit 69; }
done
# Fail instead of running competing processes against one CMake/Ninja tree.
exec {lock_fd}>"$cache_root/build.lock"
flock --nonblock "$lock_fd" || { echo "Diagnostic tree is in use: $cache_root" >&2; exit 75; }
printf '%s\n' "$configuration" > "$cache_root/configuration.txt"
echo "Diagnostic image: $image_id ($architecture)"
echo "Diagnostic tree: $cache_root/build"
echo "Variant: $variant; generator: $generator; jobs: $jobs"
git_mounts=()
if [[ -f "$repo_root/.git" ]]; then
    # A linked worktree's .git file references host paths. Preserve that
    # read-only metadata so provenance checks see the real commit, not unknown.
    git_common=$(git -C "$repo_root" rev-parse --path-format=absolute --git-common-dir)
    git_mounts+=(--mount "type=bind,source=$git_common,target=$git_common,readonly")
fi
container_id=$(docker create --init --network none \
    --user "$(id -u):$(id -g)" \
    --mount "type=bind,source=$repo_root,target=/workspace,readonly" \
    --mount "type=bind,source=$cache_root,target=/runner" \
    "${git_mounts[@]}" \
    --workdir /workspace \
    --env HOME=/runner/home --env WORKSPACE=/workspace \
    --env MANYFORGE_BUILD_DIR=/runner/build --env BUILD_DIR=/runner/build --env BUILD_ROOT=/runner/build \
    --env PYTHONPYCACHEPREFIX=/runner/home/pycache \
    --env CC=/usr/bin/gcc --env CXX=/usr/bin/g++ \
    --env "CFLAGS=${CFLAGS:-}" --env "CXXFLAGS=${CXXFLAGS:-}" --env "LDFLAGS=${LDFLAGS:-}" \
    --env CMAKE_GENERATOR=Ninja --env "CMAKE_BUILD_PARALLEL_LEVEL=$jobs" \
    --env CMAKE_C_COMPILER_LAUNCHER=/usr/bin/ccache --env CMAKE_CXX_COMPILER_LAUNCHER=/usr/bin/ccache \
    --env CCACHE_DIR=/runner/ccache --env CCACHE_MAXSIZE=5G --env CCACHE_COMPILERCHECK=content \
    "$image_id" env -u CCACHE_DISABLE bash -c '
        set +e
        "$@"
        result=$?
        ccache --show-stats
        exit "$result"
    ' diagnostic-build "$@")
cleanup_container() {
    # A client/attach failure may leave the command running. Remove our exact
    # container before releasing the build-tree lock, even on that error path.
    if ! docker rm --force "$container_id" >/dev/null; then
        echo "Could not remove diagnostic container: $container_id" >&2
        exit 69
    fi
}
trap cleanup_container EXIT
stop_on_signal() {
    trap '' INT TERM
    # Only this exact disposable diagnostic container is ours to stop.
    if ! docker stop --time 10 "$container_id" >/dev/null; then
        echo "Could not stop diagnostic container: $container_id; waiting for its command" >&2
    fi
    wait "$docker_pid" 2>/dev/null || true
    exit "$1"
}
docker_pid=
trap 'stop_on_signal 130' INT
trap 'stop_on_signal 143' TERM
docker start --attach "$container_id" &
docker_pid=$!
result=0
wait "$docker_pid" || result=$?
trap - INT TERM
exit "$result"
