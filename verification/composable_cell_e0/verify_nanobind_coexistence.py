from __future__ import annotations

import argparse
import importlib
import json
import sys
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--module-dir", type=Path, required=True)
    parser.add_argument("--reverse", action="store_true")
    args = parser.parse_args()

    sys.path.insert(0, str(args.module_dir.resolve()))
    order = ["e0_nanobind", "e0_nanobind_roboplan_peer"]
    if args.reverse:
        order.reverse()
    imported = {name: importlib.import_module(name) for name in order}

    core = imported["e0_nanobind"]
    peer = imported["e0_nanobind_roboplan_peer"]
    cell = core.CellId("nanobind-coexistence")
    if cell.value != "nanobind-coexistence":
        raise AssertionError("representative core module is not usable")
    pose = {
        "position_m": {"x_m": 0.25, "y_m": -0.5, "z_m": 1.75},
        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
    }
    if peer.pose_round_trip(pose) != pose:
        raise AssertionError("representative RoboPlan peer is not usable")

    print(
        json.dumps(
            {
                "import_order": order,
                "core_module": str(Path(core.__file__).resolve()),
                "peer_module": str(Path(peer.__file__).resolve()),
                "core_strong_id": cell.value,
                "peer_pose_round_trip": True,
            },
            indent=2,
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
