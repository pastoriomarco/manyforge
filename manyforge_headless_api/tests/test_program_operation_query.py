"""Program-operation query model bounds (the query is the runner's, sent through the gateway core)."""


import pytest
from pydantic import ValidationError

from manyforge_headless_api.models import ProgramOperationQuery


QUERY = {
    "schema_version": "manyforge.program_operation_query.v3",
    "program_binding_id": "sha256:" + "a" * 64,
    "program_run_id": 1,
    "target": {"component_id": "robot", "command_target_id": "arm"},
}


@pytest.mark.parametrize(
    "change",
    [
        {"program_run_id": ""},
        {"program_run_id": 0},
        {"program_run_id": "1"},
        {"target": {"component_id": "robot", "command_target_id": 1}},
        # Query v3 names no owner epoch (contract 1.16.4, rule 1).
        {"owner_epoch": "owner.1"},
        {"program_binding_id": "latest"},
        {"operation_id": "caller-selected"},
    ],
)
def test_operation_query_rejects_unbounded_or_caller_invented_scope(change):
    with pytest.raises(ValidationError):
        ProgramOperationQuery.model_validate({**QUERY, **change})


