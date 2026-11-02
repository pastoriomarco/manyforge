"""Scene commits and status return owner receipts without an execution bridge."""
from types import SimpleNamespace

import pytest

from manyforge_headless_api.service import HeadlessService, TrustedAttribution


REVISION = {"scene_id": "scene", "revision_id": 7, "content_hash": "hash"}
COMMITTED = {"ok": True, "projection_status": "committed",
             "committed_projection_revision": {"scene_revision": REVISION}}


class Manager:
    def __init__(self):
        self.inside = False
        self.result = COMMITTED

    def commit_scene_transaction(self, request, *, trusted_principal_id):
        self.inside = True
        try:
            return self.result
        finally:
            self.inside = False

    def scene_transaction_status(self, request):
        self.inside = True
        try:
            return {"ok": True, "result": self.result}
        finally:
            self.inside = False


class Execution:
    def __init__(self, manager):
        self.manager = manager
        self.calls = []
        self.fail = False

    def apply_tool_payload(self, revision):
        assert not self.manager.inside
        self.calls.append(revision)
        if self.fail:
            raise RuntimeError("payload failed")


def request(kind):
    operation = SimpleNamespace(kind=kind)
    return SimpleNamespace(operation=operation, program_node_evidence=None,
                           expected_admission_generation=None,
                           program_reset_scene_authorization=None)


@pytest.mark.parametrize("kind", ["attach_carried_object", "detach_carried_object", "detach_carried_object_at_current_pose"])
@pytest.mark.parametrize("program_node", [False, True])
def test_committed_scene_and_exact_receipt_do_not_call_execution_bridge(kind, program_node):
    manager = Manager()
    execution = Execution(manager)
    service = HeadlessService(object(), manager, execution)
    item = request(kind)
    if program_node:
        item.program_node_evidence = object()
        item.expected_admission_generation = 1
    assert service.scene_commit(item, TrustedAttribution()) is COMMITTED
    assert service.scene_transaction_status(item)["result"] is COMMITTED
    assert execution.calls == []


def test_commit_does_not_use_the_execution_owner():
    manager = Manager()
    execution = Execution(manager)
    execution.fail = True
    service = HeadlessService(object(), manager, execution)
    assert service.scene_commit(request("attach_carried_object"), TrustedAttribution()) is COMMITTED
    assert execution.calls == []


def test_uncommitted_result_does_not_notify():
    manager = Manager()
    manager.result = {"ok": False, "projection_status": "not_attempted"}
    execution = Execution(manager)
    service = HeadlessService(object(), manager, execution)
    service.scene_commit(request("attach_carried_object"), TrustedAttribution())
    assert execution.calls == []


@pytest.mark.parametrize("result", [None, {"ok": False, "projection_status": "not_attempted"}])
def test_unresolved_or_failed_receipt_does_not_notify(result):
    manager = Manager()
    manager.result = result
    execution = Execution(manager)
    service = HeadlessService(object(), manager, execution)
    response = service.scene_transaction_status(request("detach_carried_object"))
    assert response["result"] == result
    assert execution.calls == []
