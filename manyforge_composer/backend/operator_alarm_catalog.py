"""Permanent compatibility fence for the relocated operator-alarm catalog.

The removed Composer loader implemented ``manyforge.operator_alarm_catalog.v1``
and identified its YAML by raw file bytes.  The approved catalog contract uses
canonical JSON-compatible data and an RFC 8785/JCS ``descriptor_digest``.  A
new loader therefore belongs to the headless alarm owner; retaining or adapting
the old loader here would create a second, incompatible source of truth.
"""

from __future__ import annotations

from typing import Final, NoReturn


OPERATOR_ALARM_CATALOG_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_operator_alarm_catalog_unavailable"
)
OPERATOR_ALARM_CATALOG_REPLACEMENT_GATE: Final = "I5/H0"


class OperatorAlarmCatalogUnavailableError(RuntimeError):
    """Raised before a path is opened or a catalog document is interpreted."""


def load_operator_alarm_catalog(_source: object = None) -> NoReturn:
    """Reject the obsolete Composer loader before inspecting its source."""
    raise OperatorAlarmCatalogUnavailableError(
        f"{OPERATOR_ALARM_CATALOG_UNAVAILABLE_REASON_CODE}: the Composer "
        "loader was replaced at I5/H0 by the manyforge_headless JCS catalog"
    )
