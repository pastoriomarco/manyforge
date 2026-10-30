from manyforge_composer.backend.routes_headless import _translate
from manyforge_headless_api.client import HeadlessHttpClient


def test_execution_refusal_keeps_known_http_status_and_typed_details():
    message = "The committed Scene snapshot is unavailable."
    body = {"detail": {"code": "execution_rejected", "message": message,
                       "details": {"reason": "execution_port_not_ready"},
                       "producer": "execution"}}
    error = HeadlessHttpClient._error_from_response(body, 409)
    response = _translate(error)
    assert response.status_code == 409
    assert response.detail == body["detail"]
