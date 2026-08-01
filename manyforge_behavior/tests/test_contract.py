"""The old singleton Python Kernel contract is unavailable until I6/N."""

import pytest

from manyforge_behavior.kernel_client import (
    KernelClient,
    KernelClientBindingUnavailableError,
)


def test_contract_fails_before_old_execute_or_stop_contract_is_exposed() -> None:
    class PoisonBinding:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"binding was accessed: {name}")

    with pytest.raises(
        KernelClientBindingUnavailableError,
        match="composable_cell_kernel_client_binding_unavailable",
    ):
        KernelClient(PoisonBinding())
