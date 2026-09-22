"""Run Composer as a structural/Program-authoring shell and runtime client."""

from __future__ import annotations

import argparse
import hashlib
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
    parser.add_argument(
        "--startup-scenario-document",
        help="open and activate one exact complete v2 Scenario document",
    )
    parser.add_argument("--assistant-bridge-url")
    parser.add_argument(
        "--mcp-http",
        action="store_true",
        help="enable the optional stateless MCP adapter at /mcp/ (requires the mcp extra)",
    )
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
            allowed_program_write_roots=security.allowed_program_write_roots,
            allowed_deployment_write_roots=security.allowed_deployment_write_roots,
            headless_client=headless_client,
            assistant_client=assistant_client,
        )
        app = create_app(
            state,
            static_dir=args.static_dir,
            security=security,
            enable_mcp=args.mcp_http,
        )
        if args.startup_scenario_document is not None:
            if headless_client is None:
                parser.error("--startup-scenario-document requires --headless-api-url")
            if state.authoring_admission is None:
                raise RuntimeError("startup authoring admission is unavailable")
            # This activation precedes ASGI lifespan entry. Establish the
            # process' exact authoring-owner epoch before the shared loader
            # acquires its claim; lifespan recovery is an idempotent recheck.
            state.authoring_admission.recover()
            from manyforge_composer.backend.routes_scenario_documents import (
                LoadScenarioDocumentRequest,
                load_exact_scenario_document,
            )

            program_state = state.draft_session.retained_state()
            deployment_state = state.deployment_draft_session.retained_state()
            workspace = state.authoring_workspace.identity()
            admission = headless_client.authoring_admission()
            generation = admission.get("admission_generation")
            if type(generation) is not int:
                raise RuntimeError("startup authoring admission is unavailable")
            request = LoadScenarioDocumentRequest.model_validate(
                {
                    "path": args.startup_scenario_document,
                    "activate": True,
                    "confirmDiscard": False,
                    "draftId": program_state["draft_id"],
                    "expectedDraftRevision": program_state["draft_revision"],
                    "deploymentDraftId": deployment_state["draft_id"],
                    "expectedDeploymentDraftRevision": deployment_state[
                        "draft_revision"
                    ],
                    "expectedWorkspaceEpoch": workspace.workspace_epoch,
                    "expectedWorkspaceRevision": workspace.workspace_revision,
                    "expectedAdmissionGeneration": generation,
                    "requestOwnerEpoch": state.authoring_admission.owner_epoch,
                    "requestId": "startup-scenario-document-"
                    + hashlib.sha256(
                        args.startup_scenario_document.encode("utf-8")
                    ).hexdigest()[:24],
                }
            )
            load_exact_scenario_document(state, request)
        # The application middleware already emits bounded structured access
        # records. Uvicorn's per-request duplicate is unbounded hot-path I/O
        # and can delay the control WebSocket under read-heavy workloads.
        uvicorn.run(
            app,
            host=args.host,
            port=args.port,
            access_log=False,
            # Real-time control/viewer frames favor bounded CPU and latency;
            # initial static assets still use normal HTTP compression/caching.
            ws_per_message_deflate=False,
        )
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
