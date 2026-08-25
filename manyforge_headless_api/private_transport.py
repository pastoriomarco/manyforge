"""Shared validation for read-only private run-root transport clients."""

from __future__ import annotations

from contextlib import contextmanager
import os
from pathlib import Path
import stat
from typing import Iterator


class PrivateTransportCapabilityError(RuntimeError):
    """A private run-root or endpoint is absent, unsafe, or replaced."""


class PinnedRunRoot:
    """Pin one existing run root and validate fixed same-user socket leaves."""

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        with self._open(expected_identity=None) as (_descriptor, identity):
            self._identity = identity

    @staticmethod
    def _safe_directory(metadata: os.stat_result) -> bool:
        required = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
        return (
            stat.S_ISDIR(metadata.st_mode)
            and metadata.st_uid == os.geteuid()
            and not metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            and metadata.st_mode & required == required
        )

    @contextmanager
    def _open(
        self,
        *,
        expected_identity: tuple[int, int] | None,
    ) -> Iterator[tuple[int, tuple[int, int]]]:
        try:
            before = self.path.lstat()
            canonical = self.path.resolve(strict=True)
            descriptor = os.open(
                self.path,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            )
        except (OSError, RuntimeError) as error:
            raise PrivateTransportCapabilityError(
                f"private run root is unavailable: {error}"
            ) from error
        try:
            try:
                opened = os.fstat(descriptor)
                after = self.path.lstat()
            except OSError as error:
                raise PrivateTransportCapabilityError(
                    f"private run root changed while opening: {error}"
                ) from error
            identity = (before.st_dev, before.st_ino)
            if (
                not self.path.is_absolute()
                or canonical != self.path
                or any(
                    not self._safe_directory(metadata)
                    or (metadata.st_dev, metadata.st_ino) != identity
                    for metadata in (before, opened, after)
                )
                or (
                    expected_identity is not None
                    and identity != expected_identity
                )
            ):
                raise PrivateTransportCapabilityError(
                    "private run root is unsafe or was replaced"
                )
            # Exceptions raised by the transport user must retain their own
            # type.  Only validation performed here is translated into a
            # capability failure.
            yield descriptor, identity
            try:
                final = self.path.lstat()
            except OSError as error:
                raise PrivateTransportCapabilityError(
                    f"private run root changed during use: {error}"
                ) from error
            if (
                not self._safe_directory(final)
                or (final.st_dev, final.st_ino) != identity
            ):
                raise PrivateTransportCapabilityError(
                    "private run root changed during use"
                )
        finally:
            os.close(descriptor)

    @contextmanager
    def socket_address(self, filename: str) -> Iterator[str]:
        """Yield one fixed socket through the still-pinned run directory."""

        if (
            not filename
            or filename in {".", ".."}
            or "/" in filename
            or "\\" in filename
        ):
            raise PrivateTransportCapabilityError(
                "private socket filename is invalid"
            )
        with self._open(expected_identity=self._identity) as (descriptor, _identity):
            try:
                metadata = os.stat(
                    filename,
                    dir_fd=descriptor,
                    follow_symlinks=False,
                )
            except OSError as error:
                raise PrivateTransportCapabilityError(
                    f"private socket is unavailable: {error}"
                ) from error
            if (
                not stat.S_ISSOCK(metadata.st_mode)
                or metadata.st_uid != os.geteuid()
                or metadata.st_nlink != 1
                or stat.S_IMODE(metadata.st_mode) != 0o600
            ):
                raise PrivateTransportCapabilityError(
                    "private endpoint is not the exact same-user socket"
                )
            yield f"/proc/self/fd/{descriptor}/{filename}"


__all__ = ["PinnedRunRoot", "PrivateTransportCapabilityError"]
