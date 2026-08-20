"""Independent verification of source-local robot authoring facts."""

from __future__ import annotations

from dataclasses import dataclass
import math
import unicodedata
from typing import TYPE_CHECKING, Any

from manyforge_behavior.canonical_artifact import (
    CONTENT_IDENTITY_PREFIX,
    canonical_json_bytes,
    raw_sha256_digest,
)

from .node_environment_client import (
    VerifiedActiveNodeEnvironment,
    verify_active_node_environment,
)

if TYPE_CHECKING:
    from .state import ComposerState


_PROJECTION_VERSION = "manyforge.source_model_projection.v1"
_CONTEXT_VERSION = "manyforge.source_model_context.v1"
_PROJECTION_IDENTITY_DOMAIN = "manyforge-source-model-projection-semantic"
_PROJECTION_FIELDS = {
    "cell_id",
    "graph_revision",
    "model_map_contract",
    "model_map_product_id",
    "occurrences",
    "retained_bundle_id",
    "schema_version",
    "source",
    "targets",
}
_OCCURRENCE_FIELDS = {
    "component_id",
    "coordinates",
    "model_asset_id",
    "model_instance_id",
    "model_revision",
    "model_role_id",
    "source_model_name",
    "source_robot_name",
    "source_link_names",
    "srdf_end_effectors",
    "srdf_group_states",
    "srdf_groups",
}
_COORDINATE_FIELDS = {
    "coordinate_kind",
    "joint_type",
    "mimic",
    "source_child_link_name",
    "source_joint_index",
    "source_joint_name",
    "source_parent_link_name",
    "unit",
}
_TARGET_FIELDS = {
    "collision_bodies",
    "component_id",
    "frames",
    "model_groups",
    "target_id",
}
_SOURCE_FIELDS = {
    "available",
    "freshness",
    "known_gaps",
    "projection_generation",
    "projection_owner_epoch",
    "receiver_age_ns",
    "selection_projection_generation",
    "selection_source_content_digest",
    "selection_source_generation",
    "selection_source_owner_epoch",
    "selection_source_owner_id",
    "source_content_digest",
    "source_generation",
    "source_owner_epoch",
    "source_owner_id",
    "source_owner_kind",
}
_COORDINATE_CONTRACTS = {
    "continuous": ("angular", "radians"),
    "fixed": ("none", "none"),
    "floating": ("floating", "floating_meters_radians"),
    "planar": ("planar", "planar_meters_radians"),
    "prismatic": ("linear", "meters"),
    "revolute": ("angular", "radians"),
}


class ActiveSourceModelProjectionError(RuntimeError):
    """The active source-local model projection is unavailable or invalid."""

    def __init__(self, code: str, message: str, status_code: int = 503) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code


@dataclass(frozen=True, slots=True)
class VerifiedActiveSourceModel:
    payload: dict[str, Any]


def _invalid(message: str) -> ActiveSourceModelProjectionError:
    return ActiveSourceModelProjectionError(
        "source_model_projection_invalid",
        message,
    )


def _object(value: Any, label: str) -> dict[str, Any]:
    if type(value) is not dict or any(type(key) is not str for key in value):
        raise _invalid(f"{label} must be a built-in object")
    return value


def _closed(value: dict[str, Any], fields: set[str], label: str) -> None:
    if set(value) != fields:
        raise _invalid(f"{label} has an unsupported shape")


def _text(value: Any, label: str, *, nullable: bool = False) -> str | None:
    if nullable and value is None:
        return None
    if (
        type(value) is not str
        or not value
        or value != value.strip()
        or unicodedata.normalize("NFC", value) != value
        or len(value.encode("utf-8")) > 4096
    ):
        raise _invalid(f"{label} must be bounded non-empty NFC text")
    return value


def _digest(value: Any, label: str) -> str:
    result = _text(value, label)
    assert result is not None
    if (
        len(result) != 71
        or not result.startswith("sha256:")
        or any(character not in "0123456789abcdef" for character in result[7:])
    ):
        raise _invalid(f"{label} must be an exact SHA-256 digest")
    return result


def _framed_digest(value: dict[str, Any]) -> str:
    preimage = b"\0".join(
        (
            CONTENT_IDENTITY_PREFIX,
            _PROJECTION_IDENTITY_DOMAIN.encode("ascii"),
            _PROJECTION_VERSION.encode("ascii"),
            canonical_json_bytes(value),
        )
    )
    return raw_sha256_digest(preimage)


def _array(value: Any, label: str, *, maximum: int = 100_000) -> list[Any]:
    if type(value) is not list or len(value) > maximum:
        raise _invalid(f"{label} must be a bounded array")
    return value


def _text_array(value: Any, label: str) -> list[str]:
    result = [
        _text(item, f"{label}[{index}]")
        for index, item in enumerate(_array(value, label))
    ]
    if len(result) != len(set(result)):
        raise _invalid(f"{label} contains duplicates")
    return [item for item in result if item is not None]


def _finite_number(value: Any, label: str) -> float:
    if type(value) not in {int, float} or not math.isfinite(float(value)):
        raise _invalid(f"{label} must be a finite JSON number")
    return float(value)


def _source(value: Any) -> dict[str, Any]:
    source = _object(value, "source model source")
    _closed(source, _SOURCE_FIELDS, "source model source")
    if source.get("available") is not True or source.get("freshness") != "fresh":
        raise ActiveSourceModelProjectionError(
            "source_model_projection_unavailable",
            "the active source model projection is unavailable or stale",
        )
    if source.get("known_gaps") != []:
        raise ActiveSourceModelProjectionError(
            "source_model_projection_unavailable",
            "the active source model projection is incomplete",
        )
    for field in (
        "projection_owner_epoch",
        "selection_source_owner_epoch",
        "selection_source_owner_id",
        "source_owner_epoch",
        "source_owner_id",
    ):
        _text(source[field], f"source.{field}")
    for field in (
        "selection_source_content_digest",
        "source_content_digest",
    ):
        _digest(source[field], f"source.{field}")
    if source["source_owner_kind"] != "source_model_projection":
        raise _invalid("source.source_owner_kind is not source_model_projection")
    for field in (
        "projection_generation",
        "receiver_age_ns",
        "selection_projection_generation",
        "selection_source_generation",
        "source_generation",
    ):
        item = source[field]
        if type(item) is not int or item < 0:
            raise _invalid(f"source.{field} must be a non-negative integer")
    return source


def _reject_private_names(value: Any, *, path: str = "projection") -> None:
    if type(value) is dict:
        for key, item in value.items():
            lowered = key.lower()
            if (
                "compiled" in lowered
                or "namespace" in lowered
                or "control_binding" in lowered
                or "control_route" in lowered
                or lowered == "resource_id"
                or lowered == "resource_ids"
            ):
                raise _invalid(f"{path}.{key} exposes a private compiler/runtime name")
            _reject_private_names(item, path=f"{path}.{key}")
    elif type(value) is list:
        for index, item in enumerate(value):
            _reject_private_names(item, path=f"{path}[{index}]")
    elif type(value) is float and not math.isfinite(value):
        raise _invalid(f"{path} contains a non-finite number")


def _occurrences(value: Any) -> dict[str, dict[str, Any]]:
    by_id: dict[str, dict[str, Any]] = {}
    for index, raw in enumerate(_array(value, "occurrences", maximum=4096)):
        path = f"occurrences[{index}]"
        row = _object(raw, path)
        _closed(row, _OCCURRENCE_FIELDS, path)
        instance_id = _text(row["model_instance_id"], f"{path}.model_instance_id")
        assert instance_id is not None
        if instance_id in by_id:
            raise _invalid("occurrences contain duplicate model_instance_id values")
        for field in (
            "component_id",
            "model_role_id",
        ):
            _text(row[field], f"{path}.{field}", nullable=True)
        for field in (
            "model_asset_id",
            "model_revision",
            "source_model_name",
            "source_robot_name",
        ):
            _text(row[field], f"{path}.{field}")

        joint_names: list[str] = []
        coordinate_rows: list[dict[str, Any]] = []
        expected_index = 0
        for coordinate_index, raw_coordinate in enumerate(
            _array(row["coordinates"], f"{path}.coordinates")
        ):
            coordinate_path = f"{path}.coordinates[{coordinate_index}]"
            coordinate = _object(raw_coordinate, coordinate_path)
            _closed(coordinate, _COORDINATE_FIELDS, coordinate_path)
            source_index = coordinate["source_joint_index"]
            if type(source_index) is not int or source_index != expected_index:
                raise _invalid(
                    f"{coordinate_path}.source_joint_index must preserve source order"
                )
            expected_index += 1
            joint_name = _text(
                coordinate["source_joint_name"],
                f"{coordinate_path}.source_joint_name",
            )
            assert joint_name is not None
            joint_names.append(joint_name)
            coordinate_rows.append(coordinate)
            joint_type = _text(
                coordinate["joint_type"], f"{coordinate_path}.joint_type"
            )
            coordinate_kind = _text(
                coordinate["coordinate_kind"],
                f"{coordinate_path}.coordinate_kind",
            )
            unit = _text(coordinate["unit"], f"{coordinate_path}.unit")
            if (
                joint_type not in _COORDINATE_CONTRACTS
                or _COORDINATE_CONTRACTS[joint_type] != (coordinate_kind, unit)
            ):
                raise _invalid(f"{coordinate_path} has inconsistent URDF semantics")
        if len(joint_names) != len(set(joint_names)):
            raise _invalid(f"{path}.coordinates contain duplicate source joints")

        link_names = _text_array(
            row["source_link_names"], f"{path}.source_link_names"
        )
        if link_names != sorted(link_names):
            raise _invalid(f"{path}.source_link_names are not canonical-sorted")

        joint_name_set = set(joint_names)
        link_name_set = set(link_names)
        for coordinate_index, coordinate in enumerate(coordinate_rows):
            coordinate_path = f"{path}.coordinates[{coordinate_index}]"
            for field in ("source_parent_link_name", "source_child_link_name"):
                link_name = _text(coordinate[field], f"{coordinate_path}.{field}")
                if link_name not in link_name_set:
                    raise _invalid(f"{coordinate_path}.{field} names an unknown link")
            mimic = coordinate["mimic"]
            if mimic is not None:
                mimic_row = _object(mimic, f"{coordinate_path}.mimic")
                _closed(
                    mimic_row,
                    {"multiplier", "offset", "source_joint_name"},
                    f"{coordinate_path}.mimic",
                )
                source_joint = _text(
                    mimic_row["source_joint_name"],
                    f"{coordinate_path}.mimic.source_joint_name",
                )
                if source_joint not in joint_name_set:
                    raise _invalid(f"{coordinate_path}.mimic names an unknown joint")
                _finite_number(
                    mimic_row["multiplier"],
                    f"{coordinate_path}.mimic.multiplier",
                )
                _finite_number(
                    mimic_row["offset"],
                    f"{coordinate_path}.mimic.offset",
                )

        group_names: list[str] = []
        group_rows: list[dict[str, Any]] = []
        for group_index, raw_group in enumerate(
            _array(row["srdf_groups"], f"{path}.srdf_groups")
        ):
            group_path = f"{path}.srdf_groups[{group_index}]"
            group = _object(raw_group, group_path)
            _closed(
                group,
                {
                    "chains",
                    "source_group_name",
                    "source_joint_names",
                    "source_link_names",
                    "source_subgroup_names",
                },
                group_path,
            )
            name = _text(
                group["source_group_name"],
                f"{group_path}.source_group_name",
            )
            assert name is not None
            group_names.append(name)
            group_rows.append(group)
            if not set(
                _text_array(group["source_joint_names"], f"{group_path}.source_joint_names")
            ).issubset(joint_name_set):
                raise _invalid(f"{group_path} contains an unknown source joint")
            if not set(
                _text_array(group["source_link_names"], f"{group_path}.source_link_names")
            ).issubset(link_name_set):
                raise _invalid(f"{group_path} contains an unknown source link")
            for chain_index, raw_chain in enumerate(
                _array(group["chains"], f"{group_path}.chains")
            ):
                chain_path = f"{group_path}.chains[{chain_index}]"
                chain = _object(raw_chain, chain_path)
                _closed(
                    chain,
                    {"source_base_link_name", "source_tip_link_name"},
                    chain_path,
                )
                for field in ("source_base_link_name", "source_tip_link_name"):
                    if _text(chain[field], f"{chain_path}.{field}") not in link_name_set:
                        raise _invalid(f"{chain_path}.{field} names an unknown link")
        if len(group_names) != len(set(group_names)):
            raise _invalid(f"{path}.srdf_groups contains duplicates")

        group_name_set = set(group_names)
        for group_index, group in enumerate(group_rows):
            subgroup_names = _text_array(
                group["source_subgroup_names"],
                f"{path}.srdf_groups[{group_index}].source_subgroup_names",
            )
            if not set(subgroup_names).issubset(group_name_set):
                raise _invalid(
                    f"{path}.srdf_groups[{group_index}] contains an unknown subgroup"
                )

        states = _array(row["srdf_group_states"], f"{path}.srdf_group_states")
        state_identities: set[tuple[str, str]] = set()
        for state_index, raw_state in enumerate(states):
            state_path = f"{path}.srdf_group_states[{state_index}]"
            state = _object(raw_state, state_path)
            _closed(
                state,
                {"source_group_name", "source_group_state_name", "source_joint_names"},
                state_path,
            )
            group_name = _text(state["source_group_name"], f"{state_path}.source_group_name")
            state_name = _text(
                state["source_group_state_name"],
                f"{state_path}.source_group_state_name",
            )
            assert group_name is not None and state_name is not None
            if (group_name, state_name) in state_identities:
                raise _invalid(
                    f"{path}.srdf_group_states contains duplicate identities"
                )
            state_identities.add((group_name, state_name))
            if group_name not in group_name_set:
                raise _invalid(
                    f"{path}.srdf_group_states names an unknown source group"
                )
            if not set(
                _text_array(state["source_joint_names"], f"{state_path}.source_joint_names")
            ).issubset(joint_name_set):
                raise _invalid(f"{state_path} contains an unknown source joint")
        end_effectors = _array(
            row["srdf_end_effectors"], f"{path}.srdf_end_effectors"
        )
        end_effector_names: set[str] = set()
        for end_effector_index, raw_end_effector in enumerate(end_effectors):
            end_effector_path = (
                f"{path}.srdf_end_effectors[{end_effector_index}]"
            )
            end_effector = _object(raw_end_effector, end_effector_path)
            _closed(
                end_effector,
                {
                    "source_end_effector_name",
                    "source_group_name",
                    "source_parent_group_name",
                    "source_parent_link_name",
                },
                end_effector_path,
            )
            end_effector_name = _text(
                end_effector["source_end_effector_name"],
                f"{end_effector_path}.source_end_effector_name",
            )
            assert end_effector_name is not None
            if end_effector_name in end_effector_names:
                raise _invalid(
                    f"{path}.srdf_end_effectors contains duplicate identities"
                )
            end_effector_names.add(end_effector_name)
            if _text(
                end_effector["source_group_name"],
                f"{end_effector_path}.source_group_name",
            ) not in group_name_set:
                raise _invalid(f"{end_effector_path} names an unknown source group")
            parent_group = _text(
                end_effector["source_parent_group_name"],
                f"{end_effector_path}.source_parent_group_name",
                nullable=True,
            )
            if parent_group is not None and parent_group not in group_name_set:
                raise _invalid(
                    f"{end_effector_path} names an unknown source parent group"
                )
            if _text(
                end_effector["source_parent_link_name"],
                f"{end_effector_path}.source_parent_link_name",
            ) not in link_name_set:
                raise _invalid(f"{end_effector_path} names an unknown source link")
        by_id[instance_id] = row
    return by_id


def _targets(value: Any, occurrences: dict[str, dict[str, Any]]) -> None:
    identities: set[tuple[str, str]] = set()
    for index, raw in enumerate(_array(value, "targets", maximum=4096)):
        path = f"targets[{index}]"
        row = _object(raw, path)
        _closed(row, _TARGET_FIELDS, path)
        component_id = _text(row["component_id"], f"{path}.component_id")
        target_id = _text(row["target_id"], f"{path}.target_id")
        assert component_id is not None and target_id is not None
        if (component_id, target_id) in identities:
            raise _invalid("targets contain a duplicate component/target identity")
        identities.add((component_id, target_id))

        group_roles: set[str] = set()
        for group_index, raw_group in enumerate(
            _array(row["model_groups"], f"{path}.model_groups")
        ):
            group_path = f"{path}.model_groups[{group_index}]"
            group = _object(raw_group, group_path)
            _closed(
                group,
                {
                    "commandable_source_joint_names",
                    "model_group_role_id",
                    "model_instance_id",
                    "source_group_name",
                    "source_joint_names",
                    "source_link_names",
                },
                group_path,
            )
            instance_id = _text(
                group["model_instance_id"], f"{group_path}.model_instance_id"
            )
            assert instance_id is not None
            occurrence = occurrences.get(instance_id)
            if occurrence is None:
                raise _invalid(f"{group_path} names an unknown occurrence")
            group_role = _text(
                group["model_group_role_id"], f"{group_path}.model_group_role_id"
            )
            assert group_role is not None
            if group_role in group_roles:
                raise _invalid(f"{path}.model_groups contains duplicate roles")
            group_roles.add(group_role)
            group_name = _text(
                group["source_group_name"], f"{group_path}.source_group_name"
            )
            source_groups = {
                item["source_group_name"] for item in occurrence["srdf_groups"]
            }
            if group_name not in source_groups:
                raise _invalid(f"{group_path} names an unknown source SRDF group")
            source_joints = _text_array(
                group["source_joint_names"], f"{group_path}.source_joint_names"
            )
            occurrence_joints = {
                item["source_joint_name"] for item in occurrence["coordinates"]
            }
            if not set(source_joints).issubset(occurrence_joints):
                raise _invalid(f"{group_path} contains an unknown source joint")
            commandable = _text_array(
                group["commandable_source_joint_names"],
                f"{group_path}.commandable_source_joint_names",
            )
            if not set(commandable).issubset(source_joints):
                raise _invalid(f"{group_path} commandable joints escape the group")
            source_links = _text_array(
                group["source_link_names"], f"{group_path}.source_link_names"
            )
            if not set(source_links).issubset(set(occurrence["source_link_names"])):
                raise _invalid(f"{group_path} contains an unknown source link")

        frame_roles: set[str] = set()
        collision_roles: set[str] = set()
        for field, fields in (
            (
                "frames",
                {"frame_kind", "frame_role_id", "model_instance_id", "source_link_name"},
            ),
            (
                "collision_bodies",
                {"collision_body_role_id", "model_instance_id", "source_link_names"},
            ),
        ):
            for item_index, raw_item in enumerate(
                _array(row[field], f"{path}.{field}")
            ):
                item_path = f"{path}.{field}[{item_index}]"
                item = _object(raw_item, item_path)
                _closed(item, fields, item_path)
                if field == "frames":
                    frame_kind = _text(
                        item["frame_kind"], f"{item_path}.frame_kind"
                    )
                    frame_role = _text(
                        item["frame_role_id"], f"{item_path}.frame_role_id"
                    )
                    assert frame_kind is not None and frame_role is not None
                    if frame_role in frame_roles:
                        raise _invalid(f"{path}.frames contains duplicate roles")
                    frame_roles.add(frame_role)
                    if frame_kind == "cell_frame":
                        if (
                            item["model_instance_id"] is not None
                            or item["source_link_name"] is not None
                        ):
                            raise _invalid(
                                f"{item_path} cell frame carries a model-local link"
                            )
                    elif frame_kind in {
                        "component_model_frame",
                        "passive_model_frame",
                    }:
                        instance_id = _text(
                            item["model_instance_id"],
                            f"{item_path}.model_instance_id",
                        )
                        link_name = _text(
                            item["source_link_name"],
                            f"{item_path}.source_link_name",
                        )
                        assert instance_id is not None and link_name is not None
                        occurrence = occurrences.get(instance_id)
                        if (
                            occurrence is None
                            or link_name not in occurrence["source_link_names"]
                        ):
                            raise _invalid(
                                f"{item_path} names an unknown source occurrence/link"
                            )
                    else:
                        raise _invalid(f"{item_path}.frame_kind is unsupported")
                else:
                    collision_role = _text(
                        item["collision_body_role_id"],
                        f"{item_path}.collision_body_role_id",
                    )
                    assert collision_role is not None
                    if collision_role in collision_roles:
                        raise _invalid(
                            f"{path}.collision_bodies contains duplicate roles"
                        )
                    collision_roles.add(collision_role)
                    instance_id = _text(
                        item["model_instance_id"],
                        f"{item_path}.model_instance_id",
                    )
                    assert instance_id is not None
                    links = _text_array(
                        item["source_link_names"], f"{item_path}.source_link_names"
                    )
                    occurrence = occurrences.get(instance_id)
                    if occurrence is None or not set(links).issubset(
                        set(occurrence["source_link_names"])
                    ):
                        raise _invalid(
                            f"{item_path} names an unknown source occurrence/link"
                        )


def verify_active_source_model(
    payload: Any,
    *,
    node_environment: VerifiedActiveNodeEnvironment,
) -> VerifiedActiveSourceModel:
    projection = _object(payload, "source model projection")
    _closed(projection, _PROJECTION_FIELDS, "source model projection")
    if projection.get("schema_version") != _PROJECTION_VERSION:
        raise _invalid("source model projection version is unsupported")
    _reject_private_names(projection)
    source = _source(projection["source"])
    for field in ("cell_id", "retained_bundle_id", "graph_revision"):
        _text(projection[field], field)
    if (
        projection["retained_bundle_id"]
        != node_environment.payload["retained_bundle_id"]
        or projection["graph_revision"] != node_environment.payload["graph_revision"]
    ):
        raise _invalid("source model and node environment select different bundles")
    node_source = node_environment.payload["source"]
    for field in (
        "selection_projection_generation",
        "selection_source_content_digest",
        "selection_source_generation",
        "selection_source_owner_epoch",
        "selection_source_owner_id",
    ):
        if source.get(field) != node_source.get(field):
            raise _invalid("source model and node environment selection epochs disagree")

    occurrences = _occurrences(projection["occurrences"])
    _targets(projection["targets"], occurrences)
    contract = projection["model_map_contract"]
    product_id = projection["model_map_product_id"]
    if occurrences:
        contract_row = _object(contract, "model_map_contract")
        _closed(
            contract_row,
            {"contract_id", "contract_revision"},
            "model_map_contract",
        )
        _text(contract_row["contract_id"], "model_map_contract.contract_id")
        _text(
            contract_row["contract_revision"],
            "model_map_contract.contract_revision",
        )
        _text(product_id, "model_map_product_id")
    elif contract is not None or product_id is not None:
        raise _invalid("an empty source model projection cannot name a model map")
    semantic = {key: value for key, value in projection.items() if key != "source"}
    if source["source_content_digest"] != _framed_digest(semantic):
        raise _invalid("source model source digest does not match the projection")
    return VerifiedActiveSourceModel(payload=projection)


def read_active_source_model(
    state: "ComposerState",
    *,
    node_environment: VerifiedActiveNodeEnvironment,
) -> VerifiedActiveSourceModel:
    client = state.headless_client
    if client is None:
        raise _invalid("Composer has no configured headless client")
    try:
        payload = client.source_model()
    except Exception as error:
        code = getattr(error, "code", None)
        if isinstance(code, str) and code:
            status_code = getattr(error, "status_code", None)
            raise ActiveSourceModelProjectionError(
                code,
                str(error),
                status_code if isinstance(status_code, int) else 503,
            ) from error
        raise
    return verify_active_source_model(payload, node_environment=node_environment)


def verify_active_source_model_context(
    state: "ComposerState",
    payload: Any,
) -> VerifiedActiveSourceModel:
    context = _object(payload, "source model context")
    _closed(
        context,
        {"node_environment", "schema_version", "source_model"},
        "source model context",
    )
    if context.get("schema_version") != _CONTEXT_VERSION:
        raise _invalid("source model context version is unsupported")
    node_environment = verify_active_node_environment(
        state,
        context["node_environment"],
    )
    return verify_active_source_model(
        context["source_model"],
        node_environment=node_environment,
    )


def read_active_source_model_context(
    state: "ComposerState",
) -> VerifiedActiveSourceModel:
    client = state.headless_client
    if client is None:
        raise _invalid("Composer has no configured headless client")
    try:
        payload = client.source_model_context()
    except Exception as error:
        code = getattr(error, "code", None)
        if isinstance(code, str) and code:
            status_code = getattr(error, "status_code", None)
            raise ActiveSourceModelProjectionError(
                code,
                str(error),
                status_code if isinstance(status_code, int) else 503,
            ) from error
        raise
    return verify_active_source_model_context(state, payload)


__all__ = [
    "ActiveSourceModelProjectionError",
    "VerifiedActiveSourceModel",
    "read_active_source_model_context",
    "read_active_source_model",
    "verify_active_source_model_context",
    "verify_active_source_model",
]
