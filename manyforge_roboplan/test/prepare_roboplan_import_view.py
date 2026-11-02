#!/usr/bin/env python3
"""Write the existing private RoboPlan view for one maintained model test."""

from __future__ import annotations

import argparse
from pathlib import Path

from manyforge_behavior.robot_model_authority import (
    build_roboplan_import_view,
    load_robot_model_authority,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-root", required=True, type=Path)
    parser.add_argument("--package-root", required=True, type=Path)
    parser.add_argument("--model-name", required=True)
    parser.add_argument("--output-directory", required=True, type=Path)
    arguments = parser.parse_args()

    authority = load_robot_model_authority(
        urdf_path=arguments.model_root / f"{arguments.model_name}.urdf",
        srdf_path=arguments.model_root / f"{arguments.model_name}.srdf",
        package_paths=(str(arguments.package_root),),
        model_name=arguments.model_name,
    )
    view = build_roboplan_import_view(authority)
    if not view.changed:
        raise RuntimeError(
            "the maintained model no longer requires its RoboPlan import view"
        )

    arguments.output_directory.mkdir(parents=True, exist_ok=True)
    view.write_to_directory(
        arguments.output_directory, stem=arguments.model_name
    )
    mapping = "".join(
        f"{canonical}\t{imported}\n"
        for canonical, imported in sorted(
            view.canonical_to_view_joint_names.items()
        )
    )
    (arguments.output_directory / "joint-map.tsv").write_text(
        mapping, encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
