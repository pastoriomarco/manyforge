from __future__ import annotations

import argparse
import importlib
import json
import sys
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--module-dir",
        type=Path,
        action="append",
        required=True,
        help="repeat for every production binding build/install directory",
    )
    parser.add_argument(
        "--reverse",
        action="store_true",
        help="import RoboPlan before core to check both initialization orders",
    )
    args = parser.parse_args()

    for path in reversed(args.module_dir):
        sys.path.insert(0, str(path.resolve()))

    module_order = (
        ("manyforge_roboplan_py", "manyforge_core_py")
        if args.reverse
        else ("manyforge_core_py", "manyforge_roboplan_py")
    )
    imported = {
        name: importlib.import_module(name)
        for name in module_order
    }
    core = imported["manyforge_core_py"]
    roboplan = imported["manyforge_roboplan_py"]
    if core.__name__ != "manyforge_core_py":
        raise AssertionError("wrong core module imported")
    if roboplan.__name__ != "manyforge_roboplan_py":
        raise AssertionError("wrong RoboPlan module imported")
    cell_id = core.CellId("e0-production-coexistence")
    if cell_id.value != "e0-production-coexistence":
        raise AssertionError("production core strong-ID binding is not usable")
    if not callable(roboplan.create_native_kernel):
        raise AssertionError("production RoboPlan factory binding is not usable")
    if not hasattr(roboplan, "NativeKernelBundle"):
        raise AssertionError("production RoboPlan bundle type is absent")

    print(
        json.dumps(
            {
                "import_order": module_order,
                "manyforge_core_py": str(Path(core.__file__).resolve()),
                "manyforge_roboplan_py": str(Path(roboplan.__file__).resolve()),
                "core_strong_id": cell_id.value,
                "roboplan_factory_callable": True,
            },
            indent=2,
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
