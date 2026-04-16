"""Typed blackboard binding helpers shared by validation and runtime build paths."""

from __future__ import annotations

from typing import Any, Mapping

from manyforge_behavior.blackboard_namespace import (
    RuntimeFieldRef,
    make_runtime_field_ref,
)


def _extract_ref_id(value: Any, key_name: str) -> str | None:
    if isinstance(value, Mapping):
        ref = value.get(key_name)
        if isinstance(ref, str) and ref.strip():
            return ref.strip()
    return None


def namespace_ref_id(value: Any) -> str | None:
    return _extract_ref_id(value, "namespace_ref")


def key_ref_id(value: Any) -> str | None:
    return _extract_ref_id(value, "key_ref")


def field_ref_target(value: Any) -> tuple[str, str] | None:
    if not isinstance(value, Mapping):
        return None
    ref = value.get("field_ref")
    if not isinstance(ref, Mapping):
        return None
    namespace = ref.get("namespace")
    field_name = ref.get("field")
    if (
        isinstance(namespace, str)
        and namespace.strip()
        and isinstance(field_name, str)
        and field_name.strip()
    ):
        return namespace.strip(), field_name.strip()
    return None


def field_type_compatible(actual_type: str, expected_type: str | None) -> bool:
    if expected_type is None or not expected_type:
        return True
    return actual_type == expected_type


def validate_declared_field_ref(
    *,
    path_label: str,
    namespace_id: str,
    field_name: str,
    namespace_by_id: Mapping[str, Any],
    namespace_fields_by_id: Mapping[str, Mapping[str, Any]],
    expected_type: str | None = None,
    expected_contract: str | None = None,
) -> str | None:
    declared_namespace = namespace_by_id.get(namespace_id)
    if declared_namespace is None:
        return (
            f"{path_label} field_ref namespace '{namespace_id}' is not declared in "
            "program.blackboard.namespaces"
        )
    if expected_contract and declared_namespace.contract != expected_contract:
        return (
            f"{path_label} field_ref namespace '{namespace_id}' declares contract "
            f"'{declared_namespace.contract}', expected '{expected_contract}'"
        )
    declared_field = namespace_fields_by_id.get(namespace_id, {}).get(field_name)
    if declared_field is None:
        return (
            f"{path_label} field_ref '{namespace_id}.{field_name}' is not declared in "
            "program.blackboard.namespaces[].fields"
        )
    if not field_type_compatible(declared_field.type, expected_type):
        return (
            f"{path_label} field_ref '{namespace_id}.{field_name}' is declared as type "
            f"'{declared_field.type}', expected '{expected_type}'"
        )
    return None


def resolve_namespace_binding(
    *,
    param_name: str,
    value: Any,
    namespace_by_id: Mapping[str, Any],
    expected_contract: str | None,
) -> tuple[str | None, str | None]:
    namespace_id = namespace_ref_id(value)
    if namespace_id is None:
        return None, f"{param_name} must be a namespace_ref binding"
    declared_namespace = namespace_by_id.get(namespace_id)
    if declared_namespace is None:
        return (
            None,
            f"{param_name} namespace_ref '{namespace_id}' is not declared in "
            "program.blackboard.namespaces",
        )
    if expected_contract and declared_namespace.contract != expected_contract:
        return (
            None,
            f"{param_name} namespace_ref '{namespace_id}' is declared as contract "
            f"'{declared_namespace.contract}', expected '{expected_contract}'",
        )
    return declared_namespace.prefix, None


def resolve_key_binding(
    *,
    param_name: str,
    value: Any,
    key_by_id: Mapping[str, Any],
    expected_type: str | None,
) -> tuple[str | None, str | None]:
    key_id = key_ref_id(value)
    if key_id is None:
        return None, f"{param_name} must be a key_ref binding"
    declared_key = key_by_id.get(key_id)
    if declared_key is None:
        return (
            None,
            f"{param_name} key_ref '{key_id}' is not declared in program.blackboard.keys",
        )
    if expected_type and declared_key.type != expected_type:
        return (
            None,
            f"{param_name} key_ref '{key_id}' is declared as type "
            f"'{declared_key.type}', expected '{expected_type}'",
        )
    return declared_key.key, None


def resolve_field_binding(
    *,
    param_name: str,
    value: Any,
    namespace_by_id: Mapping[str, Any],
    namespace_fields_by_id: Mapping[str, Mapping[str, Any]],
    expected_type: str | None,
    expected_contract: str | None,
    binding_label: str = "field_ref",
) -> tuple[RuntimeFieldRef | None, str | None]:
    target = field_ref_target(value)
    if target is None:
        return None, f"{param_name} must be a {binding_label} binding"
    namespace_id, field_name = target
    field_ref_error = validate_declared_field_ref(
        path_label=param_name,
        namespace_id=namespace_id,
        field_name=field_name,
        namespace_by_id=namespace_by_id,
        namespace_fields_by_id=namespace_fields_by_id,
        expected_type=expected_type,
        expected_contract=expected_contract,
    )
    if field_ref_error is not None:
        return None, field_ref_error
    declared_namespace = namespace_by_id[namespace_id]
    return make_runtime_field_ref(declared_namespace.prefix, field_name), None


def _resolve_nested_bindings(
    value: Any,
    *,
    parameter_name: str,
    namespace_by_id: Mapping[str, Any],
    namespace_fields_by_id: Mapping[str, Mapping[str, Any]],
    key_by_id: Mapping[str, Any],
) -> Any:
    if isinstance(value, dict):
        if set(value.keys()) == {"field_ref"} and isinstance(value.get("field_ref"), dict):
            resolved, error = resolve_field_binding(
                param_name=parameter_name,
                value=value,
                namespace_by_id=namespace_by_id,
                namespace_fields_by_id=namespace_fields_by_id,
                expected_type=None,
                expected_contract=None,
            )
            if error is not None or resolved is None:
                raise ValueError(error or f"Invalid field_ref for parameter '{parameter_name}'")
            return resolved
        if set(value.keys()) == {"key_ref"}:
            resolved, error = resolve_key_binding(
                param_name=parameter_name,
                value=value,
                key_by_id=key_by_id,
                expected_type=None,
            )
            if error is not None or resolved is None:
                raise ValueError(error or f"Invalid key_ref for parameter '{parameter_name}'")
            return resolved
        return {
            item_key: _resolve_nested_bindings(
                item_value,
                parameter_name=parameter_name,
                namespace_by_id=namespace_by_id,
                namespace_fields_by_id=namespace_fields_by_id,
                key_by_id=key_by_id,
            )
            for item_key, item_value in value.items()
        }
    if isinstance(value, list):
        return [
            _resolve_nested_bindings(
                item,
                parameter_name=parameter_name,
                namespace_by_id=namespace_by_id,
                namespace_fields_by_id=namespace_fields_by_id,
                key_by_id=key_by_id,
            )
            for item in value
        ]
    return value


def resolve_runtime_blackboard_bindings(
    entry: Any,
    params: dict[str, Any],
    *,
    blackboard_namespaces: Mapping[str, Any],
    blackboard_keys: Mapping[str, Any],
) -> dict[str, Any]:
    namespace_fields_by_id = {
        namespace_id: {field.name: field for field in getattr(namespace, "fields", [])}
        for namespace_id, namespace in blackboard_namespaces.items()
    }

    resolved = dict(params)
    for parameter in getattr(entry, "parameters", []):
        raw_value = resolved.get(parameter.name)
        if raw_value is None:
            continue
        if parameter.blackboard_role in {"namespace_writer", "namespace_reader"}:
            if namespace_ref_id(raw_value) is not None:
                value, error = resolve_namespace_binding(
                    param_name=parameter.name,
                    value=raw_value,
                    namespace_by_id=blackboard_namespaces,
                    expected_contract=None,
                )
                if error is not None or value is None:
                    raise ValueError(error or f"Invalid namespace_ref for parameter '{parameter.name}'")
                resolved[parameter.name] = value
        if parameter.blackboard_role in {"key_writer", "key_reader"}:
            if key_ref_id(raw_value) is not None:
                value, error = resolve_key_binding(
                    param_name=parameter.name,
                    value=raw_value,
                    key_by_id=blackboard_keys,
                    expected_type=None,
                )
                if error is not None or value is None:
                    raise ValueError(error or f"Invalid key_ref for parameter '{parameter.name}'")
                resolved[parameter.name] = value
            elif parameter.blackboard_role == "key_reader" and field_ref_target(raw_value) is not None:
                value, error = resolve_field_binding(
                    param_name=parameter.name,
                    value=raw_value,
                    namespace_by_id=blackboard_namespaces,
                    namespace_fields_by_id=namespace_fields_by_id,
                    expected_type=None,
                    expected_contract=None,
                )
                if error is not None or value is None:
                    raise ValueError(error or f"Invalid field_ref for parameter '{parameter.name}'")
                resolved[parameter.name] = value
        if parameter.blackboard_role == "field_reader" and field_ref_target(raw_value) is not None:
            value, error = resolve_field_binding(
                param_name=parameter.name,
                value=raw_value,
                namespace_by_id=blackboard_namespaces,
                namespace_fields_by_id=namespace_fields_by_id,
                expected_type=None,
                expected_contract=None,
            )
            if error is not None or value is None:
                raise ValueError(error or f"Invalid field_ref for parameter '{parameter.name}'")
            resolved[parameter.name] = value
        resolved[parameter.name] = _resolve_nested_bindings(
            resolved[parameter.name],
            parameter_name=parameter.name,
            namespace_by_id=blackboard_namespaces,
            namespace_fields_by_id=namespace_fields_by_id,
            key_by_id=blackboard_keys,
        )
    return resolved
