"""Run Composer as a structural/Program-authoring shell and runtime client."""

from __future__ import annotations

import argparse
import sys
from typing import Any, Final, Sequence

import uvicorn

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.logging_config import setup_logging
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import (
    RUNTIME_UNAVAILABLE_MESSAGE,
    RUNTIME_UNAVAILABLE_REASON_CODE,
    ComposerState,
)

_REMOVED_RUNTIME_OPTIONS: Final = (
    "--catalog-path",
    "--kernel-mode",
    "--mock-execute-duration-s",
    "--assistant-provider",
    "--assistant-endpoint",
    "--assistant-timeout-s",
    "--assistant-api-token",
    "--mcp",
    "--mcp-http",
    "--trace-dir",
    "--failure-archive-dir",
    "--failure-archive-max-age-s",
    "--failure-archive-max-bytes",
    "--trace-max-age-s",
    "--trace-max-bytes",
    "--disable-failure-archive",
)


class ComposerRuntimeUnavailableError(RuntimeError):
    """Typed rejection for every removed Composer runtime factory path."""

    def __init__(self, *, reason_code: str = RUNTIME_UNAVAILABLE_REASON_CODE) -> None:
        super().__init__(RUNTIME_UNAVAILABLE_MESSAGE)
        self.reason_code = reason_code

    def to_detail(self) -> dict[str, Any]:
        return {
            "reason_code": self.reason_code,
            "message": str(self),
            "missing": ["composable_cell_runtime_adapter"],
            "expected_lane": None,
            "current_lane": None,
            "suggested_command": None,
            "original_error": None,
        }


def _removed_runtime_option(argv: Sequence[str]) -> str | None:
    """Return the first removed runtime option without parsing its payload."""

    for argument in argv:
        for option in _REMOVED_RUNTIME_OPTIONS:
            if argument == option or argument.startswith(f"{option}="):
                return option
    return None


def main() -> None:
    argv = tuple(sys.argv[1:])
    if _removed_runtime_option(argv) is not None:
        raise ComposerRuntimeUnavailableError()

    parser = argparse.ArgumentParser(
        description="ManyForge Composer structural and headless-client shell"
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--program-path")
    parser.add_argument("--static-dir")
    parser.add_argument("--headless-api-url")
    parser.add_argument("--headless-api-timeout-s", type=float, default=2.0)
    parser.add_argument("--assistant-bridge-url")
    parser.add_argument("--assistant-request-timeout-s", type=float, default=10.0)
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
    )
    args = parser.parse_args(argv)

    setup_logging(args.log_level)
    security = SecurityConfig.from_env()
    headless_client = None
    assistant_client = None
    if args.headless_api_url is not None:
        from manyforge_headless_api.client import HeadlessHttpClient

        headless_client = HeadlessHttpClient(
            args.headless_api_url,
            timeout_seconds=args.headless_api_timeout_s,
        )
    if args.assistant_bridge_url is not None:
        if headless_client is None:
            parser.error("--assistant-bridge-url requires --headless-api-url")
        from manyforge_composer.backend.assistant_provider_client import (
            AssistantProviderHttpClient,
        )

        assistant_client = AssistantProviderHttpClient(
            args.assistant_bridge_url,
            timeout_seconds=args.assistant_request_timeout_s,
        )
    try:
        state = ComposerState.from_paths(
            program_path=args.program_path,
            allowed_fs_roots=security.allowed_fs_roots,
            headless_client=headless_client,
            assistant_client=assistant_client,
        )
        app = create_app(
            state,
            static_dir=args.static_dir,
            security=security,
        )
        uvicorn.run(app, host=args.host, port=args.port)
    finally:
        if assistant_client is not None:
            assistant_client.close()
        if headless_client is not None:
            headless_client.close()


if __name__ == "__main__":
    try:
        main()
    except ComposerRuntimeUnavailableError as exc:
        print(f"{exc.reason_code}: {exc}", file=sys.stderr)
        raise SystemExit(78) from None
