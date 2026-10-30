"""Re-arm a moved robot with Recover cell, and re-enable it after an incident (m5.5).

Maintainer (2026-10-28), manual move: "Instead of automatically putting the
robot back to the automatic state/mode, since the manual motion is not a
problem only when the program is idle [...] could we put up a warning similar
to when we lose the joint stream in isaac? Then on execute or on run the first
thing to do would be to put the robot in the correct state."

Maintainer, e-stop: "we should give a way to the user to try enabling the
robot. If the estop is not released obviously the activation would fail and
the error would be kept (not escalated to cell lock or something like that)."

M6 (ADAPTERM): a motion command is no longer refused after an idle move.
The Kernel admits it and the robot's vendor adapter re-engages the robot at
rest, inside the motion, before the trajectory is sent. Recover cell keeps
its explicit handling: ``reenable_components`` runs the operator's Recover
for a moved robot (the driver then re-drives Prepare and Enable to its
Active target), and Recover, Prepare and Enable for a component whose Active
target an incident retired.

Every failure is a typed refusal that names the component and, when CCLC
gave one, the failed step's error (for example ``xarm_error_persists_after_clear``
with ``vendor_code`` ``C2``) and the robot's current status
(``vendor_status``, ``ros2_control_status``). Nothing here opens an incident
or a latch. A started Recover is an effect: the refusals say which re-arms
started. A Recover whose error clear does not take effect (the emergency
stop still engaged) changes nothing; a later failed step leaves the robot
disarmed with its error reported. The attempt can be retried.
"""

from __future__ import annotations

import hashlib
from collections.abc import Awaitable, Callable, Mapping
from dataclasses import dataclass
from typing import Any

from .cell_recover import RearmAborted, incident_recover_request
from .native import HeadlessApiError

# M6: each step waits within its own lifecycle allowance: the transaction
# timeout CCLC resolved for that component and intent (the component's
# Deployment `lifecycle_command_timeout_s` when set, else its vendor plan's
# bound), carried by the acceptance as `timeout_duration_ns`. Some
# components start up in minutes. Without one, the step keeps the default
# component lifecycle budget: mirrors core::kDefaultLifecycleBudgetNs
# (component_lifecycle_budget.hpp), 10 s.
REARM_DEFAULT_STEP_TIMEOUT_SECONDS = 10.0
# Past the allowance, so CCLC's own deadline verdict is read first.
REARM_STEP_GRACE_SECONDS = 1.0
REARM_POLL_SECONDS = 0.2


@dataclass(frozen=True, slots=True)
class ComponentRearmPorts:
    component_ids: Callable[[], Awaitable[list[str]]]
    component_snapshot: Callable[[str], Awaitable[Mapping[str, Any]]]
    admission_generation: Callable[[], Awaitable[int]]
    submit_lifecycle: Callable[[dict[str, Any]], Awaitable[Mapping[str, Any]]]
    lifecycle_status: Callable[[str], Awaitable[Mapping[str, Any] | None]]
    monotonic: Callable[[], float]
    sleep: Callable[[float], Awaitable[None]]


# M6 (SX-653): the re-enable names its own requests with this label, so CCLC
# keeps the incident re-enable mark only through its targets; an operator's
# own Prepare ends it (`kCellRecoverReenableSourceLabel` in CCLC).
REENABLE_SOURCE_LABEL = "cell_recover_reenable"


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
    def __init__(
        self,
        ports: ComponentRearmPorts,
        request_id: str,
        owner_epoch: str,
        acknowledge_incident: bool,
        aborted: Callable[[], bool] = lambda: False,
    ) -> None:
        self._ports = ports
        # SX-679/688: the operator's abort, checked right before each write.
        self._aborted = aborted
        self._request_id = request_id
        # M6 (item 35): the Recover cell press's CCLC owner epoch, read once.
        self._owner_epoch = owner_epoch
        # M6 (item 36): false when the press carries the operator's incidents.
        self._acknowledge_incident = acknowledge_incident

    @staticmethod
    def _allowance_seconds(acceptance: Mapping[str, Any]) -> float:
        """The step's lifecycle allowance from its CCLC acceptance."""

        timeout_ns = acceptance.get("timeout_duration_ns")
        if type(timeout_ns) is int and timeout_ns > 0:
            return timeout_ns / 1e9
        return REARM_DEFAULT_STEP_TIMEOUT_SECONDS

    async def run_intent(
        self, component_id: str, intent: str, *, wait: bool = True, marked: bool = False
    ) -> bool | None:
        """One operator lifecycle request, waited to its terminal result.

        With ``marked`` (SX-666/673), the request is built only from a snapshot
        that still carries CCLC's retired-Active mark, and is fenced on that
        snapshot's lifecycle generation: an operator target accepted after
        this read bumps it and CCLC refuses the request. Either way the mark
        is gone and this returns False: a clean stop, nothing submitted
        (or the refused request had no effect).
        """

        snapshot = await self._ports.component_snapshot(component_id)
        if marked and snapshot.get("active_target_retired_by_incident") is not True:
            return False
        request = incident_recover_request(
            snapshot,
            request_context={
                "request_id": _request_id(self._request_id, component_id, intent),
                "request_owner_epoch": self._owner_epoch,
                "source_label": REENABLE_SOURCE_LABEL,
                "correlation_id": None,
            },
            admission_generation=await self._ports.admission_generation(),
            acknowledge_incident=intent != "recover" or self._acknowledge_incident,
        )
        request["intent"] = intent
        request["reason"] = f"operator re-arm: {intent}"
        if self._aborted():
            raise RearmAborted()
        result = await self._ports.submit_lifecycle(request)
        acceptance = result.get("acceptance")
        if not isinstance(acceptance, Mapping):
            if marked and (await self._ports.component_snapshot(component_id)).get(
                "active_target_retired_by_incident"
            ) is not True:
                return False
            rejection = result.get("rejection")
            raise _failure(
                "component_rearm_refused",
                component_id,
                f"CCLC refused the re-arm {intent}",
                step=intent,
                rejection_code=rejection.get("code") if isinstance(rejection, Mapping) else None,
            )
        if not wait:
            return None
        transaction_id = acceptance["lifecycle_transaction_id"]
        allowance = self._allowance_seconds(acceptance)
        deadline = self._ports.monotonic() + allowance + REARM_STEP_GRACE_SECONDS
        while self._ports.monotonic() < deadline:
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
                    vendor_status=details.get("vendor_status"),
                    ros2_control_status=details.get("ros2_control_status"),
                )
            await self._ports.sleep(REARM_POLL_SECONDS)
        raise _failure(
            "component_rearm_timeout",
            component_id,
            f"the re-arm {intent} did not finish",
            step=intent,
            allowance_s=allowance,
        )

async def reenable_components(
    ports: ComponentRearmPorts,
    component_ids: list[str],
    *,
    request_id: str,
    owner_epoch: str,
    moved: frozenset[str] | set[str] = frozenset(),
    acknowledge_incident: bool = True,
    aborted: Callable[[], bool] = lambda: False,
) -> list[str]:
    """Recover cell: Recover, Prepare and Enable each listed component.

    A component in ``moved`` (moved from its own interface; its Active target
    is kept) gets only the operator's Recover: the target driver then
    re-drives Prepare and Enable, and the caller waits for it to be active.
    Every request names ``owner_epoch``, the press's CCLC owner epoch. With
    ``acknowledge_incident`` false (item 36) the Recover acknowledges no
    incident; an open one refuses it ``cclc_incident_open``.

    SX-666/673: every listed component that is not ``moved`` was selected by
    CCLC's incident re-enable mark, so each step requires it in the snapshot
    its request is fenced on (``run_intent``); once the mark is gone (the
    operator's newer target, Close, or Active reached) its re-enable stops
    there, cleanly, and it is not listed. Returns the components enabled again.

    SX-679/688/689: after the operator's ``aborted`` no further request is
    sent (``RearmAborted``); the one in flight settles within its own limit.
    Any error leaving here carries ``rearm_progress``: the components enabled
    again and the steps done per component.
    """

    rearm = _Rearm(ports, request_id, owner_epoch, acknowledge_incident, aborted)
    reenabled: list[str] = []
    steps: dict[str, list[str]] = {}
    try:
        for component_id in component_ids:
            marked = component_id not in moved
            intents = ("recover", "prepare", "enable") if marked else ("recover",)
            for intent in intents:
                if await rearm.run_intent(component_id, intent, marked=marked) is False:
                    break
                steps.setdefault(component_id, []).append(intent)
            else:
                reenabled.append(component_id)
    except Exception as error:
        error.rearm_progress = (list(reenabled), {k: list(v) for k, v in steps.items()})  # type: ignore[attr-defined]
        raise
    return reenabled
