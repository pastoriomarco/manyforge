"""Bounded ROS-to-CCLC private transport with no lifecycle authority."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import socket
import stat


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

    def __init__(self, trusted_state_root: Path) -> None:
        self._root = Path(trusted_state_root)
        self._socket_path = self._root / CCLC_ROS_SOCKET_FILE
        self._validate_root()
        if len(os.fsencode(self._socket_path)) >= 104:
            raise CCLCTransportError(
                "cclc_ros_transport_invalid",
                "CCLC ROS transport socket path exceeds the AF_UNIX bound",
            )

    def _validate_root(self) -> None:
        try:
            status = self._root.lstat()
        except OSError as exc:
            raise CCLCTransportError(
                "cclc_ros_transport_invalid",
                "CCLC state root is unavailable",
            ) from exc
        if not stat.S_ISDIR(status.st_mode) or stat.S_ISLNK(status.st_mode):
            raise CCLCTransportError(
                "cclc_ros_transport_invalid",
                "CCLC state root must be a pre-existing real directory",
            )

    def _validate_endpoint(self) -> None:
        try:
            status = self._socket_path.lstat()
        except OSError as exc:
            raise CCLCTransportError(
                "cclc_ros_transport_unavailable",
                "CCLC ROS owner endpoint is unavailable",
            ) from exc
        if (
            not stat.S_ISSOCK(status.st_mode)
            or stat.S_ISLNK(status.st_mode)
            or status.st_uid != os.geteuid()
            or stat.S_IMODE(status.st_mode) != 0o600
        ):
            raise CCLCTransportError(
                "cclc_ros_transport_invalid",
                "CCLC ROS owner endpoint is not the exact same-user socket",
            )

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
        self._validate_endpoint()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as channel:
                channel.settimeout(CCLC_TRANSPORT_TIMEOUT_S)
                channel.connect(str(self._socket_path))
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
