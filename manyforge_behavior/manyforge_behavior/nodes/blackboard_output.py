"""Gate for behavior-node namespace output commits.

The former writer is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` blob
``d1aff04b69c5a86decad3a4ffc46613d21fc1ab5`` lines 12-29.  Atomic bundle
commit behavior returns only after addressed Program Binding exists; this
gate intentionally inspects neither a binding nor the proposed payload.
"""

from __future__ import annotations

from typing import Final


BLACKBOARD_OUTPUT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_blackboard_output_binding_unavailable"
)


class BlackboardOutputBindingUnavailableError(RuntimeError):
    pass


class BlackboardOutputMixin:
    """Gate until Program Binding supplies the exact output namespace."""

    def _write_namespace_output(self, *_args: object, **_kwargs: object) -> None:
        raise BlackboardOutputBindingUnavailableError(
            f"{BLACKBOARD_OUTPUT_UNAVAILABLE_REASON_CODE}: bound node outputs "
            "are not available"
        )
