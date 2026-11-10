"""Static build selection; no caller paths, imports, discovery or runtime reload."""

from manyforge_example_nodes import (
    IMPLEMENTATION_ID,
    NODE_ID,
    PACKAGE_ID,
    PACKAGE_ROOT,
)

from .canonical_artifact import load_strict_json_yaml
from .catalog import EXPECTED_CONTENT_VERSION

DESCRIPTOR_PATH = PACKAGE_ROOT / "descriptor.json"
PROJECT_SNAPSHOT_PATH = PACKAGE_ROOT / "_retained_source/pyproject.toml"


def descriptor():
    if DESCRIPTOR_PATH.stat().st_size > 65_536:
        raise ValueError("node extension descriptor exceeds build bound")
    value = load_strict_json_yaml(DESCRIPTOR_PATH)
    expected = {
        "api_version": "manyforge.node_extension.v1",
        "extension_id": PACKAGE_ID,
        "version": "0.2.1",
        "catalog_api_version": "manyforge.node_catalog.v3",
        "catalog_content_version": EXPECTED_CONTENT_VERSION,
        "dependencies": ["manyforge_behavior"],
        "execution_contract": "read_only_single_invocation",
    }
    if (
        not isinstance(value, dict)
        or set(value) != set(expected) | {"entries"}
        or any(value.get(key) != item for key, item in expected.items())
        or not isinstance(value["entries"], list)
        or len(value["entries"]) != 1
    ):
        raise ValueError("node extension descriptor does not match the selected build contract")
    entry = value["entries"][0]
    if (
        not isinstance(entry, dict)
        or entry.get("id") != NODE_ID
        or entry.get("runners") != {"py_trees": {"implementation_id": IMPLEMENTATION_ID}}
        or entry.get("kind") != "leaf"
        or entry.get("execution")
        != {"blocking": True, "cancellable": False, "owner_api": "model.kinematics"}
    ):
        raise ValueError("node extension cannot change its reviewed NRT owner binding")
    return value


def verify_composition(catalog):
    selected = descriptor()
    actual = [entry for entry in catalog.raw_data["entries"] if entry["id"] == NODE_ID]
    if actual != selected["entries"]:
        raise ValueError(
            "effective catalog does not contain the exact selected extension descriptor"
        )


def source_files():
    # Match the declared flat wheel. Build products are not installed source;
    # additional modules are covered, unsupported nested code fails the build.
    for path in PACKAGE_ROOT.rglob("*"):
        relative = path.relative_to(PACKAGE_ROOT)
        if any(
            part in {"build", "__pycache__"} or part.endswith(".egg-info")
            for part in relative.parts
        ):
            continue
        if path.suffix in {".py", ".pyi"} and (len(relative.parts) != 1 or path.suffix != ".py"):
            raise ValueError(
                "selected node extension requires flat .py modules; unsupported source layout"
            )
    return tuple(
        (
            "python-module:" + PACKAGE_ID + ("" if path.stem == "__init__" else "." + path.stem),
            path,
        )
        for path in sorted(PACKAGE_ROOT.glob("*.py"))
    ) + (
        ("node-extension-descriptor:" + PACKAGE_ID, DESCRIPTOR_PATH),
        ("python-project:" + PACKAGE_ID, PROJECT_SNAPSHOT_PATH),
    )
