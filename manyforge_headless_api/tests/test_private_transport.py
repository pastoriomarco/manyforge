from __future__ import annotations

import os
from pathlib import Path
import socket
import stat

import pytest

from manyforge_headless_api.private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)


def test_socket_address_keeps_the_validated_run_root_pinned(tmp_path: Path) -> None:
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    endpoint = run_root / "command.sock"
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(endpoint))
    endpoint.chmod(0o600)
    capability = PinnedRunRoot(run_root)

    try:
        with capability.socket_address(endpoint.name) as address:
            metadata = os.stat(address, follow_symlinks=False)
            assert stat.S_ISSOCK(metadata.st_mode)
            assert address.startswith("/proc/self/fd/")
    finally:
        listener.close()


def test_socket_address_rejects_unsafe_endpoint_mode(tmp_path: Path) -> None:
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    endpoint = run_root / "command.sock"
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(endpoint))
    endpoint.chmod(0o660)

    try:
        with pytest.raises(
            PrivateTransportCapabilityError,
            match="exact same-user socket",
        ):
            with PinnedRunRoot(run_root).socket_address(endpoint.name):
                pass
    finally:
        listener.close()


def test_socket_address_rejects_a_replaced_run_root(tmp_path: Path) -> None:
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    capability = PinnedRunRoot(run_root)
    run_root.rename(tmp_path / "retained-run")
    run_root.mkdir(mode=0o700)

    with pytest.raises(
        PrivateTransportCapabilityError,
        match="unsafe or was replaced",
    ):
        with capability.socket_address("command.sock"):
            pass


def test_socket_address_preserves_transport_exceptions(tmp_path: Path) -> None:
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    endpoint = run_root / "command.sock"
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(endpoint))
    endpoint.chmod(0o600)

    try:
        with pytest.raises(TimeoutError, match="transport timeout"):
            with PinnedRunRoot(run_root).socket_address(endpoint.name):
                raise TimeoutError("transport timeout")
    finally:
        listener.close()


def test_run_root_rejects_noncanonical_or_unsafe_capabilities(
    tmp_path: Path,
) -> None:
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    alias = tmp_path / "alias"
    alias.symlink_to(run_root, target_is_directory=True)

    with pytest.raises(PrivateTransportCapabilityError):
        PinnedRunRoot(alias)

    run_root.chmod(0o720)
    with pytest.raises(PrivateTransportCapabilityError):
        PinnedRunRoot(run_root)
