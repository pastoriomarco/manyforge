from fastapi.testclient import TestClient

from manyforge_headless_api.app import create_app
from manyforge_headless_api.identity import RequestAdmission
from manyforge_headless_api.service import HeadlessService
from types import SimpleNamespace
from threading import Lock
import pytest
from manyforge_headless_api.native import HeadlessApiError


def test_recovery_routes_forward_confirmation_and_reject_undisclosed_fields():
    class Service:
        admission = RequestAdmission("3f9a0000000000a1")

        def component_recovery_preflight(self, component_id):
            return {"snapshot_id": "native", "components": [
                {"component_id": component_id, "cause": None, "eligibility": "ready"}
            ]}

        def component_recover_enable(self, component_id, confirmation):
            self.received = (component_id, confirmation)
            return {"action": "enable"}

        def component_recover_enable_group(self, targets):
            self.targets = targets
            return {"action": "enable"}

    service = Service()
    client = TestClient(create_app(service=service))
    assert client.get("/api/components/arm/recover-enable/preflight").json()["snapshot_id"] == "native"
    confirmation = {"snapshot_id": "native", "confirmed": [{
        "component_id": "arm", "cause_code": None, "vendor_code": None, "stop_code": None, "stop_vendor_code": None, "eligibility": "ready",
    }]}
    start = {"application_start_id": "3f9a0000000000a1"}
    response = client.post("/api/components/arm/recover-enable", json={**confirmation, **start})
    assert response.status_code == 202
    assert response.json() == {"action": "enable"}
    assert service.received == ("arm", confirmation)
    invalid = dict(confirmation, undisclosed=True, **start)
    assert client.post("/api/components/arm/recover-enable", json=invalid).status_code == 422
    targets = [{"component_id": "arm", "recovery_preflight": confirmation}]
    assert client.post("/api/components/recover-enable", json={"targets": targets, **start}).status_code == 202
    assert service.targets == targets
    assert client.post("/api/components/recover-enable", json={"targets": targets * 2, **start}).status_code == 422


@pytest.mark.parametrize(("owner_changed", "first_outcome", "bad_phases", "member_case", "expected"), [
    (True, "succeeded", False, "none", ["arm1", "arm2"]),
    (True, "succeeded", False, "cleared", ["arm1", "arm2"]),
    (True, "succeeded", False, "new", ["arm1"]),
    (True, "succeeded", False, "owner_fault", ["arm1"]),
    (False, "succeeded", False, "none", ["arm1"]),
    (False, "failed", False, "none", ["arm1"]),
    (False, "unknown", False, "none", ["arm1"]),
    (False, "succeeded", True, "none", ["arm1"]),
    (False, "succeeded", "collision", "none", ["arm1"]),
])
def test_group_rechecks_confirmation_and_stops_on_change_or_failure(monkeypatch, owner_changed, first_outcome, bad_phases, member_case, expected):
    class ImmediateThread:
        def __init__(self, *, target, **_kwargs):
            self.target = target
        def start(self):
            self.target()

    monkeypatch.setattr("manyforge_headless_api.service.Thread", ImmediateThread)
    events = []
    monkeypatch.setattr("manyforge_headless_api.service.record_activity", lambda _core, event: events.append(event))
    service = object.__new__(HeadlessService)
    service.core = object()
    service._group_action_lock = Lock()
    states = [
        {"component_id": "machine", "lifecycle": "prepared", "capabilities": []},
        {"component_id": "arm1", "owner_id": "machine", "lifecycle": "prepared", "capabilities": []},
        {"component_id": "arm2", "owner_id": "machine", "lifecycle": "prepared", "capabilities": []},
    ]
    if bad_phases == "collision":
        states[1]["last_action"] = {"action": "enable", "outcome": "succeeded", "started_at_ns": 123}
    service.component_model = lambda: SimpleNamespace(component_states=states)
    service.manager_graph = lambda: {"cell_graph": {"components": []}}
    service._last_stop_event_id = lambda: None
    confirmed_rows = [{"component_id": "machine", "cause_code": "estop", "vendor_code": "C2", "stop_code": "estop", "stop_vendor_code": "C2", "eligibility": "recover"}]
    def preflight(component_id):
        # arm1's completed owner recovery clears the owner before arm2.
        changed = owner_changed and (component_id == "arm2" or "arm1" in called)
        owner = {"component_id": "machine", "cause": None if changed else {"code": "estop", "vendor_code": "C2"},
                 "eligibility": "ready" if changed else "recover"}
        if changed and member_case == "owner_fault":
            owner.update(cause={"code": "new_owner_fault"}, eligibility="recover")
        if not changed:
            owner["stop"] = {"code": "estop", "vendor_code": "C2"}
        member_cause = {"code": "new_fault"} if component_id == "arm2" and member_case == "new" else None
        return {"snapshot_id": component_id, "components": [owner, {"component_id": component_id, "cause": member_cause, "eligibility": "recover" if member_cause else "ready"}]}
    service.component_recovery_preflight = preflight
    called = []
    def dispatch(component_id, confirmation):
        called.append(component_id)
        next(s for s in states if s["component_id"] == component_id)["last_action"] = {
            "action": "enable", "outcome": first_outcome if component_id == "arm1" else "succeeded", "started_at_ns": 123,
            "phases": ([{"component_id": "machine", "action": "recover", "outcome": "succeeded"}]
                       if confirmation["confirmed"][0]["eligibility"] == "recover" else []) +
                      [{"component_id": component_id, "action": "enable", "outcome": "succeeded"}],
        }
        if component_id == "arm1" and bad_phases is True:
            states[1]["last_action"]["phases"] = []
        return 123
    service._dispatch_recover_enable = dispatch
    targets = [{"component_id": name, "recovery_preflight": {"snapshot_id": name, "confirmed": confirmed_rows + [
        {"component_id": name, "cause_code": "old_fault" if name == "arm2" and member_case == "cleared" else None,
         "vendor_code": None, "stop_code": None, "stop_vendor_code": None,
         "eligibility": "recover" if name == "arm2" and member_case == "cleared" else "ready"}]}}
        for name in ("arm1", "arm2")]
    assert service.component_recover_enable_group(targets) == {"action": "enable"}
    assert called == expected
    if len(expected) == 1 and first_outcome == "succeeded" and bad_phases is False:
        assert events[-1]["error"]["code"] == "component_recovery_confirmation_changed"


def test_malformed_composite_acceptance_is_unknown_not_refused(monkeypatch):
    events = []
    monkeypatch.setattr("manyforge_headless_api.service.record_activity", lambda _core, event: events.append(event))
    service = object.__new__(HeadlessService)
    service.core = object()
    service.execution = SimpleNamespace(component_recover_enable=lambda *_args: {
        "ok": True, "accepted": {"action": "enable"},
    })
    with pytest.raises(HeadlessApiError):
        service.component_recover_enable("arm", {})
    assert events[-1]["outcome"] == "unknown"


def _recovering_service(monkeypatch, states, after_dispatch):
    """One confirmed target `arm1` whose composite is dispatched, then
    `after_dispatch(states)` shapes what the host shows."""

    class ImmediateThread:
        def __init__(self, *, target, **_kwargs):
            self.target = target
        def start(self):
            self.target()

    monkeypatch.setattr("manyforge_headless_api.service.Thread", ImmediateThread)
    monkeypatch.setattr("manyforge_headless_api.service.record_activity", lambda _core, _event: None)
    service = object.__new__(HeadlessService)
    service.core = object()
    service._group_action_lock = Lock()
    service.component_model = lambda: SimpleNamespace(component_states=states)
    service._last_stop_event_id = lambda: None
    row = {"component_id": "arm1", "cause_code": None, "vendor_code": None,
           "stop_code": None, "stop_vendor_code": None, "eligibility": "ready"}
    service.component_recovery_preflight = lambda component_id: {
        "snapshot_id": component_id,
        "components": [{"component_id": component_id, "cause": None, "eligibility": "ready"}]}
    called = []

    def dispatch(component_id, _confirmation):
        called.append(component_id)
        after_dispatch(service, states)
        return 123

    service._dispatch_recover_enable = dispatch
    targets = [{"component_id": "arm1", "recovery_preflight": {"snapshot_id": "arm1", "confirmed": [row]}}]
    return service, targets, called


def test_group_ends_on_the_owner_stall_fault_with_its_code(monkeypatch, caplog):
    # OD-214 (T27): no limit of its own; a composite whose owner stalled never
    # finishes, and the group ends unknown with the stall's code.
    states = [{"component_id": "arm1", "lifecycle": "prepared", "capabilities": []}]

    def stall(_service, rows):
        rows[0].update(lifecycle="fault", action={"action": "enable", "started_at_ns": 123}, fault={
            "code": "component_owner_stalled", "message": "stalled", "clearable": False,
            "operator_action": "recover", "since_ns": 1})

    service, targets, called = _recovering_service(monkeypatch, states, stall)
    with caplog.at_level("WARNING", logger="manyforge_headless_api.service"):
        assert service.component_recover_enable_group(targets) == {"action": "enable"}
    assert called == ["arm1"]
    [record] = [r for r in caplog.records if r.getMessage() == "component_recovery_outcome_unknown"]
    assert record.error_code == "component_owner_stalled"
    assert service._group_action_lock.acquire(blocking=False)  # released


def test_group_keeps_waiting_through_another_fault_until_its_last_action(monkeypatch):
    # OD-214 (T27): only the owner-stall fault ends the wait; the composite's
    # own result does otherwise.
    states = [{"component_id": "arm1", "lifecycle": "prepared", "capabilities": []}]
    reads = []

    def exited(service, rows):
        rows[0].update(lifecycle="fault", action={"action": "enable", "started_at_ns": 123}, fault={
            "code": "fault_process_exited", "message": "exited", "clearable": True,
            "operator_action": "recover", "since_ns": 1})
        model = service.component_model

        def read():
            reads.append(1)
            if len(reads) == 2:  # the host publishes the composite's end
                rows[0] = {"component_id": "arm1", "lifecycle": "enabled", "capabilities": [],
                           "last_action": {"action": "enable", "outcome": "succeeded",
                                           "started_at_ns": 123, "phases": [
                                               {"component_id": "arm1", "action": "enable",
                                                "outcome": "succeeded"}]}}
            return model()

        service.component_model = read

    service, targets, called = _recovering_service(monkeypatch, states, exited)
    assert service.component_recover_enable_group(targets) == {"action": "enable"}
    assert called == ["arm1"]
    assert len(reads) == 2
    assert service._group_action_lock.acquire(blocking=False)


def test_group_ends_when_the_host_read_fails(monkeypatch, caplog):
    # OD-214 (T27): a lost host ends the wait with the read's error.
    states = [{"component_id": "arm1", "lifecycle": "prepared", "capabilities": []}]

    def lose(service, _rows):
        def lost():
            raise HeadlessApiError("execution_owner_unavailable", "native execution transport failed")
        service.component_model = lost

    service, targets, called = _recovering_service(monkeypatch, states, lose)
    with caplog.at_level("WARNING", logger="manyforge_headless_api.service"):
        assert service.component_recover_enable_group(targets) == {"action": "enable"}
    assert called == ["arm1"]
    [record] = [r for r in caplog.records if r.getMessage() == "component_recovery_group_refused"]
    assert record.error_code == "execution_owner_unavailable"
    assert service._group_action_lock.acquire(blocking=False)
