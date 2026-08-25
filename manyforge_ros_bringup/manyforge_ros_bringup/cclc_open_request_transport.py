"""Bounded ROS-to-CCLC private transport with no lifecycle authority."""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
import os
from pathlib import Path
import socket
import stat
from typing import Iterator


CCLC_ROS_SOCKET_FILE = "cclc-ros-command.sock"
MAX_CCLC_REQUEST_BYTES = 4 * 1024 * 1024
MAX_CCLC_RESPONSE_BYTES = 8 * 1024 * 1024
CCLC_TRANSPORT_TIMEOUT_S = 2.0


class CCLCTransportError(RuntimeError):
    """Stable fail-closed error from the private adapter transport."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class CCLCOpenAdapterResult:
    completed: bool
    canonical_response_json: str
    error_code: str
    error_message: str


class CCLCOpenUnixClient:
    """One-request client for the CCLC owner's ROS-attributed endpoint."""

    def __init__(self, trusted_run_root: Path) -> None:
        self._root = Path(trusted_run_root)
        self._socket_path = self._root / CCLC_ROS_SOCKET_FILE
        with self._open_root(expected_identity=None) as (_descriptor, root):
            self._root_identity = (root.st_dev, root.st_ino)
        if len(os.fsencode(self._socket_path)) >= 104:
            raise CCLCTransportError(
                "cclc_ros_transport_invalid",
                "CCLC ROS transport socket path exceeds the AF_UNIX bound",
            )

    @contextmanager
    def _open_root(
        self,
        *,
        expected_identity: tuple[int, int] | None,
    ) -> Iterator[tuple[int, os.stat_result]]:
        try:
            before = self._root.lstat()
            canonical = self._root.resolve(strict=True)
            descriptor = os.open(
                self._root,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            )
        except (OSError, RuntimeError) as exc:
            raise CCLCTransportError(
                "cclc_ros_transport_invalid",
                "CCLC run root is unavailable or is not one safe "
                "pre-existing real directory",
            ) from exc
        try:
            try:
                opened = os.fstat(descriptor)
                after = self._root.lstat()
            except OSError as exc:
                raise CCLCTransportError(
                    "cclc_ros_transport_invalid",
                    "CCLC run root changed while opening",
                ) from exc
            identity = (before.st_dev, before.st_ino)
            required = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
            if (
                expected_identity is not None
                and identity != expected_identity
            ):
                raise CCLCTransportError(
                    "cclc_ros_transport_invalid",
                    "CCLC run root identity changed",
                )
            if (
                not self._root.is_absolute()
                or canonical != self._root
                or any(
                    not stat.S_ISDIR(metadata.st_mode)
                    or metadata.st_uid != os.geteuid()
                    or metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                    or metadata.st_mode & required != required
                    or (metadata.st_dev, metadata.st_ino) != identity
                    for metadata in (before, opened, after)
                )
            ):
                raise CCLCTransportError(
                    "cclc_ros_transport_invalid",
                    "CCLC run root must be one safe pre-existing real directory",
                )
            yield descriptor, opened
            try:
                final = self._root.lstat()
            except OSError as exc:
                raise CCLCTransportError(
                    "cclc_ros_transport_invalid",
                    "CCLC run root changed during use",
                ) from exc
            if (
                not stat.S_ISDIR(final.st_mode)
                or final.st_uid != os.geteuid()
                or final.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                or final.st_mode & required != required
                or (final.st_dev, final.st_ino) != identity
            ):
                raise CCLCTransportError(
                    "cclc_ros_transport_invalid",
                    "CCLC run root changed during use",
                )
        finally:
            os.close(descriptor)

    @contextmanager
    def _socket_address(self) -> Iterator[str]:
        with self._open_root(
            expected_identity=self._root_identity
        ) as (descriptor, _root):
            try:
                status = os.stat(
                    CCLC_ROS_SOCKET_FILE,
                    dir_fd=descriptor,
                    follow_symlinks=False,
                )
            except OSError as exc:
                raise CCLCTransportError(
                    "cclc_ros_transport_unavailable",
                    "CCLC ROS owner endpoint is unavailable",
                ) from exc
            if (
                not stat.S_ISSOCK(status.st_mode)
                or status.st_uid != os.geteuid()
                or status.st_nlink != 1
                or stat.S_IMODE(status.st_mode) != 0o600
            ):
                raise CCLCTransportError(
                    "cclc_ros_transport_invalid",
                    "CCLC ROS owner endpoint is not the exact same-user socket",
                )
            yield f"/proc/self/fd/{descriptor}/{CCLC_ROS_SOCKET_FILE}"

    def exchange(self, canonical_request_json: str) -> str:
        if not isinstance(canonical_request_json, str):
            raise CCLCTransportError(
                "cclc_ros_request_invalid",
                "CCLC ROS request must be UTF-8 text",
            )
        request = canonical_request_json.encode("utf-8")
        if not request or len(request) > MAX_CCLC_REQUEST_BYTES:
            raise CCLCTransportError(
                "cclc_ros_request_invalid",
                "CCLC ROS request is empty or exceeds its byte bound",
            )
        try:
            with self._socket_address() as socket_address:
                with socket.socket(
                    socket.AF_UNIX, socket.SOCK_SEQPACKET
                ) as channel:
                    channel.settimeout(CCLC_TRANSPORT_TIMEOUT_S)
                    channel.connect(socket_address)
                    sent = channel.send(request)
                    if sent != len(request):
                        raise OSError("short CCLC ROS request send")
                    response, _ancillary, flags, _address = channel.recvmsg(
                        MAX_CCLC_RESPONSE_BYTES + 1
                    )
        except OSError as exc:
            raise CCLCTransportError(
                "cclc_ros_transport_unavailable",
                "CCLC ROS owner exchange failed",
            ) from exc
        if (
            not response
            or len(response) > MAX_CCLC_RESPONSE_BYTES
            or flags & socket.MSG_TRUNC
        ):
            raise CCLCTransportError(
                "cclc_ros_response_invalid",
                "CCLC ROS owner response is empty, truncated, or oversized",
            )
        try:
            return response.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise CCLCTransportError(
                "cclc_ros_response_invalid",
                "CCLC ROS owner response is not UTF-8",
            ) from exc


def handle_open_request(
    client: CCLCOpenUnixClient,
    canonical_request_json: str,
) -> CCLCOpenAdapterResult:
    """Translate only mechanical transport success into the ROS response."""

    try:
        response = client.exchange(canonical_request_json)
    except CCLCTransportError as exc:
        return CCLCOpenAdapterResult(False, "", exc.code, str(exc))
    return CCLCOpenAdapterResult(True, response, "", "")
