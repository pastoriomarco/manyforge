"""Program artifact loader and validator for ManyForge behavior trees."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import yaml

from manyforge_behavior.catalog import NodeCatalog


EXPECTED_API_VERSION = "manyforge.program.v0"


@dataclass
class SceneResource:
    object_id: str
    shape: dict[str, Any]
    pose: dict[str, Any]


@dataclass
class ProgramParameter:
    name: str
    type: str
    default: Any
    constraints: dict[str, Any] = field(default_factory=dict)
    description: str = ""


@dataclass
class TreeNode:
    id: str
    name: str
    params: dict[str, Any] = field(default_factory=dict)
    children: list["TreeNode"] = field(default_factory=list)


@dataclass
class RobotProfile:
    robot_id: str
    group_id: str
    base_frame: str
    tcp_frame: str


class ProgramValidationError(Exception):
    pass


class Program:
    """Loads and validates a ManyForge program artifact from YAML."""

    def __init__(self, path: str | Path, catalog: NodeCatalog):
        self._path = Path(path)
        self._catalog = catalog
        self.name: str = ""
        self.description: str = ""
        self.program_profile: str = ""
        self.catalog_version: str = ""
        self.robot: Optional[RobotProfile] = None
        self.scene: list[SceneResource] = []
        self.parameters: dict[str, ProgramParameter] = {}
        self.tree: Optional[TreeNode] = None
        self._load_and_validate()

    def _load_and_validate(self) -> None:
        with open(self._path) as f:
            data = yaml.safe_load(f)

        if not isinstance(data, dict):
            raise ProgramValidationError("Program must be a YAML mapping")

        # Schema validation
        api_version = data.get("api_version")
        if api_version != EXPECTED_API_VERSION:
            raise ProgramValidationError(
                f"Expected api_version '{EXPECTED_API_VERSION}', got '{api_version}'"
            )

        self.program_profile = data.get("program_profile", "")
        if not self.program_profile:
            raise ProgramValidationError("program_profile is required")

        self.catalog_version = data.get("catalog_version", "")
        self.name = data.get("name", "")
        self.description = data.get("description", "")

        # Robot profile
        robot_data = data.get("robot")
        if not robot_data:
            raise ProgramValidationError("robot section is required")
        for field_name in ("robot_id", "group_id", "base_frame", "tcp_frame"):
            if field_name not in robot_data:
                raise ProgramValidationError(
                    f"robot.{field_name} is required"
                )
        self.robot = RobotProfile(
            robot_id=robot_data["robot_id"],
            group_id=robot_data["group_id"],
            base_frame=robot_data["base_frame"],
            tcp_frame=robot_data["tcp_frame"],
        )

        # Scene resources
        scene_ids: set[str] = set()
        for s in data.get("scene", []):
            oid = s.get("object_id", "")
            if oid in scene_ids:
                raise ProgramValidationError(
                    f"Duplicate scene object_id: '{oid}'"
                )
            scene_ids.add(oid)
            self.scene.append(
                SceneResource(
                    object_id=oid,
                    shape=s.get("shape", {}),
                    pose=s.get("pose", {}),
                )
            )

        # Parameters
        param_names: set[str] = set()
        for p in data.get("parameters", []):
            name = p.get("name", "")
            if name in param_names:
                raise ProgramValidationError(
                    f"Duplicate parameter name: '{name}'"
                )
            param_names.add(name)
            self.parameters[name] = ProgramParameter(
                name=name,
                type=p.get("type", "string"),
                default=p.get("default"),
                constraints=p.get("constraints", {}),
                description=p.get("description", ""),
            )

        # Tree
        tree_data = data.get("tree")
        if not tree_data:
            raise ProgramValidationError("tree section is required")
        self.tree = self._parse_tree(tree_data)

        # Structural validation
        self._validate_tree(self.tree)

    def _parse_tree(self, data: dict[str, Any]) -> TreeNode:
        children = [self._parse_tree(c) for c in data.get("children", [])]
        return TreeNode(
            id=data["id"],
            name=data.get("name", data["id"]),
            params=data.get("params", {}),
            children=children,
        )

    def _validate_tree(self, node: TreeNode) -> None:
        """Validate tree against catalog: node ids, required params, child counts, unique names."""
        names: set[str] = set()
        self._validate_node_recursive(node, names)

    def _validate_node_recursive(
        self, node: TreeNode, names: set[str]
    ) -> None:
        # Unique names
        if node.name in names:
            raise ProgramValidationError(
                f"Duplicate node instance name: '{node.name}'"
            )
        names.add(node.name)

        # Catalog lookup
        try:
            entry = self._catalog.get(node.id)
        except KeyError:
            raise ProgramValidationError(
                f"Unknown catalog entry: '{node.id}'"
            )

        # Child count validation
        n_children = len(node.children)
        if n_children < entry.children_min:
            raise ProgramValidationError(
                f"Node '{node.name}' ({node.id}): "
                f"requires at least {entry.children_min} children, "
                f"got {n_children}"
            )
        if entry.children_max >= 0 and n_children > entry.children_max:
            raise ProgramValidationError(
                f"Node '{node.name}' ({node.id}): "
                f"allows at most {entry.children_max} children, "
                f"got {n_children}"
            )

        # Required params validation
        for param in entry.parameters:
            if param.required and param.name not in node.params:
                if param.default is None:
                    raise ProgramValidationError(
                        f"Node '{node.name}' ({node.id}): "
                        f"missing required parameter '{param.name}'"
                    )

        # Recurse
        for child in node.children:
            self._validate_node_recursive(child, names)

    def get_parameter_defaults(self) -> dict[str, Any]:
        """Return {name: default_value} for all program parameters."""
        return {p.name: p.default for p in self.parameters.values()}
