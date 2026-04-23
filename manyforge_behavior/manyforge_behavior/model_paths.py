"""Helpers for resolving robot model assets from deployment configuration."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


@dataclass(frozen=True)
class ResolvedRobotModel:
    model_name: str
    urdf_path: str
    srdf_path: str
    package_paths: list[str]


def default_package_paths() -> list[str]:
    """Return package search roots available from the local RoboPlan install."""
    try:
        from roboplan import example_models

        return [str(example_models.get_package_share_dir())]
    except Exception:
        return []


def resolve_package_uri(
    resource_uri: str,
    package_paths: Sequence[str] | None = None,
) -> str | None:
    """Resolve a ``package://`` URI without allowing root escape.

    The URI remainder is resolved relative to each package root and MUST remain
    contained within that root after normalization. This prevents path
    traversal such as ``package://../secret`` from escaping the declared
    package roots.
    """
    if not resource_uri.startswith("package://"):
        return None

    remainder = resource_uri[len("package://") :]
    if not remainder:
        return None

    relative_path = Path(remainder)
    if relative_path.is_absolute():
        return None

    for package_root in package_paths or []:
        root = Path(package_root).resolve()
        candidate = (root / relative_path).resolve()
        try:
            candidate.relative_to(root)
        except ValueError:
            continue
        if candidate.exists():
            return str(candidate)
    return None


def normalize_package_paths(
    package_paths: Sequence[str] | None,
    *,
    base_dir: str | Path,
    extra_package_paths: Iterable[str] | None = None,
) -> list[str]:
    """Normalize package search roots to absolute strings with de-duplication."""
    roots: list[str] = []
    seen: set[str] = set()
    base_dir = Path(base_dir)
    for candidate in list(package_paths or []) + list(extra_package_paths or []):
        if not candidate:
            continue
        path = Path(candidate)
        if not path.is_absolute():
            path = (base_dir / path).resolve()
        normalized = str(path)
        if normalized not in seen:
            seen.add(normalized)
            roots.append(normalized)
    return roots


def resolve_resource_path(
    resource_path: str,
    *,
    base_dir: str | Path,
    package_paths: Sequence[str] | None = None,
) -> str:
    """Resolve a deployment-configured resource path to a filesystem path."""
    if resource_path.startswith("package://"):
        resolved = resolve_package_uri(resource_path, package_paths)
        if resolved is not None:
            return resolved
        raise FileNotFoundError(
            f"Could not resolve package resource '{resource_path}' "
            f"against package paths: {list(package_paths or [])}"
        )

    path = Path(resource_path)
    if not path.is_absolute():
        path = (Path(base_dir) / path).resolve()
    return str(path)


def resolve_robot_model(
    *,
    urdf_path: str,
    srdf_path: str,
    package_paths: Sequence[str] | None,
    deployment_path: str | Path,
    extra_package_paths: Iterable[str] | None = None,
) -> ResolvedRobotModel:
    """Resolve a robot model bundle declared in a deployment."""
    deployment_dir = Path(deployment_path).resolve().parent
    search_roots = normalize_package_paths(
        package_paths,
        base_dir=deployment_dir,
        extra_package_paths=extra_package_paths,
    )
    resolved_urdf = resolve_resource_path(
        urdf_path,
        base_dir=deployment_dir,
        package_paths=search_roots,
    )
    resolved_srdf = resolve_resource_path(
        srdf_path,
        base_dir=deployment_dir,
        package_paths=search_roots,
    )
    return ResolvedRobotModel(
        model_name=Path(resolved_urdf).stem or "robot_model",
        urdf_path=resolved_urdf,
        srdf_path=resolved_srdf,
        package_paths=search_roots,
    )
