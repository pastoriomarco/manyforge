"""Run Composer as a structural/Program-authoring shell and runtime client."""

from __future__ import annotations

import argparse
import logging
import os
from pathlib import Path
import sys
import time
from typing import Any, Final, Sequence

import uvicorn

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.application_identity import (
    parse_application_start_id,
)
from manyforge_composer.backend.composer_settings import ComposerSettingsStore
from manyforge_composer.backend.logging_config import setup_logging
from manyforge_composer.backend.security import SecurityConfig
from manyforge_headless_api.internal_http import (
    INTERNAL_HTTP_SERVER_KEEP_ALIVE_SECONDS,
)
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
            "missing": ["runtime_adapter"],
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


def _open_startup_scenario_document(
    state: ComposerState, headless_client: Any, project_path: str
) -> None:
    """Open the startup project through the one Load path, or start empty.

    Any refusal (for example a Scenario still applied in Manager) logs one
    warning and records the notice the GUI shows; Composer still starts
    serving. A refusal before the workspace commit leaves it empty. There is
    no retry; it runs once the headless API listens.
    """

    try:
        if state.authoring_admission is None:
            raise RuntimeError("startup authoring admission is unavailable")
        from manyforge_composer.backend.routes_scenario_documents import (
            load_exact_scenario_document,
        )

        row = load_exact_scenario_document(state, project_path)
        end = row["end"]
        if end["outcome"] != "succeeded":
            # The orchestrated Load ended at a failed step; nothing is retried.
            raise _StartupLoadFailed({"code": end["code"], "message": end["code"]})
    except Exception as error:  # Every refused startup load takes this one path.
        detail = getattr(error, "detail", None)
        code = detail.get("code") if isinstance(detail, dict) else None
        reason = (
            str(detail.get("message") if isinstance(detail, dict) else error)
            or type(error).__name__
        )
        message = f"Startup project {project_path} was not activated: {reason}"
        logging.getLogger(__name__).warning("%s", message, exc_info=code is None)
        state.startup_scenario_notice = {
            "path": project_path,
            "code": code if isinstance(code, str) else "startup_scenario_load_failed",
            "message": message,
        }


def _wait_for_headless_listening(headless_client: Any) -> None:
    """Return once the headless API answers (OD-332).

    The Manager starts Composer after its first converged baseline, which
    may come before the headless API listens. Any answer counts, an error
    status included; only a request that reached no server is repeated. No
    time limit: if the headless API exits, the Manager ends the application.
    """

    from manyforge_headless_api.client import HeadlessClientError

    logged = False
    while True:
        try:
            headless_client.health()
            return
        except HeadlessClientError as error:
            if error.code != "headless_http_unavailable":
                return
        if not logged:
            logging.getLogger(__name__).info("waiting for the headless API to listen")
            logged = True
        time.sleep(0.1)


class _StartupLoadFailed(RuntimeError):
    """The startup Load's row ended other than `succeeded`; carries its code."""

    def __init__(self, error: dict) -> None:
        super().__init__(error.get("message", ""))
        self.detail = error


def _application_start_id(text: str) -> int:
    try:
        return parse_application_start_id(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def main() -> None:
    argv = tuple(sys.argv[1:])
    if _removed_runtime_option(argv) is not None:
        raise ComposerRuntimeUnavailableError()

    parser = argparse.ArgumentParser(
        description="ManyForge Composer structural and headless-client shell"
    )
    parser.add_argument(
        "--application-start-id",
        required=True,
        type=_application_start_id,
        help="this application start's identity (16 lowercase hex), from the Manager",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--program-path")
    parser.add_argument("--static-dir")
    parser.add_argument("--headless-api-url")
    parser.add_argument("--headless-api-timeout-s", type=float, default=2.0)
    parser.add_argument(
        "--startup-scenario-document",
        help="open and activate one exact complete v2 Scenario document",
    )
    parser.add_argument(
        "--mcp-http",
        action="store_true",
        help="enable the optional stateless MCP adapter at /mcp/ (requires the mcp extra)",
    )
    parser.add_argument(
        "--settings-dir",
        help=(
            "folder for installation Composer settings; defaults to"
            " $MANYFORGE_STATE_DIR/composer (unset: defaults, not saved)"
        ),
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
    )
    args = parser.parse_args(argv)

    setup_logging(args.log_level)
    security = SecurityConfig.from_env()
    headless_client = None
    state = None
    if args.headless_api_url is not None:
        from manyforge_headless_api.client import HeadlessHttpClient

        headless_client = HeadlessHttpClient(
            args.headless_api_url,
            timeout_seconds=args.headless_api_timeout_s,
        )
        _wait_for_headless_listening(headless_client)
    try:
        state = ComposerState.from_paths(
            application_start_id=args.application_start_id,
            program_path=args.program_path,
            allowed_fs_roots=security.allowed_fs_roots,
            headless_client=headless_client,
        )
        state_dir = os.environ.get("MANYFORGE_STATE_DIR", "").strip()
        settings_dir = (
            Path(args.settings_dir) if args.settings_dir
            else Path(state_dir) / "composer" if state_dir
            else None
        )
        app = create_app(
            state,
            static_dir=args.static_dir,
            security=security,
            enable_mcp=args.mcp_http,
            settings_store=ComposerSettingsStore(settings_dir),
        )
        if args.startup_scenario_document is not None:
            if headless_client is None:
                parser.error("--startup-scenario-document requires --headless-api-url")
            _open_startup_scenario_document(
                state, headless_client, args.startup_scenario_document
            )
        # The application middleware already emits bounded structured access
        # records. Uvicorn's per-request duplicate is unbounded hot-path I/O
        # and can delay the control WebSocket under read-heavy workloads.
        uvicorn.run(
            app,
            host=args.host,
            port=args.port,
            access_log=False,
            timeout_keep_alive=INTERNAL_HTTP_SERVER_KEEP_ALIVE_SECONDS,
            # Real-time control/viewer frames favor bounded CPU and latency;
            # initial static assets still use normal HTTP compression/caching.
            ws_per_message_deflate=False,
        )
    finally:
        if state is not None and state.assistant_client is not None:
            state.assistant_client.close()
        if headless_client is not None:
            headless_client.close()


if __name__ == "__main__":
    try:
        main()
    except ComposerRuntimeUnavailableError as exc:
        print(f"{exc.reason_code}: {exc}", file=sys.stderr)
        raise SystemExit(78) from None
