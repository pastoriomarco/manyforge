"""Re-arm a robot before ManyForge motion, and re-enable it after an incident (m5.5).

Maintainer (2026-10-28), manual move: "Instead of automatically putting the
robot back to the automatic state/mode, since the manual motion is not a
problem only when the program is idle [...] could we put up a warning similar
to when we lose the joint stream in isaac? Then on execute or on run the first
thing to do would be to put the robot in the correct state."

Maintainer, e-stop: "we should give a way to the user to try enabling the
robot. If the estop is not released obviously the activation would fail and
the error would be kept (not escalated to cell lock or something like that)."

Both use the existing CCLC chain. ``require_no_external_move`` runs in the
control dispatch before a motion command (Execute, Run, jog, Move-to) from
every transport and refuses it ``component_rearm_required`` without any
effect (FIX5). The re-arm is Recover cell: ``reenable_components`` runs the
operator's Recover for a moved robot (the driver then re-drives Prepare and
Enable to its Active target), and Recover, Prepare and Enable for a
component whose Active target an incident retired. The Composer UI calls
Recover cell on that refusal, waits until the robot is active and re-sends
the command once.

Every failure is a typed refusal that names the component and, when CCLC
gave one, the failed step's error (for example ``xarm_error_persists_after_clear``
with ``vendor_code`` ``C2``). Nothing here opens an incident or a latch. A
started Recover is an effect: the refusals say which re-arms started; a
failed step leaves the robot disarmed with its error reported, and the
attempt can be retried.
"""

from __future__ import annotations

import hashlib
from collections.abc import Awaitable, Callable, Mapping
from dataclasses import dataclass
from typing import Any

from .cell_recover import (
    MOVED_EXTERNALLY_IN_MOTION,
    incident_recover_request,
    moved_externally,
)
from .native import HeadlessApiError

# Three Real lifecycle steps (Recover, Prepare, Enable), each bounded at 10 s.
REARM_TIMEOUT_SECONDS = 30.0
REARM_POLL_SECONDS = 0.2
# Motion command kinds refused while a robot needs re-arming.
REARM_BEFORE_COMMAND_KINDS = frozenset({"operation_start", "program_run"})


@dataclass(frozen=True, slots=True)
class ComponentRearmPorts:
    component_ids: Callable[[], Awaitable[list[str]]]
    component_snapshot: Callable[[str], Awaitable[Mapping[str, Any]]]
    cclc_owner_epoch: Callable[[], Awaitable[str]]
    admission_generation: Callable[[], Awaitable[int]]
    submit_lifecycle: Callable[[dict[str, Any]], Awaitable[Mapping[str, Any]]]
    lifecycle_status: Callable[[str], Awaitable[Mapping[str, Any] | None]]
    monotonic: Callable[[], float]
    sleep: Callable[[float], Awaitable[None]]


def _progress(snapshot: Mapping[str, Any]) -> Mapping[str, Any]:
    progress = snapshot.get("lifecycle_progress")
    return progress if isinstance(progress, Mapping) else {}


def _request_id(base: str, component_id: str, step: str) -> str:
    digest = hashlib.sha256(f"{base}\0{component_id}\0{step}".encode("utf-8")).hexdigest()
    return "component-rearm." + digest[:32]


def _failure(code: str, component_id: str, message: str, **details: Any) -> HeadlessApiError:
    return HeadlessApiError(
        code,
        message,
        details={
            "controllable_component_id": component_id,
            **{key: str(value) for key, value in details.items() if value is not None},
        },
    )


class _Rearm:
    def __init__(self, ports: ComponentRearmPorts, request_id: str, source_label: str) -> None:
        self._ports = ports
        self._request_id = request_id
        self._source_label = source_label
        self._deadline = ports.monotonic() + REARM_TIMEOUT_SECONDS

    def _remaining(self) -> bool:
        return self._ports.monotonic() < self._deadline

    async def run_intent(self, component_id: str, intent: str, *, wait: bool = True) -> None:
        """One operator lifecycle request, waited to its terminal result."""

        snapshot = await self._ports.component_snapshot(component_id)
        request = incident_recover_request(
            snapshot,
            request_context={
                "request_id": _request_id(self._request_id, component_id, intent),
                "request_owner_epoch": await self._ports.cclc_owner_epoch(),
                "source_label": self._source_label,
                "correlation_id": None,
            },
            admission_generation=await self._ports.admission_generation(),
        )
        request["intent"] = intent
        request["reason"] = f"operator re-arm: {intent}"
        result = await self._ports.submit_lifecycle(request)
        acceptance = result.get("acceptance")
        if not isinstance(acceptance, Mapping):
            rejection = result.get("rejection")
            raise _failure(
                "component_rearm_refused",
                component_id,
                f"CCLC refused the re-arm {intent}",
                step=intent,
                rejection_code=rejection.get("code") if isinstance(rejection, Mapping) else None,
            )
        if not wait:
            return
        transaction_id = acceptance["lifecycle_transaction_id"]
        while self._remaining():
            status = await self._ports.lifecycle_status(transaction_id)
            terminal = status.get("terminal_result") if status else None
            if terminal == "succeeded":
                return
            if terminal is not None:
                error = status.get("error") if status else None
                error = error if isinstance(error, Mapping) else {}
                details = error.get("details") if isinstance(error.get("details"), Mapping) else {}
                raise _failure(
                    "component_rearm_failed",
                    component_id,
                    f"the re-arm {intent} failed",
                    step=intent,
                    terminal_result=terminal,
                    error_code=error.get("code"),
                    reason=details.get("reason"),
                    vendor_code=details.get("vendor_code"),
                )
            await self._ports.sleep(REARM_POLL_SECONDS)
        raise _failure(
            "component_rearm_timeout", component_id, f"the re-arm {intent} did not finish", step=intent
        )

def require_no_external_move(service: Any) -> None:
    """Before a motion command, inside the control dispatch (every transport).

    Refuse-only (FIX5): no effect. Called after the ingress's freshness and
    busy checks, the Program availability check and the command's own request
    validation, before the owner. If a robot reports ``moved_externally:*``
    (derived from the robot's own status on every call; Headless keeps
    nothing), the command is refused ``component_rearm_required`` with the
    components and those still ``in_motion``. The client re-arms with Recover
    cell (which runs Recover, then the driver's Prepare and Enable) and
    re-sends the command; the Composer UI does that in one press.
    """

    cclc = getattr(service, "cclc", None)
    if cclc is None:
        return
    try:
        identity = service.execution_identity()
    except HeadlessApiError:
        return
    row = identity.get("identity") if isinstance(identity, Mapping) else None
    if not isinstance(row, Mapping) or row.get("control_management_mode") != "component_lifecycle":
        return
    response = service.execution_targets()
    targets = response.get("targets") if isinstance(response, Mapping) else None
    rows = targets.get("components") if isinstance(targets, Mapping) else None
    if not isinstance(rows, list):
        raise HeadlessApiError(
            "execution_targets_unavailable", "the execution targets are unavailable"
        )
    moved: list[str] = []
    in_motion: list[str] = []
    for target in rows:
        if not isinstance(target, Mapping) or not isinstance(
            target.get("controllable_component_id"), str
        ):
            continue
        snapshot = cclc.component_snapshot(target["controllable_component_id"])
        if not moved_externally(snapshot):
            continue
        moved.append(snapshot["controllable_component_id"])
        if _progress(snapshot).get("blocking_reason") == MOVED_EXTERNALLY_IN_MOTION:
            in_motion.append(snapshot["controllable_component_id"])
    if moved:
        raise HeadlessApiError(
            "component_rearm_required",
            (
                "The robot was moved outside ManyForge and is still moving; wait until it stops."
                if in_motion
                else "The robot was moved outside ManyForge; re-arm it (Recover cell), then retry."
            ),
            details={
                "components": ",".join(moved),
                **({"in_motion": ",".join(in_motion)} if in_motion else {}),
            },
        )


async def reenable_components(
    ports: ComponentRearmPorts,
    component_ids: list[str],
    *,
    request_id: str,
    source_label: str,
    moved: frozenset[str] | set[str] = frozenset(),
) -> list[str]:
    """Recover cell: Recover, Prepare and Enable each listed component.

    A component in ``moved`` (moved from its own interface; its Active target
    is kept) gets only the operator's Recover: the target driver then
    re-drives Prepare and Enable, and the caller waits for it to be active.
    """

    rearm = _Rearm(ports, request_id, source_label)
    for component_id in component_ids:
        intents = ("recover",) if component_id in moved else ("recover", "prepare", "enable")
        for intent in intents:
            await rearm.run_intent(component_id, intent)
    return list(component_ids)
