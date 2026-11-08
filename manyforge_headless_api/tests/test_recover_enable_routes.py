from fastapi.testclient import TestClient

from manyforge_headless_api.app import create_app
from manyforge_headless_api.service import HeadlessService
from types import SimpleNamespace
from threading import Lock
import pytest
from manyforge_headless_api.native import HeadlessApiError


def test_recovery_routes_forward_confirmation_and_reject_undisclosed_fields():
    class Service:
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
    response = client.post("/api/components/arm/recover-enable", json=confirmation)
    assert response.status_code == 202
    assert response.json() == {"action": "enable"}
    assert service.received == ("arm", confirmation)
    invalid = dict(confirmation, undisclosed=True)
    assert client.post("/api/components/arm/recover-enable", json=invalid).status_code == 422
    targets = [{"component_id": "arm", "recovery_preflight": confirmation}]
    assert client.post("/api/components/recover-enable", json={"targets": targets}).status_code == 202
    assert service.targets == targets
    assert client.post("/api/components/recover-enable", json={"targets": targets * 2}).status_code == 422


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
    service._stop_generation = lambda: 0
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
