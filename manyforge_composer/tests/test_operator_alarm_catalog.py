"""Packaged operator-alarm catalog schema and immutability tests."""
from __future__ import annotations

from pathlib import Path

import pytest
import yaml

from manyforge_composer.backend.operator_alarm_catalog import (
    OPERATOR_ALARM_CATALOG_SCHEMA_VERSION,
    OperatorAlarmCatalogError,
    default_operator_alarm_catalog_path,
    load_operator_alarm_catalog,
)


def _document() -> dict:
    return yaml.safe_load(default_operator_alarm_catalog_path().read_text("utf-8"))


def _write(tmp_path: Path, document: dict) -> Path:
    path = tmp_path / "catalog.yaml"
    path.write_text(yaml.safe_dump(document, sort_keys=False), encoding="utf-8")
    return path


def test_packaged_catalog_loads_five_immutable_definitions() -> None:
    catalog = load_operator_alarm_catalog()

    assert catalog.schema_version == OPERATOR_ALARM_CATALOG_SCHEMA_VERSION
    assert catalog.catalog_version == "1.0.0"
    assert len(catalog.definitions) == 5
    assert len(catalog.sha256) == 64
    assert catalog.definition(
        "manyforge.motion.runtime_collision"
    ).priority == "critical"
    with pytest.raises(TypeError):
        catalog.definitions["new"] = catalog.definition(
            "manyforge.motion.runtime_collision"
        )


@pytest.mark.parametrize(
    ("mutation", "expected_fragment"),
    [
        (lambda doc: doc.update({"unknown": True}), "field mismatch"),
        (
            lambda doc: doc["definitions"][0].update({"priority": "ERROR"}),
            "priority is unsupported",
        ),
        (
            lambda doc: doc["definitions"][0].update(
                {"message_template": "Bad {not_in_scope}"}
            ),
            "non-scope fields",
        ),
        (
            lambda doc: doc["definitions"][0].update({"max_evidence_refs": 33}),
            "must be in",
        ),
        (
            lambda doc: doc["definitions"].append(dict(doc["definitions"][0])),
            "Duplicate definition_id",
        ),
    ],
)
def test_invalid_catalog_is_rejected(
    tmp_path: Path,
    mutation,
    expected_fragment: str,
) -> None:
    document = _document()
    mutation(document)

    with pytest.raises(OperatorAlarmCatalogError, match=expected_fragment):
        load_operator_alarm_catalog(_write(tmp_path, document))


def test_missing_catalog_has_stable_unavailable_error(tmp_path: Path) -> None:
    with pytest.raises(OperatorAlarmCatalogError) as error:
        load_operator_alarm_catalog(tmp_path / "missing.yaml")
    assert error.value.code == "operator_alarm_catalog_unavailable"
