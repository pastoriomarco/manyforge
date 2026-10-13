#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "Usage: $0 ABSOLUTE_EVIDENCE_DIRECTORY [comma-separated cases] [sha256:IMAGE_ID]" >&2
  exit 2
fi
evidence=$1
if [[ $evidence != /* || -e $evidence ]]; then
  echo "Evidence directory must be an absolute, new path" >&2
  exit 2
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
image=${3:-${MANYFORGE_DEV_IMAGE:-}}
if [[ ! "$image" =~ ^sha256:[0-9a-f]{64}$ ]]; then
  echo 'Supply an immutable image ID or the launcher MANYFORGE_DEV_IMAGE.' >&2
  exit 2
fi
mkdir -p "$evidence"
docker image inspect "$image" > "$evidence/docker-image.json"
git -C "$script_dir" rev-parse HEAD > "$evidence/source-base.txt"
sha256sum "$script_dir/jtc_characterization.py" "$script_dir/run_jtc_characterization.sh" > "$evidence/harness-sha256.txt"
extra=()
if [[ $# -ge 2 && -n "$2" ]]; then extra=(--cases "$2"); fi
docker run --rm --name "manyforge-jtc-characterization-$$" --network none \
  --read-only --tmpfs /tmp:rw,nosuid,nodev --cap-drop ALL \
  --cpus 2 --memory 2g --user "$(id -u):$(id -g)" \
  -e ROS_DOMAIN_ID=187 -e ROS_LOCALHOST_ONLY=1 \
  -e ROS_LOG_DIR=/tmp/ros-logs -e PYTHONDONTWRITEBYTECODE=1 \
  -v "$script_dir:/probe:ro" -v "$evidence:/out:rw" \
  --entrypoint /bin/bash "$image" \
  -c 'source "/opt/ros/${ROS_DISTRO}/setup.bash"; exec python3 /probe/jtc_characterization.py --output /out "$@"' \
  probe "${extra[@]}" 2>&1 | tee "$evidence/run.log"
