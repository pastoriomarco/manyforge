#!/usr/bin/env python3
"""Build a versioned ManyForge OpenClaw plugin artifact.

This wraps generate-manyforge-openclaw-plugin.py and produces:

- a staged plugin directory
- a versioned `.tar.gz` archive suitable for `openclaw plugins install`
- a stable `*-latest.*` pointer set for operational scripts
- metadata with tool-surface and artifact hashes
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
from typing import Iterable


DEFAULT_OUTPUT_DIR = pathlib.Path.home() / ".manyforge" / "plugin-artifacts"


def _sha256_bytes(chunks: Iterable[bytes]) -> str:
    digest = hashlib.sha256()
    for chunk in chunks:
        digest.update(chunk)
    return digest.hexdigest()


def _surface_hash(plugin_dir: pathlib.Path) -> str:
    chunks: list[bytes] = []
    for name in ("package.json", "openclaw.plugin.json", "dist/index.js"):
        path = plugin_dir / name
        chunks.extend((name.encode("utf-8"), b"\0", path.read_bytes(), b"\0"))
    return _sha256_bytes(chunks)


def _write_reproducible_archive(plugin_dir: pathlib.Path, archive_path: pathlib.Path) -> None:
    with tarfile.open(archive_path, "w:gz", format=tarfile.PAX_FORMAT) as tar:
        for path in sorted(plugin_dir.rglob("*")):
            arcname = pathlib.Path(plugin_dir.name) / path.relative_to(plugin_dir)
            info = tar.gettarinfo(str(path), arcname=str(arcname))
            info.uid = 0
            info.gid = 0
            info.uname = "root"
            info.gname = "root"
            info.mtime = 0
            if path.is_file():
                with path.open("rb") as handle:
                    tar.addfile(info, handle)
            else:
                tar.addfile(info)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a versioned ManyForge OpenClaw plugin artifact")
    parser.add_argument("--mcp-endpoint", default="http://localhost:9000/api/mcp")
    parser.add_argument("--principal-id", default="system:manyforge-plugin-builder")
    parser.add_argument("--plugin-id", default="manyforge-mcp")
    parser.add_argument("--plugin-version", default="0.2.0")
    parser.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR))
    args = parser.parse_args()

    script_dir = pathlib.Path(__file__).resolve().parent
    generator = script_dir / "generate-manyforge-openclaw-plugin.py"
    output_dir = pathlib.Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="manyforge-plugin-build-") as tmp_root:
        tmp_root_path = pathlib.Path(tmp_root)
        plugin_dir = tmp_root_path / args.plugin_id
        subprocess.run(
            [
                sys.executable,
                str(generator),
                "--mcp-endpoint",
                args.mcp_endpoint,
                "--principal-id",
                args.principal_id,
                "--plugin-id",
                args.plugin_id,
                "--plugin-version",
                args.plugin_version,
                "--output-dir",
                str(plugin_dir),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )

        manifest = json.loads((plugin_dir / "openclaw.plugin.json").read_text(encoding="utf-8"))
        readme = (plugin_dir / "README.md").read_text(encoding="utf-8")
        tool_count = 0
        for line in readme.splitlines():
            if line.startswith("- Tool count:"):
                tool_count = int(line.split("`")[1])
                break

        surface_hash = _surface_hash(plugin_dir)
        artifact_suffix = surface_hash[:12]
        archive_name = f"{args.plugin_id}-{args.plugin_version}-{artifact_suffix}.tar.gz"
        archive_path = output_dir / archive_name
        _write_reproducible_archive(plugin_dir, archive_path)
        artifact_sha256 = _sha256_bytes([archive_path.read_bytes()])

        staged_dir = output_dir / args.plugin_id
        if staged_dir.exists():
            shutil.rmtree(staged_dir)
        shutil.copytree(plugin_dir, staged_dir)

        metadata = {
            "plugin_id": args.plugin_id,
            "plugin_version": manifest["version"],
            "tool_count": tool_count,
            "surface_sha256": surface_hash,
            "artifact_sha256": artifact_sha256,
            "archive_name": archive_name,
            "archive_path": str(archive_path),
            "mcp_endpoint": args.mcp_endpoint,
        }

        versioned_metadata = output_dir / f"{archive_path.name[:-7]}.json"
        versioned_sha = output_dir / f"{archive_path.name[:-7]}.sha256"
        versioned_metadata.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        versioned_sha.write_text(f"{artifact_sha256}  {archive_path.name}\n", encoding="utf-8")

        latest_archive = output_dir / f"{args.plugin_id}-latest.tar.gz"
        latest_metadata = output_dir / f"{args.plugin_id}-latest.json"
        latest_sha = output_dir / f"{args.plugin_id}-latest.sha256"
        shutil.copyfile(archive_path, latest_archive)
        latest_metadata.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        latest_sha.write_text(f"{artifact_sha256}  {latest_archive.name}\n", encoding="utf-8")

        print(json.dumps(metadata, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
