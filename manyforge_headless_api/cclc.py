"""Bounded same-user client for the sole CCLC owner."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from pathlib import Path
import socket
import unicodedata
from typing import Any, Protocol

import rfc8785

from manyforge_behavior.json_scalars import MAXIMUM_SAFE_INTEGER, nonnegative_int64

from .native import HeadlessApiError, strict_json_object
from .close_wire import (
    LifecycleTargetRetirementIntent,
    LifecycleTargetRetirementScope,
    retirement_offer,
    retirement_result,
    settlement_result,
)
from .private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)


CCLC_SOCKET_FILE = "cclc-command.sock"
# Stays v1 for the coordinated unreleased 128/130 cut (agreed with Track 2).
CCLC_OPEN_REQUEST_VERSION = "manyforge.cclc_open_request.v1"
CCLC_OPEN_RESPONSE_VERSION = "manyforge.cclc_open_response.v1"
MAX_CCLC_REQUEST_BYTES = 4 * 1024 * 1024
MAX_CCLC_RESPONSE_BYTES = 8 * 1024 * 1024
_RESPONSE_FIELDS = frozenset({"api_version", "command", "error", "ok", "result"})


class CCLCOwnerRefusal(HeadlessApiError):
    """A validated CCLC ``ok: false`` reply: the owner received the request.

    It proves no effect only when the owner says so: ``details.effect ==
    "not_started"`` (CCLC's known-unstarted rejection). Every other
    ``HeadlessApiError`` from the exchange is a send, receive or
    response-validation failure, after which the owner's outcome is unknown.
    """


class CCLCOpenPort(Protocol):
    """Caller-neutral lifecycle/route surface; implementations own no truth."""

    def owner_identity(self) -> dict[str, Any]: ...

    def lifecycle_target_retirement_offer(self) -> dict[str, Any]: ...

    def retire_lifecycle_targets(self, intent: LifecycleTargetRetirementIntent) -> dict[str, Any]: ...

    def lifecycle_target_retirement_result(self, intent: LifecycleTargetRetirementIntent) -> dict[str, Any] | None: ...

    def lifecycle_owner_settlement(self, scope: LifecycleTargetRetirementScope) -> dict[str, Any]: ...

    def component_snapshot(self, component_id: str) -> dict[str, Any]: ...

    def component_lifecycle_snapshot(self, component_id: str) -> dict[str, Any]: ...

    def route_templates(self, component_id: str) -> dict[str, Any]: ...

    def submit_lifecycle(self, intent: Mapping[str, Any]) -> dict[str, Any]: ...

    def lifecycle_status(self, transaction_id: str) -> dict[str, Any] | None: ...

    def lifecycle_request_result(
        self, intent: Mapping[str, Any]
    ) -> dict[str, Any] | None: ...

    def submit_route(self, intent: Mapping[str, Any]) -> dict[str, Any]: ...

    def route_request_result(
        self, intent: Mapping[str, Any]
    ) -> dict[str, Any] | None: ...

    def route_status(self, transaction_id: str) -> dict[str, Any] | None: ...

    def incident_events(self, after_sequence: int, limit: int) -> dict[str, Any]: ...


def _canonical_json(value: Mapping[str, Any], *, error_code: str, label: str) -> bytes:
    try:
        return rfc8785.dumps(dict(value))
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
        raise HeadlessApiError(
            error_code,
            f"{label} is not canonically encodable: {exc}",
        ) from exc


_TARGET_STATES = frozenset({"active", "prepared", "disabled"})
_TARGET_ORIGINS = frozenset({"startup", "operator", "program"})
_PROGRESS_PHASES = frozenset(
    {
        "satisfied",
        "waiting_for_prerequisites",
        "transition_in_progress",
        "outcome_uncertain",
        "blocked",
        "cancelled",
    }
)
_PROGRESS_INTENTS = frozenset({"prepare", "enable", "disable", "recover"})
_BLOCKING_CAUSE_CLASSES = frozenset(
    {
        "none",
        "availability",
        "emergency_stop",
        "protective_stop",
        "safeguard_stop",
        "fault",
        "configuration",
        "unknown",
        "collision",
        "external_control",
        "provider_lost",
    }
)
_INCIDENT_CAUSE_CLASSES = _BLOCKING_CAUSE_CLASSES - {"none", "availability"}
_MAX_BLOCKING_REASON_BYTES = 1024
_MAX_INCIDENT_ID_BYTES = 128
_MAX_INCIDENT_SUMMARY_BYTES = 1024
_MAX_VENDOR_CODES = 16
_MAX_VENDOR_CODE_BYTES = 64
_MAX_DIAGNOSTIC_BYTES = 512 * 1024
# Same existing whole selected-runtime packet ceiling as Close/Stop identities.
_MAX_LAUNCH_IDENTITY_BYTES = 256 * 1024
_MAX_DIAGNOSTIC_TAIL_BYTES = 128 * 1024
INCIDENT_EVENT_KINDS = frozenset(
    {"component_incident", "component_incident_acknowledged"}
)
MAX_INCIDENT_EVENT_PAGE = 64


def _is_int(value: Any) -> bool:
    return type(value) is int


def _is_owner_ns(value: Any) -> bool:
    """Owner-clock nanoseconds: full-width nonnegative int64 scalar.

    A JSON number up to 2**53 - 1 or canonical decimal text up to 2**63 - 1
    (the producer spells larger values as text). The value is forwarded
    unchanged; only its spelling is checked.
    """

    try:
        nonnegative_int64(value)
    except ValueError:
        return False
    return True


def _member(value: Any, allowed: frozenset[str]) -> bool:
    return isinstance(value, str) and value in allowed


def _bounded_text(value: Any, maximum: int) -> bool:
    return (
        isinstance(value, str)
        and bool(value)
        and len(value.encode("utf-8")) <= maximum
    )


def _diagnostic_text(value: Any, maximum: int, *, nonempty: bool = False) -> bool:
    return (
        isinstance(value, str)
        and (not nonempty or bool(value))
        and unicodedata.is_normalized("NFC", value)
        and len(value.encode("utf-8")) <= maximum
    )


def _is_owner_uint64(value: Any) -> bool:
    if _is_int(value):
        return 0 <= value <= MAXIMUM_SAFE_INTEGER
    return (
        isinstance(value, str)
        and 1 <= len(value) <= 20
        and value.isascii()
        and value.isdigit()
        and (len(value) == 1 or value[0] != "0")
        and int(value) <= 2**64 - 1
    )


def _closed(value: Any, fields: set[str]) -> bool:
    return isinstance(value, dict) and set(value) == fields


_COMPONENT_FIELDS = {
    "active_public_pointer_digest", "component_lifecycle_generation",
    "component_lifecycle_state", "controllable_component_id", "domains",
    "graph_revision", "lifecycle_progress", "lifecycle_target",
    "provider_occurrences", "publication_generation", "retained_bundle_id",
    "snapshot_digest", "readiness",
}
_OCCURRENCE_FIELDS = {
    "control_domain_id", "provider_instance_id", "provider_manifest_id",
    "provider_manifest_revision", "provider_runtime_unit_id",
    "provider_runtime_epoch", "lifecycle_plan_catalog_digest",
}
_LIFECYCLE_STATES = frozenset({
    "undeclared", "declared", "prepared", "active", "degraded", "faulted",
    "disabling", "disabled",
})


_PROVIDER_DOMAIN_FIELDS = {
    "active_control_configuration", "control_domain_id", "control_domain_revision", "kind",
    "provider", "provider_runtime_epoch", "resources",
}
_PROVIDER_FIELDS = {
    "controllable_component_id", "lifecycle_command_timeout_ns", "lifecycle_plan_catalog_digest",
    "provider_instance_id", "provider_manifest_id", "provider_manifest_revision",
    "provider_runtime_unit_id", "signal_configuration_digest", "signal_names",
}
_PROVIDER_ACTIVE_FIELDS = {
    "active_control_configuration_generation", "active_control_configuration_id",
    "signal_configuration_digest", "source_runtime_epoch", "source_runtime_unit_id",
    "source_sequence",
}
_MAX_LIFECYCLE_TIMEOUT_NS = 3_600_000_000_000
# Every response domain names its meaning; `resources` is Core resource_ids():
# controller = current control-binding resources, io_provider = sealed logical outputs.
_DOMAIN_KINDS = frozenset({"controller", "io_provider"})


def _sha256_digest(value: Any) -> bool:
    return (isinstance(value, str) and len(value) == 71 and value.startswith("sha256:")
            and all(digit in "0123456789abcdef" for digit in value[7:]))


def _provider_domain(domain: Any) -> bool:
    """Mirror the Core's model-free I/O provider domain (no bindings, topology or kinematics)."""
    def identity(value: Any) -> bool:
        return _diagnostic_text(value, 128, nonempty=True)

    if not _closed(domain, _PROVIDER_DOMAIN_FIELDS) or domain["kind"] != "io_provider":
        return False
    provider = domain["provider"]
    if not (_closed(provider, _PROVIDER_FIELDS)
            and all(identity(domain[key]) for key in
                    ("control_domain_id", "control_domain_revision", "provider_runtime_epoch"))
            and all(identity(provider[key]) for key in
                    ("controllable_component_id", "provider_runtime_unit_id", "provider_instance_id",
                     "provider_manifest_id"))
            and all(_sha256_digest(provider[key]) for key in
                    ("provider_manifest_revision", "lifecycle_plan_catalog_digest",
                     "signal_configuration_digest"))
            and _is_int(provider["lifecycle_command_timeout_ns"])
            and 0 < provider["lifecycle_command_timeout_ns"] <= _MAX_LIFECYCLE_TIMEOUT_NS
            and _canonical_rows(provider["signal_names"], 4096, lambda name: name)
            and bool(provider["signal_names"])
            and all(_bounded_text(name, 128) for name in provider["signal_names"])
            and _canonical_rows(domain["resources"], 4096, lambda resource: resource)
            and all(identity(resource) for resource in domain["resources"])):
        return False
    active = domain["active_control_configuration"]
    return active is None or (
        _closed(active, _PROVIDER_ACTIVE_FIELDS)
        and identity(active["active_control_configuration_id"])
        and all(_is_int(active[key]) and 1 <= active[key] <= MAXIMUM_SAFE_INTEGER
                for key in ("active_control_configuration_generation", "source_sequence"))
        and active["source_runtime_unit_id"] == provider["provider_runtime_unit_id"]
        and active["source_runtime_epoch"] == domain["provider_runtime_epoch"]
        and active["signal_configuration_digest"] == provider["signal_configuration_digest"])


def _readiness_error(value: Any, maximum_details: int = 64) -> bool:
    return value is None or (
        _closed(value, {"code", "message", "details"})
        and _diagnostic_text(value["code"], 128, nonempty=True)
        and _diagnostic_text(value["message"], 1024)
        and isinstance(value["details"], dict) and len(value["details"]) <= maximum_details
        and all(_diagnostic_text(key, 64, nonempty=True)
                and _diagnostic_text(detail, 256)
                for key, detail in value["details"].items())
    )


def _canonical_rows(value: Any, maximum: int, key: Callable) -> bool:
    if not isinstance(value, list) or len(value) > maximum:
        return False
    keys = [key(row) for row in value]
    return all(left < right for left, right in zip(keys, keys[1:]))


def _validate_component_readiness(
    snapshot: Mapping[str, Any], *, lifecycle_only: bool = False
) -> None:
    """Validate the paired closed owner read; never compute readiness."""
    def require(valid: bool) -> None:
        if not valid:
            raise HeadlessApiError("cclc_open_response_invalid", "CCLC component readiness is invalid")

    try:
        counters = {"dropped_output_lines", "dropped_output_bytes"}
        optional = (counters if counters & set(snapshot) else set()) | (
            {"stream_health"} if "stream_health" in snapshot else set()) | (
            {"active_target_retired_by_incident"}
            if "active_target_retired_by_incident" in snapshot else set())
        fields = (_COMPONENT_FIELDS - {"lifecycle_progress", "lifecycle_target", "readiness"}
                  if lifecycle_only else _COMPONENT_FIELDS | optional)
        require(set(snapshot) == fields)
        state = snapshot["component_lifecycle_state"]
        require(_member(state, _LIFECYCLE_STATES))
        for name in ("controllable_component_id", "graph_revision", "retained_bundle_id",
                     "snapshot_digest", "active_public_pointer_digest"):
            require(_diagnostic_text(snapshot[name], 256, nonempty=True))
        for name in ("component_lifecycle_generation", "publication_generation"):
            require(_is_int(snapshot[name]) and 1 <= snapshot[name] <= MAXIMUM_SAFE_INTEGER)
        domains = snapshot["domains"]
        require(_canonical_rows(domains, 256, lambda row: row["control_domain_id"]))
        require(bool(domains))
        revisions = {}
        provider_domains = []
        for domain in domains:
            require(isinstance(domain, dict) and _member(domain.get("kind"), _DOMAIN_KINDS))
            if domain["kind"] == "io_provider":
                require(_provider_domain(domain)
                    and domain["provider"]["controllable_component_id"] == snapshot["controllable_component_id"])
                provider_domains.append(domain)
                revisions[domain["control_domain_id"]] = domain["control_domain_revision"]
                continue
            require(_closed(domain, {"active_control_configuration", "control_domain_id",
                "control_domain_revision", "controller_topology_id", "current_control_bindings",
                "kind", "resources", "topology_revision"}))
            for name in ("control_domain_id", "control_domain_revision", "controller_topology_id", "topology_revision"):
                require(_diagnostic_text(domain[name], 256, nonempty=True))
            bindings = domain["current_control_bindings"]
            require(_canonical_rows(bindings, 4096, lambda binding: binding["control_binding_id"]) and bool(bindings))
            for binding in bindings:
                require(_closed(binding, {"control_binding_id", "control_binding_revision", "control_route_id"})
                    and all(_diagnostic_text(value, 256, nonempty=True) for value in binding.values()))
            resources = domain["resources"]
            require(_canonical_rows(resources, 4096, lambda resource: resource) and bool(resources)
                and all(_diagnostic_text(resource, 256, nonempty=True) for resource in resources))
            active = domain["active_control_configuration"]
            if active is not None:
                require(_closed(active, {"activation_set_id", "active_control_configuration_generation",
                    "active_control_configuration_id", "binding_routes", "control_domain_id", "control_domain_revision",
                    "controller_topology_id", "source_runtime_epoch", "source_runtime_unit_id", "source_sequence", "state", "topology_revision"}))
                for key in ("activation_set_id", "active_control_configuration_id", "control_domain_id", "control_domain_revision",
                            "controller_topology_id", "source_runtime_epoch", "source_runtime_unit_id", "topology_revision"):
                    require(_diagnostic_text(active[key], 256, nonempty=True))
                for key in ("active_control_configuration_generation", "source_sequence"):
                    require(_is_int(active[key]) and 1 <= active[key] <= MAXIMUM_SAFE_INTEGER)
                require(_member(active["state"], frozenset({"reconciled", "external_or_unknown_unreconciled"})))
                routes = active["binding_routes"]
                require(_canonical_rows(routes, 4096, lambda route: route["control_binding_id"]) and bool(routes))
                for route in routes:
                    require(_closed(route, {"control_binding_id", "control_binding_revision", "control_route_id"})
                        and all(_diagnostic_text(value, 256, nonempty=True) for value in route.values()))
            revisions[domain["control_domain_id"]] = domain["control_domain_revision"]
        providers = snapshot["provider_occurrences"]
        require(_canonical_rows(providers, 4096, lambda row: (row["control_domain_id"], row["provider_instance_id"])))
        for provider in providers:
            require(_closed(provider, _OCCURRENCE_FIELDS))
            require(all(_diagnostic_text(value, 256, nonempty=True) for value in provider.values()))
        for domain in provider_domains:
            identity = domain["provider"]
            require([{key: row[key] for key in _OCCURRENCE_FIELDS - {"control_domain_id"}}
                     for row in providers if row["control_domain_id"] == domain["control_domain_id"]] == [{
                "provider_instance_id": identity["provider_instance_id"],
                "provider_manifest_id": identity["provider_manifest_id"],
                "provider_manifest_revision": identity["provider_manifest_revision"],
                "provider_runtime_unit_id": identity["provider_runtime_unit_id"],
                "provider_runtime_epoch": domain["provider_runtime_epoch"],
                "lifecycle_plan_catalog_digest": identity["lifecycle_plan_catalog_digest"]}])
        if lifecycle_only:
            return
        readiness = snapshot["readiness"]
        require(_closed(readiness, {"request_owner_epoch", "evaluated_at_ns", "next_intent",
            "requested_state", "ready", "error", "domains", "measured_state"}))
        require(_diagnostic_text(readiness["request_owner_epoch"], 256, nonempty=True))
        require(_is_owner_ns(readiness["evaluated_at_ns"]))
        intent, requested = (("prepare", "prepared") if state in {"declared", "disabled"}
            else ("enable", "active") if state == "prepared" else (None, None))
        require(readiness["next_intent"] == intent and readiness["requested_state"] == requested)
        require(type(readiness["ready"]) is bool and _readiness_error(readiness["error"]))
        require(readiness["ready"] or readiness["error"] is not None)
        rows = readiness["domains"]
        ordered_fields = ("control_domain_id", "control_domain_revision", "provider_instance_id",
            "provider_manifest_id", "provider_manifest_revision", "provider_runtime_unit_id",
            "provider_runtime_epoch", "lifecycle_plan_catalog_digest")
        require(_canonical_rows(rows, 256, lambda row: tuple(row["provider_occurrence"][key] for key in ordered_fields)))
        require(len(rows) == len(providers))
        for row in rows:
            fields = {"provider_occurrence", "health", "decision_kind", "error"}
            if "has_activation_step" in row:
                fields.add("has_activation_step")
                require(type(row["has_activation_step"]) is bool)
            require(_closed(row, fields))
            occurrence = row["provider_occurrence"]
            require(_closed(occurrence, _OCCURRENCE_FIELDS | {"control_domain_revision"}))
            require(all(_diagnostic_text(value, 128, nonempty=True) for value in occurrence.values()))
            require(revisions.get(occurrence["control_domain_id"]) == occurrence["control_domain_revision"])
            require({key: occurrence[key] for key in _OCCURRENCE_FIELDS} in providers)
            require(_readiness_error(row["error"]))
            health, decision = row["health"], row["decision_kind"]
            require(decision is None or _member(decision, frozenset({"already_satisfied", "dispatch_primitive"})))
            if health is None:
                require(decision is None and row["error"] is not None)
            else:
                require(_closed(health, {"observed_state", "facts", "diagnostic"}))
                require(_member(health["observed_state"], _LIFECYCLE_STATES) and _readiness_error(health["diagnostic"], 8))
                require(_canonical_rows(health["facts"], 256, lambda fact: fact["lifecycle_fact_id"]))
                for fact in health["facts"]:
                    require(_closed(fact, {"lifecycle_fact_id", "value"}))
                    require(_diagnostic_text(fact["lifecycle_fact_id"], 128, nonempty=True))
                    value = fact["value"]
                    kind = value["kind"]
                    if kind == "unknown":
                        require(_closed(value, {"kind"}))
                    elif kind == "boolean":
                        require(_closed(value, {"kind", "boolean_value"}) and type(value["boolean_value"]) is bool)
                    elif kind == "integer":
                        require(_closed(value, {"kind", "integer_value"}) and _is_int(value["integer_value"])
                            and abs(value["integer_value"]) <= MAXIMUM_SAFE_INTEGER)
                    elif kind == "token":
                        require(_closed(value, {"kind", "token_value"})
                            and _diagnostic_text(value["token_value"], 128, nonempty=True))
                    else:
                        require(False)
                if intent is not None:
                    require((row["error"] is None) == (decision is not None))
            if intent is None:
                require(decision is None)
        measured = readiness["measured_state"]
        require(_closed(measured, {"required", "available", "error"}) and _readiness_error(measured["error"]))
        require((measured["required"] is False and measured["available"] is None and measured["error"] is None)
            or (measured["required"] is None and measured["available"] is None and measured["error"] is not None)
            or (measured["required"] is True and type(measured["available"]) is bool
                and (measured["error"] is None) == measured["available"]))
        if readiness["ready"]:
            require(readiness["error"] is None
                and all(row["health"] is not None and row["error"] is None for row in rows)
                and (intent is not None or state == "active"))
    except (KeyError, TypeError, ValueError, UnicodeError) as exc:
        raise HeadlessApiError("cclc_open_response_invalid", "CCLC component readiness is invalid") from exc


def _valid_diagnostic(value: Any) -> bool:
    """Validate observation-only wire facts; the Core owns the total row budget."""

    if value is None:
        return True
    try:
        if not _closed(value, {"error", "snapshot"}):
            return False
        error = value["error"]
        if not _closed(error, {"code", "message", "details"}):
            return False
        details = error["details"]
        if (
            not _diagnostic_text(error["code"], 128, nonempty=True)
            or not _diagnostic_text(error["message"], 1024)
            or not isinstance(details, dict)
            or len(details) > 8
            or not all(
                _diagnostic_text(key, 64, nonempty=True)
                and _diagnostic_text(detail, 256)
                for key, detail in details.items()
            )
        ):
            return False
        snapshot = value["snapshot"]
        if snapshot is None:
            return len(rfc8785.dumps(value)) <= _MAX_DIAGNOSTIC_BYTES
        groups = {
            "exit": {"status", "signal", "core_dumped", "observed_utc"},
            "context": {
                "controllable_component_id", "request_context", "runtime_unit_id",
                "runtime_epoch", "retained_bundle_id", "graph_revision", "generator_identity",
                "launch_identity",
            },
            # ros2_control launch facts (always null for an I/O provider), not the
            # CCLC owner readiness verdict.
            "launch_readiness": {
                "known", "controller_manager_present", "controller_observed",
                "controller_loaded", "controller_bound", "arm_controller_active",
                "hardware_ready", "joint_sample_available", "binding_epoch",
                "sample_age_ns", "captured_utc",
            },
            "settings": {
                "argv", "controller_profile", "runtime_namespace", "arm_controller_name",
                "gripper_controller_name", "frame_prefix", "controllers_yaml",
                "retained_configuration_json",
            },
            "generated": {"robot_description_sha256", "controllers_sha256"},
            "output": {
                "tail", "line_count", "truncated", "dropped_forward_lines",
                "dropped_forward_bytes", "invalid_text_bytes",
            },
        }
        if not _closed(snapshot, set(groups) | {"core_dump_link"}) or any(
            not _closed(snapshot[group], fields) for group, fields in groups.items()
        ):
            return False
        exit_fact = snapshot["exit"]
        if (
            any(
                fact is not None and (not _is_int(fact) or not 0 <= fact <= 255)
                for fact in (exit_fact["status"], exit_fact["signal"])
            )
            or type(exit_fact["core_dumped"]) is not bool
            or not _diagnostic_text(exit_fact["observed_utc"], 128, nonempty=True)
        ):
            return False
        for key, fact in snapshot["context"].items():
            maximum = (
                _MAX_LAUNCH_IDENTITY_BYTES if key == "launch_identity"
                else 1024 if key in {"request_context", "generator_identity"} else 128
            )
            if fact is not None and not _diagnostic_text(fact, maximum):
                return False
        for key, fact in snapshot["launch_readiness"].items():
            if fact is None:
                continue
            if key == "binding_epoch":
                valid = _is_owner_uint64(fact)
            elif key == "sample_age_ns":
                valid = _is_owner_ns(fact)
            elif key == "captured_utc":
                valid = _diagnostic_text(fact, 128)
            else:
                valid = type(fact) is bool
            if not valid:
                return False
        settings = snapshot["settings"]
        if not isinstance(settings["argv"], list) or not all(
            _diagnostic_text(argument, _MAX_DIAGNOSTIC_BYTES)
            for argument in settings["argv"]
        ):
            return False
        if any(
            fact is not None and not _diagnostic_text(fact, _MAX_DIAGNOSTIC_BYTES)
            for key, fact in settings.items() if key != "argv"
        ):
            return False
        for fact in snapshot["generated"].values():
            if fact is not None and (
                not isinstance(fact, str) or len(fact) != 71
                or not fact.startswith("sha256:")
                or any(character not in "0123456789abcdef" for character in fact[7:])
            ):
                return False
        link = snapshot["core_dump_link"]
        if link is not None and not _diagnostic_text(link, 1024):
            return False
        output = snapshot["output"]
        tail = output["tail"]
        if not _diagnostic_text(tail, _MAX_DIAGNOSTIC_TAIL_BYTES):
            return False
        lines = tail.count("\n") + (1 if tail and not tail.endswith("\n") else 0)
        if (
            not _is_int(output["line_count"])
            or not 0 <= output["line_count"] <= 2000
            or output["line_count"] != lines
            or type(output["truncated"]) is not bool
            or not all(_is_owner_uint64(output[key]) for key in (
                "dropped_forward_lines", "dropped_forward_bytes", "invalid_text_bytes",
            ))
            or len(rfc8785.dumps(tail)) > _MAX_DIAGNOSTIC_TAIL_BYTES
        ):
            return False
        return len(rfc8785.dumps(value)) <= _MAX_DIAGNOSTIC_BYTES
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError):
        return False


def _valid_incident(incident: Any) -> bool:
    """One open component incident (addendum 2 contract item 2), or null."""

    if incident is None:
        return True
    if not isinstance(incident, dict) or set(incident) - {"diagnostic"} != {
        "acknowledged",
        "cause_class",
        "detected_at_ns",
        "incident_id",
        "summary",
        "vendor_codes",
    }:
        return False
    codes = incident["vendor_codes"]
    summary = incident["summary"]
    return (
        _bounded_text(incident["incident_id"], _MAX_INCIDENT_ID_BYTES)
        and _member(incident["cause_class"], _INCIDENT_CAUSE_CLASSES)
        and isinstance(codes, list)
        and len(codes) <= _MAX_VENDOR_CODES
        and all(_bounded_text(code, _MAX_VENDOR_CODE_BYTES) for code in codes)
        and _is_owner_ns(incident["detected_at_ns"])
        and _bounded_text(summary, _MAX_INCIDENT_SUMMARY_BYTES)
        and unicodedata.is_normalized("NFC", summary)
        and incident["acknowledged"] is False
        and _valid_diagnostic(incident.get("diagnostic"))
    )


_STREAM_STATES = frozenset({"live", "stale", "absent"})
_STREAM_SEVERITIES = frozenset({"fault", "blocking", "warning"})
_STREAM_RECOVERIES = frozenset({"ack", "auto"})
_STREAM_TIMESTAMP_SOURCES = frozenset({"message_stamp", "arrival"})
_STREAM_ROW_FIELDS = {
    "acknowledgement_required", "age_ns", "budget_ns", "error", "recovery",
    "severity", "state", "stream_id", "timestamp_source",
}


def _valid_stream_health(value: Any) -> bool:
    """CP9 per-stream health rows (contract (c)), closed and sorted."""

    def row_valid(row: Any) -> bool:
        return (
            _closed(row, _STREAM_ROW_FIELDS)
            and _diagnostic_text(row["stream_id"], 64, nonempty=True)
            and _member(row["state"], _STREAM_STATES)
            and _member(row["severity"], _STREAM_SEVERITIES)
            and _member(row["recovery"], _STREAM_RECOVERIES)
            and _member(row["timestamp_source"], _STREAM_TIMESTAMP_SOURCES)
            and type(row["acknowledgement_required"]) is bool
            and _is_int(row["budget_ns"]) and 1 <= row["budget_ns"] <= MAXIMUM_SAFE_INTEGER
            and (row["age_ns"] is None
                 or (_is_int(row["age_ns"]) and 0 <= row["age_ns"] <= MAXIMUM_SAFE_INTEGER))
            and _readiness_error(row["error"])
        )

    try:
        return (
            _canonical_rows(value, 256, lambda row: row["stream_id"])
            and bool(value)
            and all(row_valid(row) for row in value)
        )
    except (KeyError, TypeError):
        return False


def _invalid_lifecycle_fact(field: str) -> HeadlessApiError:
    return HeadlessApiError(
        "cclc_open_response_invalid",
        f"CCLC component snapshot {field} is invalid",
    )


def _validate_lifecycle_target_facts(snapshot: Mapping[str, Any]) -> None:
    """Fail closed on a malformed requested target or target progress.

    The snapshot is otherwise forwarded unchanged; these two owner-issued
    facts carry closed enums that Composer presents, so Headless rejects an
    unknown key or spelling instead of forwarding an unpresentable state.
    Available output-loss counters arrive as one exact uint64 pair.
    """

    if "stream_health" in snapshot and not _valid_stream_health(snapshot["stream_health"]):
        raise _invalid_lifecycle_fact("stream_health")
    # M6: present, and true, only while an incident-retired Active target waits.
    if snapshot.get("active_target_retired_by_incident", True) is not True:
        raise _invalid_lifecycle_fact("active_target_retired_by_incident")
    output_counter_fields = {"dropped_output_lines", "dropped_output_bytes"}
    present_output_counters = output_counter_fields.intersection(snapshot)
    if present_output_counters and (
        present_output_counters != output_counter_fields
        or not all(_is_owner_uint64(snapshot[field]) for field in output_counter_fields)
    ):
        raise _invalid_lifecycle_fact("dropped_output_lines/dropped_output_bytes")
    if "lifecycle_target" in snapshot:
        target = snapshot["lifecycle_target"]
        if target is not None and (
            not isinstance(target, dict)
            or set(target) != {"origin", "requested_state", "target_generation"}
            or not _member(target["requested_state"], _TARGET_STATES)
            or not _member(target["origin"], _TARGET_ORIGINS)
            or not _is_int(target["target_generation"])
            or not 1 <= target["target_generation"] <= MAXIMUM_SAFE_INTEGER
        ):
            raise _invalid_lifecycle_fact("lifecycle_target")
    if "lifecycle_progress" in snapshot:
        progress = snapshot["lifecycle_progress"]
        if progress is None:
            return
        if (
            not isinstance(progress, dict)
            or set(progress)
            != {
                "blocking_cause_class",
                "blocking_reason",
                "delayed",
                "incident",
                "next_intent",
                "phase",
                "waiting_since_ns",
            }
            or not _member(progress["phase"], _PROGRESS_PHASES)
            or (
                progress["next_intent"] is not None
                and not _member(progress["next_intent"], _PROGRESS_INTENTS)
            )
            or not _member(
                progress["blocking_cause_class"], _BLOCKING_CAUSE_CLASSES
            )
            or (
                progress["blocking_reason"] is not None
                and not _bounded_text(
                    progress["blocking_reason"], _MAX_BLOCKING_REASON_BYTES
                )
            )
            or (
                progress["waiting_since_ns"] is not None
                and not _is_owner_ns(progress["waiting_since_ns"])
            )
            or type(progress["delayed"]) is not bool
            or not _valid_incident(progress["incident"])
        ):
            raise _invalid_lifecycle_fact("lifecycle_progress")


def _validate_close_incident_intent(intent: Mapping[str, Any]) -> None:
    close = intent.get("close_incident_recovery")
    if close is not None and (
            intent.get("intent") != "recover"
            or intent.get("reason") != "cell closed by operator"
            or not isinstance(close, dict)
            or set(close) != {"expected_incident_id"}
            or not _bounded_text(close["expected_incident_id"], _MAX_INCIDENT_ID_BYTES)):
        raise HeadlessApiError("cclc_open_request_invalid", "Close incident recovery intent is invalid")


def _validate_incident_recovery(
    result: Mapping[str, Any], intent: Mapping[str, Any]
) -> None:
    """Fail closed on a malformed authorization-only incident Recover.

    CCLC answers the operator's Recover of an open incident without a
    lifecycle transaction: `acceptance` and `status` are null and
    `incident_recovery` names the closed incident. The key is optional and
    absent for every other request.
    """

    close = intent.get("close_incident_recovery")
    if "stream_acknowledgement" in result:
        # CP9: a Recover with no open incident that cleared a latched stream.
        acknowledged = result["stream_acknowledgement"]
        if (
            close is not None
            or intent.get("intent") != "recover"
            or not isinstance(acknowledged, dict)
            or set(acknowledged) != {"acknowledged_at_ns", "controllable_component_id"}
            or not _is_owner_ns(acknowledged["acknowledged_at_ns"])
            or acknowledged["controllable_component_id"] != intent.get("controllable_component_id")
            or result.get("acceptance") is not None
            or result.get("status") is not None
            or result.get("replay_status") != "accepted_new"
            or "rejection" in result
            or "incident_recovery" in result
        ):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC stream acknowledgement result is invalid",
            )
        return
    if "incident_recovery" not in result:
        if close is not None:
            if (set(result) == {"acceptance", "replay_status", "status"}
                    and result.get("replay_status") in {"stale_request_owner_epoch", "request_id_conflict",
                        "request_ledger_full", "request_result_expired"}
                    and result["acceptance"] is None and result["status"] is None):
                return  # Preserve the existing typed refusal; it is no acknowledgement.
            raise HeadlessApiError("cclc_open_response_invalid", "Close requires the exact incident recovery acknowledgement")
        return
    recovery = result["incident_recovery"]
    component_id = intent.get("controllable_component_id")
    if (
        not isinstance(recovery, dict)
        or set(recovery)
        != ({
            "acknowledged_at_ns",
            "acknowledged_by",
            "cause_class",
            "controllable_component_id",
            "incident_id",
        } | ({"closure_reason"} if close is not None else set()))
        or (close is not None and (recovery.get("closure_reason") != "cell_closed_by_operator"
            or recovery.get("incident_id") != close["expected_incident_id"]))
        or not _bounded_text(recovery["incident_id"], _MAX_INCIDENT_ID_BYTES)
        or not _member(recovery["cause_class"], _INCIDENT_CAUSE_CLASSES)
        or not _bounded_text(recovery["acknowledged_by"], 1024)
        or not _is_owner_ns(recovery["acknowledged_at_ns"])
        or recovery["controllable_component_id"] != component_id
        or result.get("acceptance") is not None
        or result.get("status") is not None
        or result.get("replay_status") not in {"accepted_new", "replayed_exact"}
        or "rejection" in result
    ):
        raise HeadlessApiError(
            "cclc_open_response_invalid",
            "CCLC incident recovery result is invalid",
        )


def _validate_incident_events(
    result: Mapping[str, Any], after_sequence: int, limit: int
) -> None:
    """Strict ordered page of incident history events (contract item 5)."""

    def invalid(reason: str) -> HeadlessApiError:
        return HeadlessApiError(
            "cclc_open_response_invalid",
            f"CCLC incident events {reason}",
        )

    events = result.get("events")
    through = result.get("through_sequence")
    if (
        set(result) != {"events", "through_sequence"}
        or not isinstance(events, list)
        or len(events) > limit
        or not _is_int(through)
    ):
        raise invalid("page is malformed")
    previous = after_sequence
    for event in events:
        if not isinstance(event, dict) or set(event) != {
            "event_kind",
            "event_sequence",
            "incident_id",
            "occurred_at_ns",
            "payload",
        }:
            raise invalid("row is malformed")
        sequence = event["event_sequence"]
        payload = event["payload"]
        if (
            not _is_int(sequence)
            or not previous < sequence <= MAXIMUM_SAFE_INTEGER
            or not _member(event["event_kind"], INCIDENT_EVENT_KINDS)
            or not _bounded_text(event["incident_id"], _MAX_INCIDENT_ID_BYTES)
            or not _is_owner_ns(event["occurred_at_ns"])
            or not isinstance(payload, dict)
            or payload.get("incident_id") != event["incident_id"]
            or payload.get("event_kind") != event["event_kind"]
        ):
            raise invalid("row is invalid or out of order")
        previous = sequence
    if through != previous:
        raise invalid("cursor does not match the page")


class CCLCOpenRequestClient:
    """Forward exact requests without caching, reducing, or replaying CCLC state."""

    def __init__(self, run_root: Path, *, timeout_seconds: float = 5.0) -> None:
        if not 0 < timeout_seconds <= 30:
            raise ValueError("CCLC timeout must be in (0, 30] seconds")
        try:
            self._run_root = PinnedRunRoot(run_root)
        except PrivateTransportCapabilityError as error:
            raise HeadlessApiError(
                "cclc_owner_unavailable",
                f"CCLC run capability is invalid: {error}",
            ) from error
        self._timeout_seconds = timeout_seconds

    def _exchange(
        self, command: str, payload: Mapping[str, Any]
    ) -> dict[str, Any] | None:
        request = {
            "api_version": CCLC_OPEN_REQUEST_VERSION,
            "command": command,
            "payload": dict(payload),
        }
        encoded = _canonical_json(
            request,
            error_code="cclc_open_request_invalid",
            label="CCLC request",
        )
        if len(encoded) > MAX_CCLC_REQUEST_BYTES:
            raise HeadlessApiError(
                "cclc_open_request_oversized",
                "CCLC request exceeds its byte bound",
            )
        try:
            with self._run_root.socket_address(CCLC_SOCKET_FILE) as socket_address:
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                    client.setsockopt(
                        socket.SOL_SOCKET,
                        socket.SO_RCVBUF,
                        MAX_CCLC_RESPONSE_BYTES,
                    )
                    client.settimeout(self._timeout_seconds)
                    client.connect(socket_address)
                    sent = client.send(encoded)
                    if sent != len(encoded):
                        raise HeadlessApiError(
                            "cclc_open_transport_failure",
                            "CCLC request packet was truncated",
                        )
                    response_bytes, _ancillary, flags, _address = client.recvmsg(
                        MAX_CCLC_RESPONSE_BYTES
                    )
        except HeadlessApiError:
            raise
        except PrivateTransportCapabilityError as exc:
            raise HeadlessApiError(
                "cclc_owner_unavailable",
                f"CCLC command socket is unavailable: {exc}",
            ) from exc
        except (OSError, TimeoutError) as exc:
            raise HeadlessApiError(
                "cclc_owner_unavailable",
                f"CCLC transport failed: {exc}",
            ) from exc
        if flags & socket.MSG_TRUNC or not response_bytes:
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC response is empty or oversized",
            )
        try:
            response_text = response_bytes.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC response is not UTF-8",
            ) from exc
        response = strict_json_object(response_text, label="CCLC response")
        if (
            _canonical_json(
                response,
                error_code="cclc_open_response_invalid",
                label="CCLC response",
            )
            != response_bytes
        ):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC response is not canonical JSON",
            )
        if (
            set(response) != _RESPONSE_FIELDS
            or response.get("api_version") != CCLC_OPEN_RESPONSE_VERSION
            or response.get("command") != command
            or type(response.get("ok")) is not bool
        ):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC response envelope is invalid",
            )
        if response["ok"] is False:
            error = response.get("error")
            if (
                response.get("result") is not None
                or not isinstance(error, dict)
                or set(error) != {"code", "details", "message"}
                or not isinstance(error.get("code"), str)
                or not error["code"]
                or not isinstance(error.get("message"), str)
                or not error["message"]
                or not isinstance(error.get("details"), dict)
                or not all(
                    isinstance(key, str) and isinstance(value, str)
                    for key, value in error["details"].items()
                )
            ):
                raise HeadlessApiError(
                    "cclc_open_response_invalid",
                    "CCLC error response is invalid",
                )
            raise CCLCOwnerRefusal(
                error["code"], error["message"], details=error["details"]
            )
        if response.get("error") is not None:
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "successful CCLC response carries an error",
            )
        result = response.get("result")
        if result is not None and not isinstance(result, dict):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC response result is not an object or null",
            )
        return result

    @staticmethod
    def _require_object(
        result: dict[str, Any] | None, *, command: str
    ) -> dict[str, Any]:
        if result is None:
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                f"{command} returned a null result",
            )
        return result

    def owner_identity(self) -> dict[str, Any]:
        result = self._require_object(
            self._exchange("owner_identity", {}), command="owner_identity"
        )
        if (
            set(result) != {"request_owner_epoch"}
            or not isinstance(result.get("request_owner_epoch"), str)
            or not result["request_owner_epoch"]
            or len(result["request_owner_epoch"].encode("utf-8")) > 256
        ):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC owner identity is invalid",
            )
        return result

    def component_snapshot(self, component_id: str) -> dict[str, Any]:
        result = self._require_object(
            self._exchange(
                "component_snapshot",
                {"controllable_component_id": component_id},
            ),
            command="component_snapshot",
        )
        _validate_lifecycle_target_facts(result)
        _validate_component_readiness(result)
        return result

    def component_lifecycle_snapshot(self, component_id: str) -> dict[str, Any]:
        result = self._require_object(
            self._exchange("component_lifecycle_snapshot", {"controllable_component_id": component_id}),
            command="component_lifecycle_snapshot",
        )
        _validate_component_readiness(result, lifecycle_only=True)
        return result

    def lifecycle_target_retirement_offer(self) -> dict[str, Any]:
        return retirement_offer(self._require_object(
            self._exchange("lifecycle_target_retirement_offer", {}),
            command="lifecycle_target_retirement_offer"))

    def retire_lifecycle_targets(self, intent: LifecycleTargetRetirementIntent) -> dict[str, Any]:
        return retirement_result(self._require_object(
            self._exchange("lifecycle_target_retire", intent.model_dump(mode="json", exclude_none=False)),
            command="lifecycle_target_retire"), intent)

    def lifecycle_target_retirement_result(self, intent: LifecycleTargetRetirementIntent) -> dict[str, Any] | None:
        result = self._exchange("lifecycle_target_retirement_result", intent.model_dump(mode="json", exclude_none=False))
        return None if result is None else retirement_result(result, intent)

    def lifecycle_owner_settlement(self, scope: LifecycleTargetRetirementScope) -> dict[str, Any]:
        return settlement_result(self._require_object(
            self._exchange("lifecycle_owner_settlement", {"scope": scope.model_dump(mode="json")}),
            command="lifecycle_owner_settlement"), scope)

    def route_templates(self, component_id: str) -> dict[str, Any]:
        result = self._require_object(
            self._exchange(
                "route_templates",
                {"controllable_component_id": component_id},
            ),
            command="route_templates",
        )
        if (
            set(result)
            != {
                "controllable_component_id",
                "graph_revision",
                "retained_bundle_id",
                "templates",
            }
            or not isinstance(result.get("graph_revision"), str)
            or not result["graph_revision"]
            or not isinstance(result.get("retained_bundle_id"), str)
            or not result["retained_bundle_id"]
            or not isinstance(result.get("templates"), list)
        ):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC route templates response is malformed",
            )
        return result

    def submit_lifecycle(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        _validate_close_incident_intent(intent)
        result = self._require_object(
            self._exchange("lifecycle_submit", intent),
            command="lifecycle_submit",
        )
        acceptance = result.get("acceptance")
        if acceptance is not None and not isinstance(acceptance, dict):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC lifecycle acceptance is malformed",
            )
        _validate_incident_recovery(result, intent)
        return result

    def lifecycle_request_result(
        self, intent: Mapping[str, Any]
    ) -> dict[str, Any] | None:
        _validate_close_incident_intent(intent)
        result = self._request_result("lifecycle_request_result", intent)
        if result is not None:
            _validate_incident_recovery(result, intent)
        return result

    def incident_events(self, after_sequence: int, limit: int) -> dict[str, Any]:
        """Read incident history events after `after_sequence`, in order."""

        if (
            not _is_int(after_sequence)
            or not 0 <= after_sequence <= MAXIMUM_SAFE_INTEGER
            or not _is_int(limit)
            or not 1 <= limit <= MAX_INCIDENT_EVENT_PAGE
        ):
            raise ValueError("incident event cursor or limit is outside its bound")
        result = self._require_object(
            self._exchange(
                "incident_events",
                {"after_sequence": after_sequence, "limit": limit},
            ),
            command="incident_events",
        )
        _validate_incident_events(result, after_sequence, limit)
        return result

    def route_request_result(self, intent: Mapping[str, Any]) -> dict[str, Any] | None:
        return self._request_result("route_request_result", intent)

    def _request_result(
        self, command: str, intent: Mapping[str, Any]
    ) -> dict[str, Any] | None:
        result = self._exchange(command, intent)
        if result is not None:
            acceptance = result.get("acceptance")
            if acceptance is not None and not isinstance(acceptance, dict):
                raise HeadlessApiError(
                    "cclc_open_response_invalid",
                    "CCLC result acceptance is malformed",
                )
        return result

    def lifecycle_status(self, transaction_id: str) -> dict[str, Any] | None:
        result = self._exchange(
            "lifecycle_status",
            {"lifecycle_transaction_id": transaction_id},
        )
        if result is not None:
            if not isinstance(result.get("acceptance"), dict):
                raise HeadlessApiError(
                    "cclc_open_response_invalid",
                    "CCLC lifecycle status is malformed",
                )
        return result

    def submit_route(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        result = self._require_object(
            self._exchange("route_submit", intent), command="route_submit"
        )
        acceptance = result.get("acceptance")
        if acceptance is not None and not isinstance(acceptance, dict):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC route acceptance is malformed",
            )
        return result

    def route_status(self, transaction_id: str) -> dict[str, Any] | None:
        result = self._exchange(
            "route_status",
            {"route_selection_transaction_id": transaction_id},
        )
        if result is not None:
            if not isinstance(result.get("acceptance"), dict):
                raise HeadlessApiError(
                    "cclc_open_response_invalid",
                    "CCLC route status is malformed",
                )
        return result
