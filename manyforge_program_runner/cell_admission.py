"""The Program runner's cell-admission calls, through the core's gateway."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

# Codes after which the owner may have acted: the outcome is unknown.
_OUTCOME_UNKNOWN_CODES = frozenset(
    {"execution_owner_unavailable", "execution_command_response_invalid",
     "execution_command_transport_failure"}
)
_RESULT_MEMBER = {
    "cell_admission_get": "authoring_admission",
    "cell_admission_acquire": "claim",
    "cell_admission_release": "authoring_admission",
    "cell_admission_recovery_receipt": "recovery_receipt",
}


class CellAdmissionError(RuntimeError):
    """A typed cell-admission refusal, or a failure after which the owner's
    outcome is unknown (``outcome_unknown``)."""

    def __init__(
        self,
        code: str,
        message: str,
        details: Mapping[str, Any] | None = None,
        *,
        outcome_unknown: bool = False,
        producer: str | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.details = dict(details or {})
        self.outcome_unknown = outcome_unknown
        self.producer = producer


class CellAdmission:
    """``get``, ``acquire``, ``release`` and ``recovery_receipt`` of the
    execution host's cell admission, over ``gateway`` (the process's one
    ``manyforge_headless_py.Gateway``). Each returns the reply's result
    member; a refusal raises ``CellAdmissionError`` with the owner's code."""

    def __init__(self, gateway: Any) -> None:
        self._gateway = gateway

    def _call(self, command: str, members: Mapping[str, Any]) -> dict[str, Any]:
        try:
            reply = getattr(self._gateway, command)(dict(members))
        except Exception as error:
            code = getattr(error, "code", None)
            if not isinstance(code, str):
                raise
            raise CellAdmissionError(
                code, str(error), outcome_unknown=code in _OUTCOME_UNKNOWN_CODES
            ) from error
        if reply.get("ok") is not True:
            error = reply.get("error")
            producer = error.get("producer", "kernel") if isinstance(error, Mapping) else None
            if (
                not isinstance(error, Mapping)
                or not isinstance(error.get("code"), str)
                or not error["code"]
                or not isinstance(error.get("message"), str)
                or not isinstance(producer, str)
                or not producer
            ):
                raise CellAdmissionError(
                    "execution_command_response_invalid",
                    "native cell-admission error is malformed",
                    outcome_unknown=True,
                )
            raise CellAdmissionError(
                error["code"], error["message"], error.get("details"), producer=producer
            )
        result = reply.get(_RESULT_MEMBER[command])
        if not isinstance(result, dict):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native cell-admission result is malformed",
                outcome_unknown=True,
            )
        return result

    def get(self) -> dict[str, Any]:
        return self._call("cell_admission_get", {})

    def acquire(
        self,
        intent: Mapping[str, Any],
        *,
        program_cycle_status: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        """A Program Reset claim carries the runner's current cycle status
        beside its intent, and only it does (OD-139). Only a claim of this
        exact intent is this acquisition's."""

        members: dict[str, Any] = {"request": dict(intent)}
        if program_cycle_status is not None:
            members["program_cycle_status"] = dict(program_cycle_status)
        claim = self._call("cell_admission_acquire", members)
        if claim.get("intent") != dict(intent):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native cell-admission claim names another intent",
                outcome_unknown=True,
            )
        return claim

    def release(self, release: Mapping[str, Any]) -> dict[str, Any]:
        return self._call("cell_admission_release", {"request": dict(release)})

    def recovery_receipt(self, query: Mapping[str, Any]) -> dict[str, Any]:
        return self._call("cell_admission_recovery_receipt", {"request": dict(query)})


__all__ = ["CellAdmission", "CellAdmissionError"]
