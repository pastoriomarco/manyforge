"""The arm's controlled-Stop fallback sequence (one place for the contract).

Modes are ordered by certainty.  The authored ``stop_mode`` is the first step;
``fallback_stop_modes`` lists, in this fixed order and without duplicates,
modes strictly below it that Core tries when the previous step cannot be
carried out.  ``hard_deceleration`` is always the final step (the existing
software_hard escalation stage), so it is never listed.  Absent means ``[]``:
primary, then hard.
"""

from __future__ import annotations

from typing import Any, Mapping

FALLBACK_STOP_MODES_FIELD = "fallback_stop_modes"
# Fixed order by certainty; hard_deceleration closes every sequence.
STOP_MODE_ORDER: tuple[str, ...] = (
    "soft_on_path",
    "spring_back_hold",
    "spring_back_fast",
    "hard_deceleration",
)
FINAL_STOP_MODE = "hard_deceleration"


def fallback_candidates(primary: str) -> tuple[str, ...]:
    """Modes that may follow ``primary`` as fallbacks, in the fixed order."""

    if primary not in STOP_MODE_ORDER:
        return ()
    index = STOP_MODE_ORDER.index(primary)
    return tuple(
        mode for mode in STOP_MODE_ORDER[index + 1 :] if mode != FINAL_STOP_MODE
    )


def fallback_sequence_error(primary: str, value: Any) -> str | None:
    """Return why ``value`` is not a valid fallback list for ``primary``."""

    if type(value) is not list:
        return "must be a list of Stop modes"
    candidates = fallback_candidates(primary)
    previous = -1
    seen: set[str] = set()
    for mode in value:
        if type(mode) is not str or mode not in STOP_MODE_ORDER:
            return f"contains unknown Stop mode {mode!r}"
        if mode == FINAL_STOP_MODE:
            return (
                "must not list hard_deceleration; it is always the final step"
            )
        if mode in seen:
            return f"contains duplicate Stop mode {mode!r}"
        if mode not in candidates:
            return (
                f"may only list modes below the Stop mode {primary!r} "
                f"({', '.join(candidates) or 'none'}); {mode!r} is not"
            )
        index = STOP_MODE_ORDER.index(mode)
        if index < previous:
            return (
                "must follow the fixed order "
                + " -> ".join(STOP_MODE_ORDER[:-1])
            )
        previous = index
        seen.add(mode)
    return None


LEGACY_SUBSTITUTION_FIELD = "recoverable_substitution_mode"


def recorded_fallback_stop_modes(parameters: Mapping[str, Any] | None) -> list[str]:
    """Read one compiled ``default_contract_parameters`` fallback sequence.

    Canonical documents carry ``fallback_stop_modes``.  A compiled v1 / legacy
    ``recoverable_substitution_mode: spring_back_hold`` alone (soft_on_path
    only) reads as ``[spring_back_hold]``.  Carrying both is ambiguous and
    raises ``ValueError``.  Writers emit only the new field (non-empty).
    """

    if not isinstance(parameters, Mapping):
        return []
    has_new = FALLBACK_STOP_MODES_FIELD in parameters
    has_legacy = LEGACY_SUBSTITUTION_FIELD in parameters
    if has_new and has_legacy:
        raise ValueError(
            f"Controlled Stop carries both {FALLBACK_STOP_MODES_FIELD} and the "
            f"older {LEGACY_SUBSTITUTION_FIELD}; keep only {FALLBACK_STOP_MODES_FIELD}"
        )
    if has_new:
        value = parameters[FALLBACK_STOP_MODES_FIELD]
        return list(value) if type(value) is list else []
    if (
        parameters.get("stop_mode") == "soft_on_path"
        and parameters.get(LEGACY_SUBSTITUTION_FIELD) == "spring_back_hold"
    ):
        return ["spring_back_hold"]
    return []
