from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="relative path excluded from the ledger; may be repeated",
    )
    args = parser.parse_args()

    root = args.evidence_root.resolve()
    excluded = set(args.exclude)
    records: list[dict[str, object]] = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise SystemExit(f"symlink is not valid retained evidence: {path}")
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        if relative in excluded:
            continue
        payload = path.read_bytes()
        records.append(
            {
                "path": relative,
                "bytes": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
        )

    canonical_records = json.dumps(
        records, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    print(
        json.dumps(
            {
                "schema": "manyforge.e0.raw-evidence-ledger.v1",
                "root": str(root),
                "record_count": len(records),
                "records": records,
                "records_sha256": hashlib.sha256(canonical_records).hexdigest(),
            },
            indent=2,
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
