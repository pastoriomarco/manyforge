#!/usr/bin/env bash
# Run the read-only provider check, then the confirmed Draft-only scene edit.

set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON:-python3}" "$SCRIPT_DIR/smoke-assistant-e2e.py" "$@"
