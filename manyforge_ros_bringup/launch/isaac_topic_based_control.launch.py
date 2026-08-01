"""Bounded Isaac launch inputs and the composable-runtime restoration gate.

The historical launch graph is preserved at
manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
5220bb1c08c2f1d533d6ea6006da89f0acb00583. Its Composer supervisor,
singleton ROS topics, execution actions, Stop gate, and Isaac node graph are
acceptance inputs for H0, N, P5, and I; they are not a runnable fallback.

Only owner-neutral resource-profile parsers remain active here.
"""

from __future__ import annotations

from typing import NoReturn


ISAAC_RUNTIME_UNAVAILABLE_MESSAGE = (
    "composable_cell_runtime_unavailable: Isaac topic-based control is "
    "intentionally unavailable until the retained-product, headless-client, "
    "execution, Kernel Stop, and Isaac adapter restoration gates pass"
)


def generate_launch_description() -> NoReturn:
    """Fail before importing ROS or constructing a launch action."""

    _launch_setup(None)


def _launch_setup(_context, *_, **__) -> NoReturn:
    """Fail before constructing ROS nodes, timers, topics, or clients."""

    raise RuntimeError(ISAAC_RUNTIME_UNAVAILABLE_MESSAGE)


def _parse_bool_launch_value(value: str, *, name: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise ValueError(f"{name} must be true or false; got {value!r}")


def _parse_bounded_int_launch_value(
    value: str,
    *,
    name: str,
    minimum: int,
    maximum: int,
) -> int:
    try:
        parsed = int(value.strip())
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc
    if parsed < minimum or parsed > maximum:
        raise ValueError(f"{name} must be in [{minimum}, {maximum}]")
    return parsed


def _parse_cpu_affinity_launch_value(value: str, *, name: str) -> list[int]:
    # controller_manager and the native scheduler ultimately use Linux
    # cpu_set_t. Bound expansion before constructing a Python range so a
    # malformed deployment value cannot consume startup memory or CPU.
    maximum_cpu_index = 1023
    normalized = value.strip()
    if not normalized:
        return []
    cpus: set[int] = set()
    for raw_token in normalized.split(","):
        token = raw_token.strip()
        if not token:
            raise ValueError(f"{name} contains an empty CPU token")
        if "-" not in token:
            cpus.add(
                _parse_bounded_int_launch_value(
                    token,
                    name=name,
                    minimum=0,
                    maximum=maximum_cpu_index,
                )
            )
            continue
        pieces = token.split("-")
        if len(pieces) != 2:
            raise ValueError(f"{name} contains an invalid CPU range: {token!r}")
        first = _parse_bounded_int_launch_value(
            pieces[0],
            name=name,
            minimum=0,
            maximum=maximum_cpu_index,
        )
        last = _parse_bounded_int_launch_value(
            pieces[1],
            name=name,
            minimum=0,
            maximum=maximum_cpu_index,
        )
        if last < first:
            raise ValueError(f"{name} contains a descending CPU range: {token!r}")
        cpus.update(range(first, last + 1))
    return sorted(cpus)
