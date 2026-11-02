"""Unavailable production-binding coexistence verifier.

The historical verifier imported the old production RoboPlan module and
required create_native_kernel and NativeKernelBundle. Those symbols are
deliberately removed and must not become compatibility requirements.

The exact verifier remains available at
manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
8ff8eb61bc49086cefb0715f308232a1151d89ce. Its two-order module coexistence,
strong-ID, source-identity, and clean-shutdown requirements return in the
atomic I2 Python/compiler binding qualification. The deleted factory and
bundle assertions do not return.
"""

from __future__ import annotations

import sys
from collections.abc import Sequence


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATE = "I2"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: production binding coexistence requires the "
    f"atomic {RESTORATION_GATE} Python/compiler cutover"
)


def main(argv: Sequence[str] | None = None) -> int:
    """Fail before parsing paths or importing either production module."""

    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
