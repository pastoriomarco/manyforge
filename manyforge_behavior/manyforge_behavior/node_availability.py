"""Truthful per-node authoring and execution availability projection."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, Final

from manyforge_behavior.executable_node_resolver import ExecutableNodeResolver

NODE_EXECUTION_AVAILABILITY_API: Final = "manyforge.node_execution_availability.v1"


class NodeAvailabilityError(ValueError):
    """Availability facts are incomplete, inconsistent, or not exact."""


@dataclass(frozen=True, slots=True)
class AvailabilityFact:
    """One explicit availability dimension and its stable failure reasons."""

    available: bool
    reason_codes: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if type(self.available) is not bool:
            raise TypeError("availability must be boolean")
        if (
            not isinstance(self.reason_codes, tuple)
            or any(
                not isinstance(reason, str) or not reason
                for reason in self.reason_codes
            )
            or len(set(self.reason_codes)) != len(self.reason_codes)
            or self.reason_codes != tuple(sorted(self.reason_codes))
        ):
            raise ValueError("availability reason_codes must be unique canonical text")
        if self.available and self.reason_codes:
            raise ValueError("available facts cannot carry failure reasons")
        if not self.available and not self.reason_codes:
            raise ValueError("unavailable facts require a reason code")


@dataclass(frozen=True, slots=True)
class NodeExecutionReadiness:
    """Exact binding and live-owner facts supplied by headless owners."""

    program_binding_id: str | None
    binding_by_node: Mapping[str, AvailabilityFact]
    owner_by_port: Mapping[str, AvailabilityFact]

    def __post_init__(self) -> None:
        if self.program_binding_id is not None and (
            not isinstance(self.program_binding_id, str)
            or not self.program_binding_id.startswith("sha256:")
            or len(self.program_binding_id) != 71
            or any(
                character not in "0123456789abcdef"
                for character in self.program_binding_id[7:]
            )
        ):
            raise ValueError("program_binding_id must be an exact SHA-256 identity")
        if not isinstance(self.binding_by_node, Mapping):
            raise TypeError("binding_by_node must be a mapping")
        if not isinstance(self.owner_by_port, Mapping):
            raise TypeError("owner_by_port must be a mapping")
        bindings = dict(self.binding_by_node)
        owners = dict(self.owner_by_port)
        if any(
            not isinstance(key, str) or not isinstance(value, AvailabilityFact)
            for key, value in bindings.items()
        ):
            raise TypeError("binding readiness must map node IDs to facts")
        if any(
            not isinstance(key, str) or not isinstance(value, AvailabilityFact)
            for key, value in owners.items()
        ):
            raise TypeError("owner readiness must map port names to facts")
        object.__setattr__(self, "binding_by_node", MappingProxyType(bindings))
        object.__setattr__(self, "owner_by_port", MappingProxyType(owners))

    @classmethod
    def unselected(
        cls,
        resolver: ExecutableNodeResolver,
    ) -> "NodeExecutionReadiness":
        dispositions = resolver.dispositions
        return cls(
            program_binding_id=None,
            binding_by_node={
                disposition.row.node_id: AvailabilityFact(
                    False,
                    ("program_binding_unselected",),
                )
                for disposition in dispositions
            },
            owner_by_port={
                owner_port: AvailabilityFact(
                    False,
                    ("program_runtime_owner_unavailable",),
                )
                for owner_port in sorted(
                    {
                        disposition.owner_port_name
                        for disposition in dispositions
                        if disposition.owner_port_name is not None
                    }
                )
            },
        )


def _exact_keys(
    observed: Mapping[str, Any],
    expected: set[str],
    *,
    label: str,
) -> None:
    actual = set(observed)
    if actual != expected:
        raise NodeAvailabilityError(
            f"{label} is not exact; missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def project_node_execution_availability(
    resolver: ExecutableNodeResolver,
    readiness: NodeExecutionReadiness,
    *,
    required_node_ids: Sequence[str] = (),
) -> dict[str, Any]:
    """Join static adapters to exact binding and owner-health dimensions."""

    if not isinstance(resolver, ExecutableNodeResolver):
        raise TypeError("availability projection requires the closed resolver")
    if not isinstance(readiness, NodeExecutionReadiness):
        raise TypeError("availability projection requires exact readiness facts")
    dispositions = resolver.dispositions
    expected_nodes = {item.row.node_id for item in dispositions}
    expected_ports = {
        item.owner_port_name
        for item in dispositions
        if item.owner_port_name is not None
    }
    _exact_keys(readiness.binding_by_node, expected_nodes, label="binding facts")
    _exact_keys(readiness.owner_by_port, expected_ports, label="owner facts")

    required = tuple(required_node_ids)
    if (
        any(not isinstance(node_id, str) or not node_id for node_id in required)
        or len(set(required)) != len(required)
        or tuple(sorted(required)) != required
        or not set(required).issubset(expected_nodes)
    ):
        raise NodeAvailabilityError(
            "required_node_ids must be a unique canonical registry subset"
        )

    implementations: list[dict[str, Any]] = []
    execution_by_node: dict[str, bool] = {}
    reasons_by_node: dict[str, tuple[str, ...]] = {}
    for disposition in dispositions:
        node_id = disposition.row.node_id
        binding = readiness.binding_by_node[node_id]
        if not disposition.adapter_build_qualified:
            owner = AvailabilityFact(
                False,
                ("program_runtime_owner_unavailable",),
            )
        elif disposition.owner_port_name is None:
            owner = AvailabilityFact(True)
        else:
            owner = readiness.owner_by_port[disposition.owner_port_name]
        reasons: set[str] = set()
        if not disposition.adapter_build_qualified:
            if disposition.unavailable_reason_code is None:
                raise NodeAvailabilityError(
                    f"node {node_id!r} lacks an adapter disposition reason"
                )
            reasons.add(disposition.unavailable_reason_code)
        if not binding.available:
            reasons.update(binding.reason_codes)
        if not owner.available:
            reasons.update(owner.reason_codes)
        reason_codes = tuple(sorted(reasons))
        execution_available = (
            disposition.adapter_build_qualified
            and binding.available
            and owner.available
        )
        execution_by_node[node_id] = execution_available
        reasons_by_node[node_id] = reason_codes
        implementations.append(
            {
                **disposition.row.to_dict(),
                "authoring_available": True,
                "adapter_build_qualified": disposition.adapter_build_qualified,
                "binding_available": binding.available,
                "owner_port_name": disposition.owner_port_name,
                "owner_health_available": owner.available,
                "execution_available": execution_available,
                "implementation_identity_resolved": True,
                "reason_codes": list(reason_codes),
                "restoration_gates": list(disposition.restoration_gates),
            }
        )

    execution_available = (
        readiness.program_binding_id is not None
        and bool(required)
        and all(execution_by_node[node_id] for node_id in required)
    )
    aggregate_reasons = sorted(
        {reason for node_id in required for reason in reasons_by_node[node_id]}
    )
    if readiness.program_binding_id is None:
        aggregate_reasons.append("program_binding_unselected")
        aggregate_reasons = sorted(set(aggregate_reasons))
    return {
        "schema_version": NODE_EXECUTION_AVAILABILITY_API,
        "environment_digest": resolver.environment_digest,
        "program_binding_id": readiness.program_binding_id,
        "required_node_ids": list(required),
        "portable_authoring_available": True,
        "execution_available": execution_available,
        "reason_codes": aggregate_reasons,
        "implementations": implementations,
    }


__all__ = [
    "AvailabilityFact",
    "NODE_EXECUTION_AVAILABILITY_API",
    "NodeAvailabilityError",
    "NodeExecutionReadiness",
    "project_node_execution_availability",
]
