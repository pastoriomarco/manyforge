from __future__ import annotations

from typing import Any

import pytest

from manyforge_assistant_bridge.headless import (
    ASSISTANT_H0_READ_OPERATIONS,
    AssistantHeadlessReadClient,
    AssistantHeadlessReadError,
)


class RecordingReader:
    def __init__(self) -> None:
        self.calls: list[tuple[str, object]] = []

    def invoke(self, operation: str, arguments=None) -> dict[str, Any]:
        self.calls.append((operation, arguments))
        return {"schema_version": "manyforge.headless_runtime_snapshot.v1"}


def test_assistant_reads_owner_facts_without_callback_or_mutation() -> None:
    reader = RecordingReader()
    client = AssistantHeadlessReadClient(reader)

    assert client.observe("runtime")["schema_version"].endswith("snapshot.v1")
    assert reader.calls == [("runtime", None)]
    assert "node_environment" in ASSISTANT_H0_READ_OPERATIONS
    assert "source_model" in ASSISTANT_H0_READ_OPERATIONS


@pytest.mark.parametrize(
    "operation",
    ["manager_apply", "acknowledge_alarm", "run_program", "mutate_scene"],
)
def test_assistant_rejects_later_gate_operations_before_transport(
    operation: str,
) -> None:
    reader = RecordingReader()
    client = AssistantHeadlessReadClient(reader)

    with pytest.raises(AssistantHeadlessReadError, match="unavailable at H0"):
        client.observe(operation, {})

    assert reader.calls == []
