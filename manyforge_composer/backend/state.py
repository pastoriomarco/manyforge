"""ComposerState - Holds the runtime state shared across all API routes."""
from __future__ import annotations

import hashlib
import logging
import threading
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional
from urllib.parse import urlparse

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import (
    Program,
    apply_tree_param_overrides,
    compute_program_tree_hash,
    find_tree_node_by_name,
)
from manyforge_behavior.deployment import Deployment
from manyforge_behavior.cycle_manager import CycleManager
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.skill_loader import SkillCatalog, load_skill_catalog, default_skills_dir
from .event_recorder import EventRecorder
from .security import normalize_allowed_roots

logger = logging.getLogger(__name__)


def _normalize_scene_snapshot_base_url(value: str, *, field_name: str) -> str:
    candidate = value.strip().rstrip("/")
    if not candidate:
        raise RuntimeError(f"{field_name} is empty")

    parsed = urlparse(candidate)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise RuntimeError(
            f"{field_name} must be an absolute http(s) URL with a host"
        )
    if parsed.username or parsed.password:
        raise RuntimeError(
            f"{field_name} must not include embedded credentials"
        )
    return candidate


@dataclass
class RuntimeOverrideRecord:
    node_name: str
    params: dict[str, Any]
    authority_source: str
    approval_mode: str
    reason: str
    session_id: Optional[str]
    trigger_node_instance: Optional[str]
    base_program_hash: str = ""
    requested_at_ns: int = field(default_factory=time.monotonic_ns)
    activation_state: str = "active"  # active | staged
    activated_at_ns: Optional[int] = None


@dataclass
class PublishedSceneSnapshot:
    snapshot_id: str
    image_bytes: bytes
    media_type: str
    width: int
    height: int
    format: str
    sha256: str
    created_at_ns: int = field(default_factory=time.monotonic_ns)


@dataclass
class AssistantProposalRecord:
    proposal_id: str
    conversation_id: str
    proposal: dict[str, Any]
    source: dict[str, Any] = field(default_factory=dict)
    lifecycle_status: str = "staged"
    created_at_ns: int = field(default_factory=time.monotonic_ns)
    updated_at_ns: int = field(default_factory=time.monotonic_ns)


class ComposerState:
    """Holds the runtime state shared across all API routes."""

    def __init__(
        self,
        catalog_path: str,
        kernel: KernelClient,
        kernel_factory: Optional[Callable[[Optional[Deployment]], tuple[KernelClient, Any]]] = None,
        kernel_bundle: Any = None,
        trace_dir: str | None = None,
        allowed_fs_roots: list[str] | None = None,
    ):
        self.catalog = NodeCatalog(catalog_path)
        self._catalog_path = str(Path(catalog_path).resolve())
        self._kernel = kernel
        self._kernel_factory = kernel_factory
        self.kernel_bundle = kernel_bundle
        self.program: Optional[Program] = None
        self._effective_program: Optional[Program] = None
        self.program_source_path: str = ""
        self.deployment: Optional[Deployment] = None
        self.deployment_source_path: str = ""
        self.cycle_manager: Optional[CycleManager] = None
        self.hmi_server = None
        self.hmi_public_port: int | None = None
        self.public_api_base_url: str | None = None
        self._on_program_loaded: Optional[Callable[["ComposerState"], None]] = None
        self.event_recorder = EventRecorder(trace_dir=trace_dir)
        self.scene_edit_lock = threading.Lock()
        self.allowed_fs_roots = normalize_allowed_roots(allowed_fs_roots)
        self._runtime_overrides: dict[str, RuntimeOverrideRecord] = {}
        self._program_tree_hash: str = ""
        self._program_session_generation: int = 0
        self._scene_snapshot_lock = threading.Lock()
        self._published_scene_snapshots: dict[str, PublishedSceneSnapshot] = {}
        self.assistant_provider: Any = None
        self._assistant_proposal_lock = threading.Lock()
        self._assistant_proposals: dict[str, AssistantProposalRecord] = {}
        self.skill_catalog: SkillCatalog = load_skill_catalog(
            default_skills_dir(),
            catalog=self.catalog,
            strict=True,
        )

    def stage_assistant_proposals(
        self,
        *,
        conversation_id: str,
        proposals: list[dict[str, Any]],
        source: dict[str, Any] | None = None,
    ) -> list[AssistantProposalRecord]:
        """Store reviewable assistant proposals without changing program state."""
        now_ns = time.monotonic_ns()
        staged: list[AssistantProposalRecord] = []
        source_payload = dict(source or {})
        with self._assistant_proposal_lock:
            for proposal in proposals:
                proposal_id = str(proposal["id"])
                record = AssistantProposalRecord(
                    proposal_id=proposal_id,
                    conversation_id=conversation_id,
                    proposal=dict(proposal),
                    source=source_payload,
                    lifecycle_status="staged",
                    created_at_ns=now_ns,
                    updated_at_ns=now_ns,
                )
                self._assistant_proposals[proposal_id] = record
                staged.append(record)
        return staged

    def list_assistant_proposals(
        self,
        *,
        lifecycle_status: str | None = None,
        conversation_id: str | None = None,
    ) -> list[AssistantProposalRecord]:
        with self._assistant_proposal_lock:
            records = list(self._assistant_proposals.values())
        if lifecycle_status:
            records = [
                record for record in records
                if record.lifecycle_status == lifecycle_status
            ]
        if conversation_id:
            records = [
                record for record in records
                if record.conversation_id == conversation_id
            ]
        return sorted(records, key=lambda item: (item.created_at_ns, item.proposal_id))

    def get_assistant_proposal(self, proposal_id: str) -> AssistantProposalRecord | None:
        with self._assistant_proposal_lock:
            return self._assistant_proposals.get(proposal_id)

    def discard_assistant_proposal(
        self,
        proposal_id: str,
        *,
        reason: str = "",
    ) -> AssistantProposalRecord | None:
        with self._assistant_proposal_lock:
            record = self._assistant_proposals.get(proposal_id)
            if record is None:
                return None
            record.lifecycle_status = "discarded"
            record.updated_at_ns = time.monotonic_ns()
            if reason:
                record.source = {**record.source, "discardReason": reason}
            return record

    def _clear_published_scene_snapshots(self) -> None:
        with self._scene_snapshot_lock:
            self._published_scene_snapshots.clear()

    def _prune_published_scene_snapshots(self, *, now_ns: Optional[int] = None) -> None:
        now = now_ns or time.monotonic_ns()
        ttl_ns = 10 * 60 * 1_000_000_000
        max_entries = 64
        expired_ids = [
            snapshot_id
            for snapshot_id, snapshot in self._published_scene_snapshots.items()
            if now - snapshot.created_at_ns > ttl_ns
        ]
        for snapshot_id in expired_ids:
            self._published_scene_snapshots.pop(snapshot_id, None)
        if len(self._published_scene_snapshots) <= max_entries:
            return
        oldest_first = sorted(
            self._published_scene_snapshots.values(),
            key=lambda item: item.created_at_ns,
        )
        while len(oldest_first) > max_entries:
            stale = oldest_first.pop(0)
            self._published_scene_snapshots.pop(stale.snapshot_id, None)

    def publish_scene_snapshot(
        self,
        image_bytes: bytes,
        *,
        width: int,
        height: int,
        format: str,
        fetch_base_url: Optional[str] = None,
        model_base_url: Optional[str] = None,
    ) -> dict[str, Any]:
        resolved_fetch_base_url = self.resolve_scene_snapshot_fetch_base_url(
            fetch_base_url=fetch_base_url,
            model_base_url=model_base_url,
        )
        media_type = "image/png" if format.lower() == "png" else "image/jpeg"
        snapshot_id = uuid.uuid4().hex
        snapshot = PublishedSceneSnapshot(
            snapshot_id=snapshot_id,
            image_bytes=bytes(image_bytes),
            media_type=media_type,
            width=width,
            height=height,
            format=format,
            sha256=hashlib.sha256(image_bytes).hexdigest(),
        )
        with self._scene_snapshot_lock:
            self._prune_published_scene_snapshots()
            self._published_scene_snapshots[snapshot_id] = snapshot
            self._prune_published_scene_snapshots()
        return {
            "snapshot_id": snapshot_id,
            "url": f"{resolved_fetch_base_url.rstrip('/')}/api/scene/snapshots/{snapshot_id}",
            "sha256": snapshot.sha256,
            "format": format,
            "width": width,
            "height": height,
            "byte_size": len(image_bytes),
        }

    def resolve_scene_snapshot_fetch_base_url(
        self,
        *,
        fetch_base_url: Optional[str] = None,
        model_base_url: Optional[str] = None,
    ) -> str:
        explicit = (fetch_base_url or "").strip()
        if explicit:
            return _normalize_scene_snapshot_base_url(
                explicit,
                field_name="Scene snapshot fetch base URL",
            )

        public_base = (self.public_api_base_url or "").strip()
        if model_base_url and public_base:
            model_parts = urlparse(model_base_url)
            public_parts = urlparse(public_base)
            if (
                model_parts.scheme
                and model_parts.hostname
                and public_parts.scheme
                and public_parts.hostname
            ):
                auth_host = model_parts.hostname
                if model_parts.port is not None:
                    auth_host = f"{auth_host}:{model_parts.port}"
                public_host = public_parts.hostname
                if public_parts.port is not None:
                    public_host = f"{public_host}:{public_parts.port}"
                derived = public_parts._replace(
                    netloc=auth_host,
                    path="",
                    params="",
                    query="",
                    fragment="",
                )
                return _normalize_scene_snapshot_base_url(
                    derived.geturl(),
                    field_name="Derived scene snapshot fetch base URL",
                )

        if public_base:
            return _normalize_scene_snapshot_base_url(
                public_base,
                field_name="Composer public API base URL",
            )

        raise RuntimeError("No scene snapshot fetch base URL is configured")

    def get_published_scene_snapshot(
        self,
        snapshot_id: str,
    ) -> Optional[PublishedSceneSnapshot]:
        with self._scene_snapshot_lock:
            self._prune_published_scene_snapshots()
            return self._published_scene_snapshots.get(snapshot_id)

    def _bind_cycle_manager_callbacks(self) -> None:
        if self.cycle_manager is None:
            return
        self.cycle_manager.set_parent_run_id_provider(
            self._current_or_last_run_id_for_recovery,
        )
        self.cycle_manager.set_on_delegated_recovery_requested(
            self._record_delegated_recovery_requested,
        )
        self.cycle_manager.set_on_interrupt_event(self._record_interrupt_event)

    def _current_or_last_run_id_for_recovery(self) -> Optional[str]:
        self.event_recorder.observe(self)
        return self.event_recorder.current_run_id or self.event_recorder.last_run_id

    def _record_delegated_recovery_requested(self, request) -> None:
        self.event_recorder.record_delegated_recovery_requested(request)

    def _record_interrupt_event(self, **payload: Any) -> None:
        self.event_recorder.observe(self)
        self.event_recorder.record_interrupt_event(
            run_id=self.event_recorder.current_run_id or self.event_recorder.last_run_id,
            execution_mode="program",
            **payload,
        )

    @property
    def kernel(self) -> Optional[KernelClient]:
        """Access the kernel client."""
        return self._kernel

    @property
    def effective_program(self) -> Optional[Program]:
        """Program snapshot currently bound into the runtime runner."""
        return self._effective_program

    @property
    def program_tree_hash(self) -> str:
        """Current base program tree hash."""
        return self._program_tree_hash

    @property
    def program_session_generation(self) -> int:
        """Monotonic generation for the loaded/applied program snapshot."""
        return self._program_session_generation

    @property
    def runtime_overrides(self) -> dict[str, RuntimeOverrideRecord]:
        """Return a shallow copy of the runtime override ledger keyed by node name."""
        return dict(self._runtime_overrides)

    def _update_program_tree_hash(self) -> None:
        """Recompute and cache the program tree hash from the current base program."""
        if self.program is not None and self.program.tree is not None:
            self._program_tree_hash = compute_program_tree_hash(self.program.tree)
        else:
            self._program_tree_hash = ""

    def _override_is_stale(self, record: RuntimeOverrideRecord) -> bool:
        """An override is stale when its base_program_hash doesn't match the current program."""
        return (
            bool(record.base_program_hash)
            and record.base_program_hash != self._program_tree_hash
        )

    def _has_stale_overrides(self) -> bool:
        return any(self._override_is_stale(r) for r in self._runtime_overrides.values())

    def _stale_override_names(self) -> list[str]:
        return [
            name for name, r in self._runtime_overrides.items()
            if self._override_is_stale(r)
        ]

    def _deployment_capability_keys(self) -> set[str] | None:
        if self.deployment is None:
            return None
        capability_keys: set[str] = set()
        for adapter in self.deployment.adapters.values():
            capability_keys.update(adapter.capability_keys)
        return capability_keys

    def _build_program_with_overrides(
        self,
        *,
        include_staged: bool,
    ) -> Program:
        if self.program is None or self.program.tree is None:
            raise RuntimeError("No program loaded")

        stale_names = [
            r.node_name for r in self._runtime_overrides.values()
            if self._override_is_stale(r)
            and (include_staged or r.activation_state == "active")
        ]
        if stale_names:
            raise RuntimeError(
                f"Cannot build effective program: stale overrides must be "
                f"revalidated or discarded first: {', '.join(stale_names)}"
            )

        overrides_by_name = {
            record.node_name: dict(record.params)
            for record in self._runtime_overrides.values()
            if include_staged or record.activation_state == "active"
        }
        effective_tree = apply_tree_param_overrides(self.program.tree, overrides_by_name)
        program_data = self.program.to_dict()
        program_data["tree"] = effective_tree.to_dict()
        return Program.from_dict(
            program_data,
            self.catalog,
            path=self.program_source_path or "<memory>",
            skill_catalog=self.skill_catalog,
            deployment_capability_keys=self._deployment_capability_keys(),
        )

    def _set_runtime_override_states(self, activation_state: str) -> None:
        now_ns = time.monotonic_ns()
        for record in self._runtime_overrides.values():
            record.activation_state = activation_state
            if activation_state == "active":
                record.activated_at_ns = now_ns

    def _selected_runtime_override_records(
        self,
        *,
        include_staged: bool,
    ) -> list[RuntimeOverrideRecord]:
        records = [
            record
            for record in self._runtime_overrides.values()
            if include_staged or record.activation_state == "active"
        ]
        if not records:
            selected = "active/staged" if include_staged else "active"
            raise RuntimeError(f"No {selected} runtime overrides selected")
        return records

    def _validate_persistent_edit_authority(
        self,
        records: list[RuntimeOverrideRecord],
        *,
        authority_source: str,
        approval_mode: str,
    ) -> None:
        if authority_source not in {"human", "llm"}:
            raise RuntimeError("authority_source must be one of: human, llm")
        if approval_mode not in {"human_approved", "not_required"}:
            raise RuntimeError("approval_mode must be one of: human_approved, not_required")
        if authority_source == "human" and approval_mode != "human_approved":
            raise RuntimeError("human authority_source requires approval_mode=human_approved")
        if authority_source == "llm" and approval_mode == "human_approved":
            raise RuntimeError("approval_mode=human_approved is only valid for human authority_source")

        if self.program is None or self.program.tree is None:
            raise RuntimeError("No program loaded")

        for record in records:
            target_node = find_tree_node_by_name(self.program.tree, record.node_name)
            if target_node is None:
                raise RuntimeError(f"Unknown node instance name: {record.node_name}")
            policy = target_node.modification_policy.get("persistent_edit", "forbidden")
            if policy == "forbidden":
                raise RuntimeError(
                    f"Node '{record.node_name}' does not allow persistent edits"
                )
            if policy == "human_approval" and authority_source != "human":
                raise RuntimeError(
                    f"Node '{record.node_name}' requires human approval for persistent edits"
                )

    def _runtime_override_run_context(
        self,
        session_id: Optional[str],
    ) -> tuple[Optional[str], Optional[str], Optional[str]]:
        run_id = self.event_recorder.current_run_id or self.event_recorder.last_run_id
        authority_source = None
        trigger_node_instance = None
        intervention = (
            self.cycle_manager.intervention
            if self.cycle_manager is not None
            else None
        )
        if intervention is not None and intervention.session_id == session_id:
            authority_source = intervention.authority_source
            trigger_node_instance = intervention.trigger_node_instance
            run_id = intervention.parent_run_id or f"intervention-{intervention.session_id}"
        return run_id, authority_source, trigger_node_instance

    def _clear_runtime_overrides(self) -> None:
        self._runtime_overrides.clear()

    def _override_entry_dict(self, record: RuntimeOverrideRecord) -> dict[str, Any]:
        """Serialize a single override record for REST/MCP consumption."""
        return {
            "node_name": record.node_name,
            "params": dict(record.params),
            "authority_source": record.authority_source,
            "approval_mode": record.approval_mode,
            "reason": record.reason,
            "session_id": record.session_id,
            "trigger_node_instance": record.trigger_node_instance,
            "base_program_hash": record.base_program_hash,
            "requested_at_ns": record.requested_at_ns,
            "activation_state": record.activation_state,
            "activated_at_ns": record.activated_at_ns,
            "is_stale": self._override_is_stale(record),
        }

    def runtime_override_status(self) -> dict[str, Any]:
        """Return ledger state optimized for REST/MCP/UI consumption."""
        cycle_state = self.cycle_manager.state.value if self.cycle_manager is not None else "idle"
        active = [
            record for record in self._runtime_overrides.values()
            if record.activation_state == "active"
        ]
        staged = [
            record for record in self._runtime_overrides.values()
            if record.activation_state == "staged"
        ]
        stale = [
            record for record in self._runtime_overrides.values()
            if self._override_is_stale(record)
        ]
        requires_reset_to_activate = bool(staged) and cycle_state == "stopped"
        can_activate_now = (
            bool(staged) and cycle_state == "idle" and not stale
        )
        next_effective_tree = None
        if self.program is not None and staged and not stale:
            try:
                next_effective_tree = self._build_program_with_overrides(
                    include_staged=True
                ).tree.to_dict()
            except Exception:
                next_effective_tree = None
        return {
            "active": [self._override_entry_dict(r) for r in active],
            "staged": [self._override_entry_dict(r) for r in staged],
            "stale": [self._override_entry_dict(r) for r in stale],
            "active_count": len(active),
            "staged_count": len(staged),
            "stale_count": len(stale),
            "has_active": bool(active),
            "has_staged": bool(staged),
            "has_stale": bool(stale),
            "can_activate_now": can_activate_now,
            "requires_reset_to_activate": requires_reset_to_activate,
            "current_tree_origin": "runtime_override" if active else "saved_program",
            "base_program_hash": self._program_tree_hash,
            "next_effective_tree": next_effective_tree,
            "timestamp_ns": time.monotonic_ns(),
        }

    def materialized_runtime_override_program(
        self,
        *,
        include_staged: bool,
    ) -> Program:
        """Return the program snapshot produced by materializing runtime overrides."""
        if self.program is None or self.program.tree is None:
            raise RuntimeError("No program loaded")
        if not self._runtime_overrides:
            return self.program
        return self._build_program_with_overrides(include_staged=include_staged)

    def _current_cycle_state(self) -> str:
        return self.cycle_manager.state.value if self.cycle_manager is not None else "idle"

    def promote_runtime_overrides_to_loaded_program(
        self,
        *,
        include_staged: bool,
    ) -> Program:
        """Materialize selected runtime overrides into the loaded in-memory program."""
        cycle_state = self._current_cycle_state()
        if cycle_state not in {"idle", "stopped"}:
            raise RuntimeError(
                "Runtime overrides may only be promoted into the loaded program in IDLE or STOPPED"
            )
        if not include_staged and any(
            record.activation_state == "staged"
            for record in self._runtime_overrides.values()
        ):
            raise RuntimeError(
                "Staged runtime overrides exist; either include staged overrides in the promotion or clear them first"
            )
        selected_records = self._selected_runtime_override_records(
            include_staged=include_staged,
        )
        materialized_program = self.materialized_runtime_override_program(
            include_staged=include_staged,
        )
        self.apply_models(
            materialized_program,
            self.deployment,
            force_discard_overrides=True,
        )
        for record in selected_records:
            self.event_recorder.record_runtime_override_event(
                action="promoted_loaded_program",
                node_name=record.node_name,
                params=record.params,
                activation_state=record.activation_state,
                base_program_hash=record.base_program_hash,
                run_id=self.event_recorder.last_run_id,
                session_id=record.session_id,
                authority_source=record.authority_source,
                trigger_node_instance=record.trigger_node_instance,
            )
        return materialized_program

    def save_materialized_runtime_override_program(
        self,
        target_path: str,
        *,
        include_staged: bool,
        authority_source: str,
        approval_mode: str,
    ) -> Program:
        """Save a materialized program view produced from runtime overrides."""
        cycle_state = self._current_cycle_state()
        if cycle_state in {"running", "interrupt_handler"}:
            raise RuntimeError(
                "Materialized runtime override saves are not allowed while execution is active"
            )
        selected_records = self._selected_runtime_override_records(
            include_staged=include_staged,
        )
        self._validate_persistent_edit_authority(
            selected_records,
            authority_source=authority_source,
            approval_mode=approval_mode,
        )
        materialized_program = self.materialized_runtime_override_program(
            include_staged=include_staged,
        )
        materialized_program.save(target_path)
        for record in selected_records:
            self.event_recorder.record_runtime_override_event(
                action="saved_materialized_program",
                node_name=record.node_name,
                params=record.params,
                activation_state=record.activation_state,
                base_program_hash=record.base_program_hash,
                run_id=self.event_recorder.last_run_id,
                session_id=record.session_id,
                authority_source=authority_source,
                trigger_node_instance=record.trigger_node_instance,
                details={"path": target_path},
            )
        return materialized_program

    def activate_staged_runtime_overrides(self) -> None:
        """Promote staged overrides into the runtime by rebuilding the effective program."""
        if not any(
            record.activation_state == "staged"
            for record in self._runtime_overrides.values()
        ):
            return
        cycle_state = self.cycle_manager.state.value if self.cycle_manager is not None else "idle"
        if cycle_state != "idle":
            raise RuntimeError("Staged runtime overrides can only be activated from IDLE")
        effective_program = self._build_program_with_overrides(include_staged=True)
        previous_states = {
            node_name: (record.activation_state, record.activated_at_ns)
            for node_name, record in self._runtime_overrides.items()
        }
        self._set_runtime_override_states("active")
        try:
            self._rebuild_runtime(
                self.program,
                self.deployment,
                effective_program=effective_program,
            )
        except Exception:
            for node_name, (activation_state, activated_at_ns) in previous_states.items():
                record = self._runtime_overrides.get(node_name)
                if record is None:
                    continue
                record.activation_state = activation_state
                record.activated_at_ns = activated_at_ns
            raise
        for record in self._runtime_overrides.values():
            run_id, authority_source, trigger_node_instance = self._runtime_override_run_context(
                record.session_id
            )
            self.event_recorder.record_runtime_override_event(
                action="activated",
                node_name=record.node_name,
                params=record.params,
                activation_state=record.activation_state,
                base_program_hash=record.base_program_hash,
                run_id=run_id,
                session_id=record.session_id,
                authority_source=authority_source or record.authority_source,
                trigger_node_instance=trigger_node_instance or record.trigger_node_instance,
            )

    def remove_runtime_override(self, node_name: str) -> bool:
        """Remove a runtime override and rebuild immediately if it was active."""
        cycle_state = self._ensure_runtime_override_mutation_allowed()
        record = self._runtime_overrides.pop(node_name, None)
        if record is None:
            return False
        if cycle_state == "direct_control" and record.activation_state == "active":
            self._runtime_overrides[node_name] = record
            raise RuntimeError(
                "Cannot remove an active runtime override during DIRECT_CONTROL; return to IDLE first"
            )
        if record.activation_state == "active":
            effective_program = (
                self._build_program_with_overrides(include_staged=False)
                if self._runtime_overrides
                else self.program
            )
            try:
                self._rebuild_runtime(
                    self.program,
                    self.deployment,
                    effective_program=effective_program,
                )
            except Exception:
                self._runtime_overrides[node_name] = record
                raise
        run_id, authority_source, trigger_node_instance = self._runtime_override_run_context(
            record.session_id
        )
        self.event_recorder.record_runtime_override_event(
            action="removed",
            node_name=record.node_name,
            params=record.params,
            activation_state=record.activation_state,
            base_program_hash=record.base_program_hash,
            run_id=run_id,
            session_id=record.session_id,
            authority_source=authority_source or record.authority_source,
            trigger_node_instance=trigger_node_instance or record.trigger_node_instance,
        )
        return True

    def clear_runtime_overrides(self) -> None:
        """Drop all runtime overrides and return the runtime to the base program."""
        cycle_state = self._ensure_runtime_override_mutation_allowed()
        removed = list(self._runtime_overrides.values())
        if cycle_state == "direct_control" and any(
            record.activation_state == "active" for record in removed
        ):
            raise RuntimeError(
                "Cannot clear active runtime overrides during DIRECT_CONTROL; return to IDLE first"
            )
        had_active = any(
            record.activation_state == "active"
            for record in self._runtime_overrides.values()
        )
        self._clear_runtime_overrides()
        if had_active and self.program is not None:
            try:
                self._rebuild_runtime(
                    self.program,
                    self.deployment,
                    effective_program=self.program,
                )
            except Exception:
                self._runtime_overrides = {
                    record.node_name: record
                    for record in removed
                }
                raise
        for record in removed:
            run_id, authority_source, trigger_node_instance = self._runtime_override_run_context(
                record.session_id
            )
            self.event_recorder.record_runtime_override_event(
                action="cleared",
                node_name=record.node_name,
                params=record.params,
                activation_state=record.activation_state,
                base_program_hash=record.base_program_hash,
                run_id=run_id,
                session_id=record.session_id,
                authority_source=authority_source or record.authority_source,
                trigger_node_instance=trigger_node_instance or record.trigger_node_instance,
            )

    def revalidate_stale_overrides(self) -> dict[str, str]:
        """Re-validate stale overrides against the current program.

        Returns a mapping of node_name -> outcome where outcome is one of:
        ``"revalidated"`` (hash updated) or ``"invalid"`` (removed from ledger).
        """
        if self.program is None or self.program.tree is None:
            raise RuntimeError("No program loaded")

        stale = [
            (name, record)
            for name, record in self._runtime_overrides.items()
            if self._override_is_stale(record)
        ]
        if not stale:
            return {}

        # Determine outcome for each stale override and snapshot record data
        # for event emission before any mutations.
        outcomes: list[tuple[str, str, RuntimeOverrideRecord]] = []
        for name, record in stale:
            target_node = find_tree_node_by_name(self.program.tree, name)
            if target_node is None:
                outcomes.append((name, "invalid", record))
                continue

            policy = target_node.modification_policy.get("runtime_override", "forbidden")
            if policy == "forbidden":
                outcomes.append((name, "invalid", record))
                continue

            try:
                test_overrides = {name: dict(record.params)}
                apply_tree_param_overrides(self.program.tree, test_overrides)
            except Exception:
                outcomes.append((name, "invalid", record))
                continue

            outcomes.append((name, "revalidated", record))

        # Apply mutations and emit events.
        results: dict[str, str] = {}
        for name, outcome, record in outcomes:
            old_hash = record.base_program_hash
            if outcome == "revalidated":
                record.base_program_hash = self._program_tree_hash
            else:
                self._runtime_overrides.pop(name, None)
            results[name] = outcome

            run_id, authority_source, trigger_node_instance = (
                self._runtime_override_run_context(record.session_id)
            )
            action = "revalidated" if outcome == "revalidated" else "invalidated"
            self.event_recorder.record_runtime_override_event(
                action=action,
                node_name=name,
                params=record.params,
                activation_state=record.activation_state,
                base_program_hash=old_hash,
                run_id=run_id,
                session_id=record.session_id,
                authority_source=authority_source or record.authority_source,
                trigger_node_instance=trigger_node_instance or record.trigger_node_instance,
            )

        return results

    def apply_runtime_override(
        self,
        *,
        node_name: str,
        params: dict[str, Any],
        authority_source: str,
        approval_mode: str,
        reason: str = "",
        session_id: Optional[str] = None,
        trigger_node_instance: Optional[str] = None,
    ) -> RuntimeOverrideRecord:
        """Validate, record, and apply or stage a runtime override."""
        if self.program is None or self.program.tree is None:
            raise RuntimeError("No program loaded")
        if not isinstance(params, dict) or not params:
            raise RuntimeError("Runtime override params must be a non-empty mapping")

        cycle_state = self.cycle_manager.state.value if self.cycle_manager is not None else "idle"
        if cycle_state not in {"idle", "direct_control"}:
            raise RuntimeError("Runtime overrides may only be changed in IDLE or DIRECT_CONTROL")
        if cycle_state == "direct_control" and any(
            record.activation_state == "active"
            for record in self._runtime_overrides.values()
        ):
            raise RuntimeError(
                "Cannot stage additional runtime overrides during DIRECT_CONTROL while active overrides are already bound into the runner; return to IDLE first"
            )
        if authority_source not in {"human", "llm"}:
            raise RuntimeError("authority_source must be one of: human, llm")
        if approval_mode not in {"human_approved", "not_required"}:
            raise RuntimeError("approval_mode must be one of: human_approved, not_required")
        if authority_source == "human" and approval_mode != "human_approved":
            raise RuntimeError("human authority_source requires approval_mode=human_approved")
        if authority_source == "llm" and approval_mode == "human_approved":
            raise RuntimeError("approval_mode=human_approved is only valid for human authority_source")

        target_node = find_tree_node_by_name(self.program.tree, node_name)
        if target_node is None:
            raise RuntimeError(f"Unknown node instance name: {node_name}")

        policy = target_node.modification_policy.get("runtime_override", "forbidden")
        if policy == "forbidden":
            raise RuntimeError(f"Node '{node_name}' does not allow runtime overrides")
        if policy == "human_approval" and authority_source != "human":
            raise RuntimeError(f"Node '{node_name}' requires human approval for runtime override")

        existing = self._runtime_overrides.get(node_name)
        merged_params = dict(existing.params) if existing is not None else {}
        merged_params.update(params)

        candidate = RuntimeOverrideRecord(
            node_name=node_name,
            params=merged_params,
            authority_source=authority_source,
            approval_mode=approval_mode,
            reason=reason,
            session_id=session_id,
            trigger_node_instance=trigger_node_instance,
            base_program_hash=self._program_tree_hash,
            activation_state="active" if cycle_state == "idle" else "staged",
            activated_at_ns=time.monotonic_ns() if cycle_state == "idle" else None,
        )
        self._runtime_overrides[node_name] = candidate
        try:
            effective_program = self._build_program_with_overrides(include_staged=True)
        except Exception:
            if existing is None:
                self._runtime_overrides.pop(node_name, None)
            else:
                self._runtime_overrides[node_name] = existing
            raise

        if cycle_state == "idle":
            try:
                self._rebuild_runtime(
                    self.program,
                    self.deployment,
                    effective_program=effective_program,
                )
            except Exception:
                if existing is None:
                    self._runtime_overrides.pop(node_name, None)
                else:
                    self._runtime_overrides[node_name] = existing
                raise
        run_id, event_authority_source, event_trigger_node_instance = self._runtime_override_run_context(
            candidate.session_id
        )
        self.event_recorder.record_runtime_override_event(
            action="applied",
            node_name=candidate.node_name,
            params=candidate.params,
            activation_state=candidate.activation_state,
            base_program_hash=candidate.base_program_hash,
            run_id=run_id,
            session_id=candidate.session_id,
            authority_source=event_authority_source or candidate.authority_source,
            trigger_node_instance=event_trigger_node_instance or candidate.trigger_node_instance,
            details={"reason": candidate.reason},
        )
        return candidate

    def _ensure_runtime_override_mutation_allowed(self) -> str:
        cycle_state = self.cycle_manager.state.value if self.cycle_manager is not None else "idle"
        if cycle_state not in {"idle", "direct_control"}:
            raise RuntimeError("Runtime overrides may only be changed in IDLE or DIRECT_CONTROL")
        return cycle_state

    def _rebuild_runtime(
        self,
        program: Program,
        deployment: Optional[Deployment],
        *,
        effective_program: Optional[Program] = None,
    ) -> None:
        """Rebuild runner + cycle manager from already-validated in-memory models."""
        scene_snapshot_provider = (
            getattr(self.hmi_server, "get_cached_scene_snapshot", None)
            or getattr(self.hmi_server, "render_scene", None)
        )
        if self.cycle_manager is not None:
            self.cycle_manager.reset()

        if deployment is not None and self._kernel_factory is not None:
            self._kernel, self.kernel_bundle = self._kernel_factory(deployment)

        runner = OrchestratorRunner(
            catalog_path=self._catalog_path,
            program_path=self.program_source_path or "<memory>",
            kernel=self._kernel,
            deployment_path=self.deployment_source_path or None,
            program=effective_program or program,
            deployment=deployment,
            skill_catalog=self.skill_catalog,
            event_recorder=self.event_recorder,
            scene_snapshot_provider=scene_snapshot_provider,
            scene_snapshot_url_publisher=self.publish_scene_snapshot,
        )
        runner.load()

        self.program = program
        self._effective_program = effective_program or program
        self.deployment = deployment
        self._update_program_tree_hash()
        self._program_session_generation += 1
        self._clear_published_scene_snapshots()
        self.cycle_manager = CycleManager(runner, self._kernel)
        self._bind_cycle_manager_callbacks()

        if self._on_program_loaded:
            try:
                self._on_program_loaded(self)
            except Exception:
                logger.exception("on_program_loaded callback failed")

    def apply_models(
        self,
        program: Program,
        deployment: Optional[Deployment],
        *,
        force_discard_overrides: bool = False,
    ) -> None:
        """Apply validated program/deployment models to the runtime session.

        If runtime overrides exist, this method rejects by default.
        Pass ``force_discard_overrides=True`` to explicitly discard them,
        or materialize/clear them before calling this method.
        """
        if self._runtime_overrides:
            if force_discard_overrides:
                self._clear_runtime_overrides()
            else:
                raise RuntimeError(
                    "Active or staged runtime overrides exist; "
                    "materialize (promote) or clear them before changing the program, "
                    "or pass force_discard_overrides=True to discard"
                )
        self._rebuild_runtime(program, deployment, effective_program=program)

    def load_program(
        self,
        program_path: str,
        deployment_path: Optional[str] = None,
        *,
        force_discard_overrides: bool = False,
    ) -> Program:
        """Load a program (and optional deployment). Creates new runner + CycleManager.

        If runtime overrides exist from a previous program, this method rejects
        by default.  Pass ``force_discard_overrides=True`` to explicitly discard
        them.
        """
        if self._runtime_overrides:
            if not force_discard_overrides:
                raise RuntimeError(
                    "Active or staged runtime overrides exist; "
                    "materialize (promote) or clear them before loading a new program, "
                    "or pass force_discard_overrides=True to discard"
                )
            self._clear_runtime_overrides()

        # Stop existing cycle manager if running
        if self.cycle_manager is not None:
            self.cycle_manager.reset()

        # Load deployment first (if provided)
        scene_snapshot_provider = (
            getattr(self.hmi_server, "get_cached_scene_snapshot", None)
            or getattr(self.hmi_server, "render_scene", None)
        )
        deployment = self.deployment
        if deployment_path:
            deployment = Deployment(deployment_path)
            deployment.validate_assistant_modes(self.catalog, self.skill_catalog)
            self.deployment = deployment
            self.deployment_source_path = deployment_path
        elif deployment is not None:
            deployment.validate_assistant_modes(self.catalog, self.skill_catalog)

        if deployment is not None and self._kernel_factory is not None:
            self._kernel, self.kernel_bundle = self._kernel_factory(deployment)

        # Create runner
        runner = OrchestratorRunner(
            catalog_path=self._catalog_path,
            program_path=program_path,
            kernel=self._kernel,
            deployment_path=deployment_path,
            deployment=deployment,
            skill_catalog=self.skill_catalog,
            event_recorder=self.event_recorder,
            scene_snapshot_provider=scene_snapshot_provider,
            scene_snapshot_url_publisher=self.publish_scene_snapshot,
        )
        runner.load()

        self.program = runner.program
        self._effective_program = runner.program
        self.program_source_path = program_path
        self._update_program_tree_hash()
        self._program_session_generation += 1
        self._clear_published_scene_snapshots()
        self.cycle_manager = CycleManager(runner, self._kernel)
        self._bind_cycle_manager_callbacks()

        # Notify listeners (e.g. Viser HMI)
        if self._on_program_loaded:
            try:
                self._on_program_loaded(self)
            except Exception:
                logger.exception("on_program_loaded callback failed")

        return self.program
