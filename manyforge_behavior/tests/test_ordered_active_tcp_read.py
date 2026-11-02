"""The optional ordered Active TCP read is passed to the native transport."""

import pytest

from manyforge_behavior.kernel_client import KernelClient


class _Transport:
    def __init__(self):
        self.reads = []

    def identity(self): pass
    def submit(self, body): pass
    def read(self, operation_id): pass
    def reset(self, operation_id, body): pass
    def acknowledge_completed(self, operation_id, body): pass
    def stop(self, body): pass

    def read_active_tcp(self, *, ordered=False):
        self.reads.append(ordered)
        return {"ok": True, "robots": []}


def test_ordered_is_explicit_and_plain_reads_remain_ordinary():
    transport = _Transport()
    kernel = KernelClient(transport)
    assert kernel.read_active_tcp()["robots"] == []
    assert kernel.read_active_tcp(ordered=True)["robots"] == []
    assert transport.reads == [False, True]
    with pytest.raises(TypeError):
        kernel.read_active_tcp(ordered=1)
