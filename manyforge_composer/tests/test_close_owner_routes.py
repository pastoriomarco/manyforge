import json

from fastapi.testclient import TestClient
import httpx
import pytest

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessHttpClient
from manyforge_headless_api.tests.test_close_wire import _intent, _result, _selection


def test_composer_passively_proxies_exact_retirement_owner_commands():
    calls = []; intent = _intent()
    def handler(request):
        body = json.loads(request.content) if request.content else None
        calls.append((request.method, request.url.path, body))
        path = request.url.path
        value = _selection() if path.endswith('canonical-empty-selection') else (
            {"scope": intent["scope"], "targets": intent["expected_targets"]} if path.endswith('retirement-offer') else
            {"result": None} if path.endswith('/result') else {"scope": intent["scope"], "settled": True} if path.endswith('settlement') else _result(intent))
        return httpx.Response(200, json=value)
    with httpx.Client(base_url="http://headless.test", transport=httpx.MockTransport(handler)) as raw:
        headless = HeadlessHttpClient("http://unused.test", client=raw)
        client = TestClient(create_app(ComposerState(headless_client=headless)))
        assert client.get('/api/manager/canonical-empty-selection').json() == _selection()
        assert client.get('/api/cclc/targets/retirement-offer').status_code == 200
        bad = {**intent, "confirmed": True}
        assert client.post('/api/cclc/targets/retire', json=bad).status_code == 422
        assert client.post('/api/cclc/targets/retire', json=intent).json() == _result(intent)
        assert client.post('/api/cclc/targets/retire/result', json=intent).json() == {"result": None}
        assert client.post('/api/cclc/targets/settlement', json={"scope": intent["scope"]}).json()['settled'] is True
    assert calls == [('GET', '/api/manager/canonical-empty-selection', None), ('GET', '/api/cclc/targets/retirement-offer', None),
                     ('POST', '/api/cclc/targets/retire', intent), ('POST', '/api/cclc/targets/retire/result', intent),
                     ('POST', '/api/cclc/targets/settlement', {"scope": intent["scope"]})]


@pytest.mark.parametrize("correlation", [None, "absent"])
def test_no_correlation_stop_through_composer_headless_and_manager(tmp_path, monkeypatch, correlation):
    from manyforge_headless_api.app import create_app as create_headless_app
    from manyforge_headless_api.manager import ManagerCommandClient
    from manyforge_headless_api.service import HeadlessService
    from manyforge_headless_api.tests.test_manager import _process_stop_request, _process_stop_response
    from manyforge_headless_api.models import ComponentProcessStopRequest
    body = {"request": _process_stop_request().model_dump(mode="json")}
    if correlation == "absent": body["request"]["request_context"].pop("correlation_id")
    else: body["request"]["request_context"]["correlation_id"] = None
    canonical = ComponentProcessStopRequest.model_validate(body["request"])
    manager = ManagerCommandClient(tmp_path); commands = []
    def exchange(command):
        assert set(command["request"]["request_context"]) == {"request_id", "request_owner_epoch", "source_label"}
        commands.append(command)
        return _process_stop_response(canonical)
    monkeypatch.setattr(manager, "_exchange", exchange)
    service = HeadlessService(object(), manager)
    with TestClient(create_headless_app(service=service)) as headless_api:
        def handler(request):
            response = headless_api.post(request.url.path, json=json.loads(request.content))
            return httpx.Response(response.status_code, json=response.json())
        with httpx.Client(base_url="http://headless.test", transport=httpx.MockTransport(handler)) as raw:
            headless = HeadlessHttpClient("http://unused.test", client=raw)
            with TestClient(create_app(ComposerState(headless_client=headless))) as composer:
                for suffix in ("", "/result"):
                    response = composer.post('/api/cclc/components/arm/process/stop' + suffix, json=body)
                    assert response.status_code == 200
                    assert response.json()["request"] == canonical.model_dump(mode="json")
    assert [command["command"] for command in commands] == ["stop_component_process", "component_process_stop_result"]


def test_stop_offer_is_a_passive_thin_get_and_unknown_is_not_empty(tmp_path, monkeypatch):
    from manyforge_headless_api.app import create_app as create_headless_app
    from manyforge_headless_api.manager import ManagerCommandClient
    from manyforge_headless_api.service import HeadlessService
    manager = ManagerCommandClient(tmp_path); commands = []
    offer = {"controllable_component_id": "arm", "process_inventories": []}
    def exchange(command):
        commands.append(command)
        return {"api_version": "manyforge.manager_command_response.v1", "ok": True, "error": None, "result": offer}
    monkeypatch.setattr(manager, "_exchange", exchange)
    with TestClient(create_headless_app(service=HeadlessService(object(), manager))) as headless_api:
        def handler(request):
            response = headless_api.get(request.url.path)
            return httpx.Response(response.status_code, json=response.json())
        with httpx.Client(base_url="http://headless.test", transport=httpx.MockTransport(handler)) as raw:
            headless = HeadlessHttpClient("http://unused.test", client=raw)
            with TestClient(create_app(ComposerState(headless_client=headless))) as composer:
                response = composer.get('/api/cclc/components/arm/process/stop-offer')
                assert response.status_code == 200 and response.json() == offer
                monkeypatch.setattr(manager, "_exchange", lambda command: {"api_version": "manyforge.manager_command_response.v1",
                    "ok": False, "result": None, "error": {"code": "process_inventory_unknown", "message": "Prior host unavailable", "details": {}}})
                response = composer.get('/api/cclc/components/arm/process/stop-offer')
                assert response.status_code != 200
                assert response.json()['detail']['code'] == 'process_inventory_unknown'
    assert commands == [{"api_version": "manyforge.manager_command.v1", "command": "component_process_stop_offer", "controllable_component_id": "arm"}]


def test_empty_preparation_preview_and_apply_are_thin_existing_owner_proxies():
    from manyforge_headless_api.tests.test_manager import _request
    selection = _selection()
    prepare = {"origin": {"kind": "sealed_canonical_empty", "cell_id": selection["cell_id"],
        "deployment_artifact_id": selection["deployment_artifact_id"], "deployment_revision": selection["deployment_revision"],
        "retained_bundle_id": selection["candidate_retained_bundle_id"], "graph_revision": selection["candidate_graph_revision"],
        "admission_digest": selection["admission_digest"]}, "request_owner_epoch": selection["request_owner_epoch"], "expected_admission_generation": 7}
    apply = _request().model_dump(mode='json')
    receipt = apply['candidate_receipt']
    preview = {key: receipt[key] for key in ['deployment_artifact_id', 'deployment_revision', 'prepared_activation_id',
        'candidate_retained_bundle_id', 'candidate_graph_revision', 'expected_current', 'expected_admission_generation']}
    preview['request_owner_epoch'] = receipt['manager_owner_epoch']
    calls = []
    def handler(request):
        calls.append((request.url.path, json.loads(request.content)))
        return httpx.Response(200, json={"actual_owner_reply": request.url.path})
    with httpx.Client(base_url='http://headless.test', transport=httpx.MockTransport(handler)) as raw:
        with TestClient(create_app(ComposerState(headless_client=HeadlessHttpClient('http://unused.test', client=raw)))) as composer:
            # Application startup has its own passive recovery read. Scope this assertion to the three commands.
            calls.clear()
            for path, body in [('/api/activation-preparations', prepare), ('/api/manager/deployment-candidates/preview', preview), ('/api/manager/apply', apply)]:
                response = composer.post(path, json=body)
                assert response.status_code == 200
                assert response.json() == {"actual_owner_reply": path}
    assert [path for path, _body in calls] == ['/api/activation-preparations', '/api/manager/deployment-candidates/preview', '/api/manager/apply']
    assert calls[0][1] == prepare and calls[1][1] == preview and calls[2][1] == apply
