"""Recover cell: one server-side operator action (recovery design step 0.3).

It does what Composer chained in the browser, with a bounded wait:

1. acknowledge every open component incident with one CCLC ``recover``,
   fenced on that component's current snapshot;
2. read the Stop-fault offer until it is ``ready``, an incident is open again,
   or the bounded wait ends;
3. submit the exact ``recover_stop_fault`` built from that fresh offer and the
   current execution owner epoch; a moved assessment of the same latch is
   retried once, a replacing latch is never recovered by this press;
4. submit one CCLC ``recover`` for each component that was already blocked at
   the press with its recovered cause persisting (the press after an e-stop is
   released, design step 0.2 / R02), fenced on a fresh snapshot.

Reads fail closed: an owner refusal or an incomplete projection is
``cell_state_unavailable``, never "direct control" or "no components".

Replay: a repeated request ID returns the stored result and reads nothing.
This holds per Headless process only (bounded in-memory map, lost on restart
or eviction); the GUI sends a fresh ID per press. There is no durable ledger.
An unexpected error or cancellation stores ``cell_recover_outcome_unknown``
for the ID: a step may already have run, so a replay never starts again.

The client sends only a request identity. Every fence is read here, from the
current owners. CCLC and the Kernel still decide: the latch's clearing
conditions (rest, no uncertain operation, no open incident, configuration) are
unchanged, and an unready latch is never cleared.
"""

from __future__ import annotations

import hashlib
import logging
from collections import OrderedDict
from collections.abc import Awaitable, Callable, Mapping
from dataclasses import dataclass
from typing import Any

from .cclc import CCLCOwnerRefusal
from .native import HeadlessApiError

_LOGGER = logging.getLogger(__name__)

CELL_RECOVER_RESULT_VERSION = "manyforge.cell_recover_result.v1"
# Reference note for the Recover cell route (rendered into the REST references).
CELL_RECOVER_REFERENCE_NOTE = (
    "Recover cell, one server-side composite. Send only `request_context` with "
    "`request_id`, `source_label` and an optional `correlation_id`; every fence "
    "is read from the current owners. It acknowledges open component incidents, "
    "attempts the current cell Stop-fault latch recovery, and submits Recover "
    "for components whose previously recovered cause was still reported at the "
    "press and remains so. It then tries to enable again each component "
    "whose Active target an incident retired (Recover, Prepare, Enable; "
    "`component_rearm_*` reasons, for example the emergency stop still engaged), "
    "listing it in `recovered_components` on success. It is also the re-arm of a "
    "robot moved from its own interface on an idle cell: Execute and Run "
    "(`operation_start`, `program_run`) are refused `component_rearm_required` "
    "(no effect; `details.components`, and `details.in_motion` while it still "
    "moves) until it is re-armed. Clients call this route, wait until the "
    "component is `active` (`GET /api/cclc/components/{id}`, no "
    "`moved_externally:*` reason), then re-send the command with fresh fences; "
    "a robot still moving is refused `component_rearm_required` here too. "
    "A refused recovery "
    "still returns a JSON result: read `outcome` (`done` or `not_recovered`), "
    "`reason.code`, `acknowledged_incidents`, `latch_cleared` and "
    "`recovered_components` rather than treating HTTP success as recovery. "
    "Example reason codes (not exhaustive): `stop_fault_recovery_unavailable`, "
    "`stop_fault_recovery_closing` (Scenario Close keeps the latch; the latch "
    "recovery was refused, but incident acknowledgements earlier in the same "
    "press may already have happened), `stop_fault_latch_replaced`, "
    "`cell_state_unavailable`, `cell_recover_in_progress` and "
    "`cell_recover_outcome_unknown` (a step may have reached its owner: inspect "
    "and reconcile, do not assume nothing happened). `done` does not prove that "
    "every robot is ready to move. Repeating a `request_id` returns the stored "
    "result, or `cell_recover_in_progress`, without reading state again; this "
    "replay is in memory per Headless process, bounded, and lost on restart or "
    "eviction. Use a new `request_id` for each new press."
)
# How long one request waits for the latch to become recoverable.
CELL_RECOVER_WAIT_SECONDS = 5.0
CELL_RECOVER_POLL_SECONDS = 0.2
# The first submission plus one retry after the assessment moved under it.
_MAXIMUM_LATCH_SUBMISSIONS = 2
_RESUBMITTABLE_CODES = frozenset(
    {
        "stop_fault_recovery_identity_mismatch",
        "stop_fault_recovery_not_ready",
    }
)
# Kernel latch-recovery refusals decided before any durable write (D6).
_REFUSED_BEFORE_WRITE = frozenset({"stop_fault_recovery_closing"})
_REASON_MESSAGES = {
    "incident_open": "A component incident is open again; its cause is still present.",
    "incident_acknowledgement_refused": "CCLC refused the incident acknowledgement.",
    "stop_fault_recovery_unavailable": "The cell Stop has no configuration proof to recover from.",
    "stop_fault_recovery_waiting_for_observation": (
        "Waiting for a fresh controller observation or an uncertain operation to resolve."
    ),
    "stop_fault_recovery_waiting_for_quiescence": "The arm is not proven at rest.",
    "stop_fault_recovery_waiting_for_configuration": (
        "The control configuration is not confirmed."
    ),
    "stop_fault_recovery_waiting_for_external_reconciliation": (
        "External reconciliation is required."
    ),
    "stop_fault_recovery_waiting_for_durable_record": "The Stop record is not saved yet.",
    "stop_fault_recovery_pending": "A recovery is already in progress.",
    "cell_state_unavailable": "A cell owner did not report its current state.",
    "stop_fault_recovery_closing": (
        "Scenario Close is in progress and keeps the cell Stop fault. "
        "Finish Close with New or Load, then press Recover cell."
    ),
    "component_recover_refused": "CCLC refused the component Recover.",
    "component_rearm_refused": "CCLC refused a step of enabling the robot again.",
    "component_rearm_failed": (
        "Enabling the robot again failed (is the emergency stop still engaged?); "
        "the error is kept. Release it and press Recover cell again."
    ),
    "component_rearm_timeout": "Enabling the robot again did not finish in time.",
    "component_rearm_required": (
        "The robot was moved outside ManyForge and is still moving; wait until it stops."
    ),
    "stop_fault_latch_replaced": (
        "A newer cell Stop latch replaced the one this press saw; press Recover cell again."
    ),
    "cell_recover_in_progress": "This Recover cell request is still running.",
    "cell_recover_outcome_unknown": (
        "The outcome is not confirmed: a request may have reached its owner. "
        "Listed steps are confirmed; anything else is unknown, not proven undone."
    ),
}
# Completed results kept for exact request replay (in memory, bounded).
CELL_RECOVER_LEDGER_SIZE = 256


@dataclass(frozen=True, slots=True)
class CellRecoverPorts:
    """The current owners, each read or written once per call."""

    execution_identity: Callable[[], Awaitable[Mapping[str, Any]]]
    execution_targets: Callable[[], Awaitable[Mapping[str, Any]]]
    cclc_owner_epoch: Callable[[], Awaitable[str]]
    component_snapshot: Callable[[str], Awaitable[Mapping[str, Any]]]
    admission_generation: Callable[[], Awaitable[int]]
    submit_lifecycle: Callable[[dict[str, Any]], Awaitable[Mapping[str, Any]]]
    stop_fault: Callable[[], Awaitable[Mapping[str, Any]]]
    submit_stop_recovery: Callable[[dict[str, Any]], Awaitable[Mapping[str, Any]]]
    monotonic: Callable[[], float]
    sleep: Callable[[float], Awaitable[None]]
    # m5.5: re-arm the listed components (`component_rearm.reenable_components`):
    # Recover, Prepare and Enable; only Recover for those in the second list
    # (moved from their own interface). Returns the re-armed ones.
    reenable: Callable[[list[str], frozenset[str]], Awaitable[list[str]]] | None = None


class _NotRecovered(Exception):
    def __init__(self, code: str, details: Mapping[str, Any] | None = None) -> None:
        super().__init__(code)
        self.code = code
        self.details = {key: str(value) for key, value in (details or {}).items()}


def derived_request_id(request_id: str, step: str) -> str:
    """A bounded, deterministic owner request ID for one step of one request."""

    digest = hashlib.sha256(f"{request_id}\0{step}".encode("utf-8")).hexdigest()
    return "cell-recover." + digest[:32]


def _expected_domain(domain: Mapping[str, Any]) -> dict[str, Any]:
    active = domain.get("active_control_configuration")
    expected_active = (
        {"presence": "absent"}
        if active is None
        else {
            "presence": "present",
            "active_control_configuration_id": active["active_control_configuration_id"],
            "active_control_configuration_generation": active[
                "active_control_configuration_generation"
            ],
        }
    )
    common = {
        "kind": domain["kind"],
        "control_domain_id": domain["control_domain_id"],
        "control_domain_revision": domain["control_domain_revision"],
        "expected_active_control_configuration": expected_active,
    }
    if domain["kind"] == "io_provider":
        return {**common, "provider": dict(domain["provider"])}
    return {
        **common,
        "controller_topology_id": domain["controller_topology_id"],
        "topology_revision": domain["topology_revision"],
        "expected_control_bindings": [
            {
                "control_binding_id": binding["control_binding_id"],
                "control_binding_revision": binding["control_binding_revision"],
                "control_route_id": binding["control_route_id"],
            }
            for binding in domain["current_control_bindings"]
        ],
    }


def incident_recover_request(
    snapshot: Mapping[str, Any],
    *,
    request_context: dict[str, Any],
    admission_generation: int,
) -> dict[str, Any]:
    """The operator's CCLC ``recover``, fenced on one current snapshot."""

    try:
        return {
            "request_context": request_context,
            "controllable_component_id": snapshot["controllable_component_id"],
            "intent": "recover",
            "expected_graph_revision": snapshot["graph_revision"],
            "expected_component_lifecycle_generation": snapshot[
                "component_lifecycle_generation"
            ],
            "expected_control_domains": [
                _expected_domain(domain) for domain in snapshot["domains"]
            ],
            "reason": "operator-confirmed Recover cell",
            "confirmed": True,
            "expected_admission_generation": admission_generation,
        }
    except (KeyError, TypeError) as error:
        raise HeadlessApiError(
            "cclc_open_response_invalid",
            "CCLC component snapshot lacks a lifecycle fence",
        ) from error


def stop_fault_recovery_request(
    offer: Mapping[str, Any],
    *,
    request_context: dict[str, Any],
) -> dict[str, Any]:
    """The exact ``recover_stop_fault`` for the offer just read."""

    latch = offer["latch"]
    certificate = latch["stop_certificate"]
    return {
        "request_context": request_context,
        "expected_latch_id": latch["latch_id"],
        "expected_latch_generation": latch["latch_generation"],
        "expected_recovery_assessment_revision": latch["recovery_assessment_revision"],
        "expected_stop_certificate_id": certificate["stop_certificate_id"],
        "expected_stop_certificate_revision": certificate["stop_certificate_revision"],
        "expected_cell_id": latch["cell_id"],
        "expected_retained_bundle_id": latch["retained_bundle_id"],
        "expected_graph_revision": latch["graph_revision"],
        "configuration_expectations": [
            dict(row) for row in offer["configuration_expectations"]
        ],
        "reason": "operator requested cell recovery",
    }


# m5.5: the target driver's warning for a robot moved from its own interface
# (same strings as `component_rearm`, which imports this module).
_MOVED_EXTERNALLY_PREFIX = "moved_externally:"
MOVED_EXTERNALLY_IN_MOTION = "moved_externally:in_motion"


def _progress_reason(snapshot: Mapping[str, Any]) -> str | None:
    progress = snapshot.get("lifecycle_progress")
    reason = progress.get("blocking_reason") if isinstance(progress, Mapping) else None
    return reason if isinstance(reason, str) else None


def moved_externally(snapshot: Mapping[str, Any]) -> bool:
    reason = _progress_reason(snapshot)
    return reason is not None and reason.startswith(_MOVED_EXTERNALLY_PREFIX)


def _incident(snapshot: Mapping[str, Any]) -> Mapping[str, Any] | None:
    progress = snapshot.get("lifecycle_progress")
    incident = progress.get("incident") if isinstance(progress, Mapping) else None
    return incident if isinstance(incident, Mapping) else None


# The R02 driver's projection of "the operator's Recover ran, the cause is
# still reported": blocked, next intent recover, no incident.
_RECOVERED_CAUSE_PERSISTS = "recovered_cause_persists:"
_CONTROL_MODES = frozenset({"component_lifecycle", "direct"})


def _cause_persists(snapshot: Mapping[str, Any]) -> bool:
    progress = snapshot.get("lifecycle_progress")
    return (
        isinstance(progress, Mapping)
        and progress.get("incident") is None
        and progress.get("phase") == "blocked"
        and progress.get("next_intent") == "recover"
        and isinstance(progress.get("blocking_reason"), str)
        and progress["blocking_reason"].startswith(_RECOVERED_CAUSE_PERSISTS)
    )


def _unavailable(source: str, response: Mapping[str, Any] | None = None) -> _NotRecovered:
    error = response.get("error") if isinstance(response, Mapping) else None
    code = error.get("code") if isinstance(error, Mapping) else None
    return _NotRecovered(
        "cell_state_unavailable",
        {"source": source, **({"code": code} if isinstance(code, str) else {})},
    )


def _waiting_code(latch: Mapping[str, Any]) -> str:
    status = latch.get("recovery_status")
    if status == "recovery_pending":
        return "stop_fault_recovery_pending"
    return f"stop_fault_recovery_{status}"


def _latch_details(latch: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "latch_id": latch.get("latch_id"),
        "recovery_status": latch.get("recovery_status"),
    }


class _CellRecovery:
    def __init__(
        self,
        ports: CellRecoverPorts,
        *,
        request_id: str,
        source_label: str,
        correlation_id: str | None,
        wait_seconds: float,
        poll_seconds: float,
        pending_reenable: set[str] | None = None,
    ) -> None:
        self._ports = ports
        # m5.5: components whose re-enable after an incident has not
        # succeeded yet (kept by the ledger, in memory), and this press's.
        self._pending_reenable = pending_reenable if pending_reenable is not None else set()
        self._reenable: list[str] = []
        self._moved: frozenset[str] = frozenset()
        self._moved_in_motion: list[str] = []
        self._request_id = request_id
        self._source_label = source_label
        self._correlation_id = correlation_id or request_id
        self._wait_seconds = wait_seconds
        self._poll_seconds = poll_seconds
        self._component_ids: list[str] | None = None
        self._persisting: list[str] = []
        self.recovered_components: list[str] = []
        self.acknowledged: list[dict[str, str]] = []
        self.latch_cleared = False

    def _context(self, owner_epoch: str, step: str) -> dict[str, Any]:
        return {
            "request_id": derived_request_id(self._request_id, step),
            "request_owner_epoch": owner_epoch,
            "source_label": self._source_label,
            "correlation_id": self._correlation_id,
        }

    async def _components(self) -> list[str]:
        """The listed components, or a typed unavailable: never assumed empty."""

        if self._component_ids is not None:
            return self._component_ids
        response = await self._ports.execution_identity()
        identity = response.get("identity")
        mode = identity.get("control_management_mode") if isinstance(identity, Mapping) else None
        if response.get("ok") is not True or mode not in _CONTROL_MODES:
            raise _unavailable("execution_identity", response)
        if mode == "direct":
            # Direct control has no CCLC owner and no component incidents.
            self._component_ids = []
            return self._component_ids
        response = await self._ports.execution_targets()
        targets = response.get("targets")
        rows = targets.get("components") if isinstance(targets, Mapping) else None
        if (
            response.get("ok") is not True
            or not isinstance(rows, list)
            or not all(
                isinstance(row, Mapping)
                and isinstance(row.get("controllable_component_id"), str)
                and row["controllable_component_id"]
                for row in rows
            )
        ):
            raise _unavailable("execution_targets", response)
        self._component_ids = [row["controllable_component_id"] for row in rows]
        return self._component_ids

    async def _snapshots(self) -> list[Mapping[str, Any]]:
        return [
            await self._ports.component_snapshot(component_id)
            for component_id in await self._components()
        ]

    async def _open_incidents(self) -> list[tuple[Mapping[str, Any], Mapping[str, Any]]]:
        rows = []
        for snapshot in await self._snapshots():
            incident = _incident(snapshot)
            if incident is not None:
                rows.append((snapshot, incident))
        return rows

    async def _submit_recover(self, snapshot: Mapping[str, Any], step: str, refusal: str) -> Mapping[str, Any]:
        component_id = snapshot["controllable_component_id"]
        request = incident_recover_request(
            snapshot,
            request_context=self._context(
                await self._ports.cclc_owner_epoch(), f"{step}.{component_id}"
            ),
            admission_generation=await self._ports.admission_generation(),
        )
        result = await self._submit(
            self._ports.submit_lifecycle(request),
            {"step": step, "controllable_component_id": component_id},
        )
        # As in the lifecycle panel: an authorization-only Recover closes
        # the incident; an accepted transaction is an ordinary Recover.
        if (
            result.get("incident_recovery") is None
            and result.get("acceptance") is None
            and result.get("stream_acknowledgement") is None
        ):
            rejection = result.get("rejection")
            raise _NotRecovered(
                refusal,
                {
                    "controllable_component_id": component_id,
                    "replay_status": result.get("replay_status"),
                    **(
                        {"rejection_code": rejection.get("code")}
                        if isinstance(rejection, Mapping)
                        else {}
                    ),
                },
            )
        return result

    async def acknowledge_incidents(self) -> None:
        snapshots = await self._snapshots()
        # Components already blocked on a persisting recovered cause at the
        # press get their Recover after the latch (step 4).
        self._persisting = [
            snapshot["controllable_component_id"]
            for snapshot in snapshots
            if _cause_persists(snapshot)
        ]
        # m5.5: an incident retires an Active target; the operator's press
        # tries to enable such a component again (Recover, Prepare, Enable),
        # and again on a later press while that has not succeeded.
        self._reenable = [
            snapshot["controllable_component_id"]
            for snapshot in snapshots
            if snapshot.get("lifecycle_target") is None
            and (
                _incident(snapshot) is not None
                or snapshot["controllable_component_id"] in self._pending_reenable
            )
        ]
        # m5.5 FIX5: Recover cell is also the re-arm of a robot moved from its
        # own interface on an idle cell (no incident; its target is kept).
        moved = [
            snapshot
            for snapshot in snapshots
            if _incident(snapshot) is None and moved_externally(snapshot)
        ]
        self._moved_in_motion = [
            snapshot["controllable_component_id"]
            for snapshot in moved
            if _progress_reason(snapshot) == MOVED_EXTERNALLY_IN_MOTION
        ]
        self._moved = frozenset(
            snapshot["controllable_component_id"] for snapshot in moved
        ) - set(self._moved_in_motion)
        self._reenable += sorted(self._moved - set(self._reenable))
        for snapshot in snapshots:
            if _incident(snapshot) is None:
                continue
            component_id = snapshot["controllable_component_id"]
            result = await self._submit_recover(
                snapshot, "incident", "incident_acknowledgement_refused"
            )
            # Report the incident CCLC actually acknowledged. A plain Recover is
            # not pinned to the observed incident (open point, see report).
            acknowledged = result.get("incident_recovery")
            if isinstance(acknowledged, Mapping):
                self.acknowledged.append(
                    {
                        "controllable_component_id": component_id,
                        "incident_id": str(acknowledged.get("incident_id")),
                        "cause_class": str(acknowledged.get("cause_class")),
                    }
                )

    @staticmethod
    async def _submit(
        submission: Awaitable[Mapping[str, Any]], details: Mapping[str, Any]
    ) -> Mapping[str, Any]:
        """One owner write. A reply is confirmed; an error may follow a send.

        A refusal the owner returned stays typed: a reply carrying its
        rejection, or a CCLC error envelope marked ``effect=not_started``.
        Any other error here (an owner error without that evidence, or for
        example a timeout after the send, or a reply that fails validation)
        does not prove the owner refused, so its outcome is unknown.
        """

        try:
            return await submission
        except CCLCOwnerRefusal as error:
            # A CCLC error envelope proves no effect only with the owner's
            # existing evidence (`effect=not_started`, set where it retains
            # the rejection); any other owner error may follow a durable claim.
            if error.details.get("effect") == "not_started":
                raise _NotRecovered(error.code, {**details, "message": str(error)}) from error
            raise _NotRecovered(
                "cell_recover_outcome_unknown", {**details, "code": error.code}
            ) from error
        except HeadlessApiError as error:
            raise _NotRecovered(
                "cell_recover_outcome_unknown", {**details, "code": error.code}
            ) from error

    async def _raise_if_incident_open(self) -> None:
        open_incidents = await self._open_incidents()
        if open_incidents:
            snapshot, incident = open_incidents[0]
            raise _NotRecovered(
                "incident_open",
                {
                    "controllable_component_id": snapshot["controllable_component_id"],
                    "incident_id": incident.get("incident_id"),
                    "cause_class": incident.get("cause_class"),
                },
            )

    async def reenable_components(self) -> None:
        """After the latch: try to enable again what an incident disabled.

        A refused or failed attempt (for example the emergency stop still
        engaged: ``component_rearm_failed`` with the vendor code) is this
        press's typed reason; it opens nothing and can be retried.
        """

        if self._moved_in_motion:
            # A robot still moving under its own controller is never re-armed.
            raise _NotRecovered(
                "component_rearm_required",
                {
                    "components": ",".join(self._moved_in_motion),
                    "in_motion": ",".join(self._moved_in_motion),
                },
            )
        if not self._reenable or self._ports.reenable is None:
            return
        self._pending_reenable.update(set(self._reenable) - self._moved)
        try:
            reenabled = await self._ports.reenable(list(self._reenable), self._moved)
        except CCLCOwnerRefusal as error:
            # As every other step here: a refusal proves no effect only with
            # the owner's `effect=not_started`; otherwise the outcome is unknown.
            if error.details.get("effect") == "not_started":
                raise _NotRecovered(error.code, {"message": str(error)}) from error
            raise _NotRecovered(
                "cell_recover_outcome_unknown", {"step": "reenable", "code": error.code}
            ) from error
        except HeadlessApiError as error:
            if error.code.startswith("component_rearm_"):
                # Typed from a terminal CCLC status or a CCLC rejection reply.
                raise _NotRecovered(error.code, error.details) from error
            raise _NotRecovered(
                "cell_recover_outcome_unknown", {"step": "reenable", "code": error.code}
            ) from error
        for component_id in reenabled:
            self._pending_reenable.discard(component_id)
            self.recovered_components.append(component_id)

    async def recover_persisting_causes(self) -> None:
        """The press after a cause was released: one ordinary Recover each."""

        for component_id in self._persisting:
            snapshot = await self._ports.component_snapshot(component_id)
            if not _cause_persists(snapshot):
                continue  # The driver moved on; nothing for this press to do.
            result = await self._submit_recover(
                snapshot, "recover", "component_recover_refused"
            )
            if result.get("acceptance") is not None:
                self.recovered_components.append(component_id)

    async def _offer(self) -> Mapping[str, Any] | None:
        response = await self._ports.stop_fault()
        if response.get("ok") is not True:
            raise _unavailable("stop_fault", response)
        offer = response.get("stop_fault")
        return offer if isinstance(offer, Mapping) else None

    async def recover_latch(self) -> None:
        deadline = self._ports.monotonic() + self._wait_seconds
        submissions = 0
        pinned: tuple[Any, Any] | None = None
        while True:
            offer = await self._offer()
            if offer is None:
                return
            latch = offer["latch"]
            # This press recovers only the latch it first saw; its assessment
            # revision may move, its identity may not.
            identity = (latch.get("latch_id"), latch.get("latch_generation"))
            if pinned is None:
                pinned = identity
            elif identity != pinned:
                raise _NotRecovered(
                    "stop_fault_latch_replaced",
                    {
                        "latch_id": pinned[0],
                        "latch_generation": pinned[1],
                        "current_latch_id": identity[0],
                        "current_latch_generation": identity[1],
                    },
                )
            if offer.get("recovery_unavailable_reason") in {
                "stop_fault_recovery_closing", "stop_fault_recovery_closing_not_ready"
            }:
                # D6: Scenario Close keeps the latch; a confirmed refusal.
                raise _NotRecovered("stop_fault_recovery_closing", _latch_details(latch))
            if offer.get("recovery_unavailable_reason") is not None:
                raise _NotRecovered(
                    "stop_fault_recovery_unavailable",
                    {
                        **_latch_details(latch),
                        "recovery_unavailable_reason": offer["recovery_unavailable_reason"],
                    },
                )
            if (
                latch.get("recovery_status") == "ready"
                and latch.get("stop_certificate") is not None
            ):
                submissions += 1
                response = await self._ports.execution_identity()
                owner = response.get("identity")
                owner_epoch = owner.get("owner_epoch") if isinstance(owner, Mapping) else None
                if response.get("ok") is not True or not isinstance(owner_epoch, str) or not owner_epoch:
                    raise _unavailable("execution_identity", response)
                response = await self._submit(
                    self._ports.submit_stop_recovery(
                        stop_fault_recovery_request(
                            offer,
                            request_context=self._context(owner_epoch, f"latch.{submissions}"),
                        )
                    ),
                    {"step": "latch", "latch_id": latch.get("latch_id")},
                )
                result = response.get("result")
                if response.get("ok") is True and isinstance(result, Mapping):
                    if result.get("accepted") is True:
                        self.latch_cleared = True
                        return
                    error = result.get("error")
                    latch = result.get("latch") if isinstance(result.get("latch"), Mapping) else latch
                elif response.get("ingress_rejected") is True:
                    # Refused by the Headless ingress before the Kernel was called.
                    error = response.get("error")
                else:
                    # A Kernel error reply does not prove no effect (for example
                    # `stop_fault_recovery_postcommit_conflict` after a durable
                    # clear). Only `accepted: false` above is a refusal, plus
                    # the Kernel refusals checked before any write.
                    error = response.get("error")
                    code = error.get("code") if isinstance(error, Mapping) else None
                    if code in _REFUSED_BEFORE_WRITE:
                        raise _NotRecovered(code, _latch_details(latch))
                    raise _NotRecovered(
                        "cell_recover_outcome_unknown",
                        {
                            "step": "latch",
                            "latch_id": latch.get("latch_id"),
                            **{
                                key: error[key]
                                for key in ("code", "message")
                                if isinstance(error, Mapping) and isinstance(error.get(key), str)
                            },
                        },
                    )
                code = error.get("code") if isinstance(error, Mapping) else None
                if not isinstance(code, str) or not code:
                    code = "stop_fault_recovery_rejected"
                if code not in _RESUBMITTABLE_CODES or submissions >= _MAXIMUM_LATCH_SUBMISSIONS:
                    if code == "stop_fault_recovery_not_ready":
                        code = _waiting_code(latch)
                    raise _NotRecovered(code, _latch_details(latch))
                if code == "stop_fault_recovery_identity_mismatch":
                    # The assessment moved; the Kernel published it. Re-read now.
                    continue
            else:
                # An incident that opens again needs another operator Recover;
                # waiting longer cannot clear it.
                await self._raise_if_incident_open()
            if self._ports.monotonic() >= deadline:
                raise _NotRecovered(_waiting_code(latch), _latch_details(latch))
            await self._ports.sleep(self._poll_seconds)

    @staticmethod
    def _typed(request_id: str, code: str) -> dict[str, Any]:
        return {
            "schema_version": CELL_RECOVER_RESULT_VERSION,
            "request_id": request_id,
            "outcome": "not_recovered",
            "acknowledged_incidents": [],
            "latch_cleared": False,
            "recovered_components": [],
            "reason": {"code": code, "message": _REASON_MESSAGES[code], "details": {}},
        }

    @classmethod
    def in_progress(cls, request_id: str) -> dict[str, Any]:
        return cls._typed(request_id, "cell_recover_in_progress")

    @classmethod
    def outcome_unknown(cls, request_id: str) -> dict[str, Any]:
        return cls._typed(request_id, "cell_recover_outcome_unknown")

    def result(self, reason: _NotRecovered | None) -> dict[str, Any]:
        return {
            "schema_version": CELL_RECOVER_RESULT_VERSION,
            "request_id": self._request_id,
            "outcome": "done" if reason is None else "not_recovered",
            "acknowledged_incidents": list(self.acknowledged),
            "latch_cleared": self.latch_cleared,
            "recovered_components": list(self.recovered_components),
            "reason": (
                None
                if reason is None
                else {
                    "code": reason.code,
                    "message": _REASON_MESSAGES.get(
                        reason.code, "Cell recovery was refused by its owner."
                    ),
                    "details": reason.details,
                }
            ),
        }


class CellRecoverLedger:
    """Exact replay of one Recover cell request ID, in memory and bounded.

    A repeated request ID returns its stored result, or ``in_progress`` while
    the first is running, and never reads current state again: an old press
    must not act on a latch or incident that appeared after it.
    """

    def __init__(self, maximum: int = CELL_RECOVER_LEDGER_SIZE) -> None:
        self._maximum = maximum
        # m5.5: components to try enabling again on the next press.
        self.pending_reenable: set[str] = set()
        self._rows: OrderedDict[str, dict[str, Any] | None] = OrderedDict()

    def begin(self, request_id: str) -> dict[str, Any] | None:
        """Claim a new request ID, or return what a repeat of it answers."""

        if request_id in self._rows:
            stored = self._rows[request_id]
            if stored is not None:
                return stored
            return _CellRecovery.in_progress(request_id)
        self._rows[request_id] = None
        return None

    def finish(self, request_id: str, result: dict[str, Any] | None) -> None:
        """Store the final result.

        ``None`` (an unexpected error or cancellation) stores
        ``cell_recover_outcome_unknown``: an owner request may already have
        been dispatched, so a replay must never start a new composite.
        """

        self._rows[request_id] = (
            _CellRecovery.outcome_unknown(request_id) if result is None else result
        )
        completed = [key for key, value in self._rows.items() if value is not None]
        for key in completed[: max(0, len(completed) - self._maximum)]:
            del self._rows[key]


async def recover_cell_once(
    ledger: CellRecoverLedger,
    ports: CellRecoverPorts,
    *,
    request_id: str,
    source_label: str,
    correlation_id: str | None = None,
) -> tuple[dict[str, Any], bool]:
    """Run one request ID at most once; returns (result, replayed)."""

    replayed = ledger.begin(request_id)
    if replayed is not None:
        return replayed, True
    recovery = _CellRecovery(
        ports,
        request_id=request_id,
        source_label=source_label,
        correlation_id=correlation_id,
        wait_seconds=CELL_RECOVER_WAIT_SECONDS,
        poll_seconds=CELL_RECOVER_POLL_SECONDS,
        pending_reenable=ledger.pending_reenable,
    )
    result = None
    try:
        result = await _run(recovery, request_id)
    finally:
        if result is None:
            # Cancelled: store the unknown outcome with the steps confirmed so
            # far; the cancellation propagates.
            result = recovery.result(
                _NotRecovered("cell_recover_outcome_unknown", {"error": "cancelled"})
            )
        ledger.finish(request_id, result)
    return result, False


async def _run(recovery: "_CellRecovery", request_id: str) -> dict[str, Any]:
    try:
        await recovery.acknowledge_incidents()
        await recovery.recover_latch()
        await recovery.recover_persisting_causes()
        await recovery.reenable_components()
    except _NotRecovered as reason:
        return recovery.result(reason)
    except HeadlessApiError as error:
        # A read failed (no write was sent at this step): its typed code, with
        # any step already confirmed.
        return recovery.result(_NotRecovered(error.code, {"message": str(error)}))
    except Exception as error:
        _LOGGER.exception("Recover cell %s failed unexpectedly", request_id)
        return recovery.result(
            _NotRecovered("cell_recover_outcome_unknown", {"error": type(error).__name__})
        )
    return recovery.result(None)


async def recover_cell(
    ports: CellRecoverPorts,
    *,
    request_id: str,
    source_label: str,
    correlation_id: str | None = None,
    wait_seconds: float = CELL_RECOVER_WAIT_SECONDS,
    poll_seconds: float = CELL_RECOVER_POLL_SECONDS,
) -> dict[str, Any]:
    """Acknowledge incidents, then recover the current latch; one typed result."""

    recovery = _CellRecovery(
        ports,
        request_id=request_id,
        source_label=source_label,
        correlation_id=correlation_id,
        wait_seconds=wait_seconds,
        poll_seconds=poll_seconds,
    )
    return await _run(recovery, request_id)


__all__ = [
    "CELL_RECOVER_RESULT_VERSION",
    "CELL_RECOVER_WAIT_SECONDS",
    "CellRecoverLedger",
    "CellRecoverPorts",
    "derived_request_id",
    "incident_recover_request",
    "recover_cell",
    "recover_cell_once",
    "stop_fault_recovery_request",
]
