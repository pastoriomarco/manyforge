"""The immediate-step receipt distinguishes admission from refusal."""

import pytest

from manyforge_behavior.kernel_client import (
    KernelClient, KernelClientResponseError, KernelImmediateStepFailedError,
    KernelOperationAdmissionRejectedError,
)


class _Transport:
    def __init__(self, details):
        self.details = details

    def identity(self): pass
    def read(self, operation_id): pass
    def reset(self, operation_id, body): pass
    def acknowledge_completed(self, operation_id, body): pass
    def stop(self, body): pass

    def submit(self, body):
        return {"ok": False, "error": {
            "code": "execution_rejected", "message": "failed", "details": self.details,
            "producer": "kernel",
        }}


def _intent(kind="set_tcp"):
    return {"request_context": {"request_id": "request.1"},
            "participants": [{"payload": {"kind": kind}}]}


def test_exact_immediate_receipt_is_admitted_failure():
    details = {"immediate_step": "admitted", "request_id": "request.1",
               "operation_id": "operation.1", "reason": "tcp_offset_not_applied"}
    with pytest.raises(KernelImmediateStepFailedError) as failure:
        KernelClient(_Transport(details)).execute(_intent())
    assert failure.value.code == "execution_rejected"
    assert failure.value.details == details


def test_no_receipt_remains_a_pre_admission_refusal():
    with pytest.raises(KernelOperationAdmissionRejectedError) as failure:
        KernelClient(_Transport({"reason": "set_tcp_unavailable"})).execute(_intent())
    assert failure.value.code == "execution_rejected"
    assert failure.value.details["reason"] == "set_tcp_unavailable"


@pytest.mark.parametrize("details", [
    {"immediate_step": "admitted", "request_id": "other", "operation_id": "operation.1"},
    {"immediate_step": "admitted", "request_id": "request.1"},
])
def test_bad_immediate_receipt_leaves_outcome_unknown(details):
    with pytest.raises(KernelClientResponseError):
        KernelClient(_Transport(details)).execute(_intent())


@pytest.mark.parametrize("details", [
    {"immediate_step": "unexpected", "request_id": "request.1", "operation_id": "operation.1"},
    {"request_id": "request.1", "operation_id": "operation.1"},
    {"immediate_step": "refused", "request_id": "request.1"},
])
def test_non_admitted_receipt_remains_pre_admission_refusal(details):
    with pytest.raises(KernelOperationAdmissionRejectedError) as failure:
        KernelClient(_Transport(details)).execute(_intent())
    assert failure.value.details == details


def test_other_operations_keep_existing_refusal_handling():
    with pytest.raises(KernelOperationAdmissionRejectedError):
        KernelClient(_Transport({"reason": "rejected"})).execute(_intent("motion"))


def test_exception_transport_preserves_typed_set_tcp_code_and_reason():
    class Transport(_Transport):
        def submit(self, body):
            error = RuntimeError("failed")
            error.code = "execution_rejected"
            error.details = {"reason": "tcp_tool_not_mounted"}
            error.producer = "kernel"
            raise error

    with pytest.raises(KernelOperationAdmissionRejectedError) as failure:
        KernelClient(Transport({})).execute(_intent())
    assert failure.value.code == "execution_rejected"
    assert failure.value.details["reason"] == "tcp_tool_not_mounted"
