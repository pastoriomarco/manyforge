#!/usr/bin/env python3
"""Run ManyForge qualification scenarios against a live or self-launched Composer backend."""
from __future__ import annotations

import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import subprocess
import sys
import time

import httpx

from manyforge_composer.verification import (
    ScenarioRunner,
    expand_matrix_scenarios,
    load_matrix_definition,
    load_scenario_definition,
)


class ManagedComposerProcess:
    """Launch and manage a local ManyForge Composer process for qualification runs."""

    def __init__(
        self,
        *,
        workspace_root: Path,
        catalog_path: Path,
        host: str,
        port: int,
        log_level: str,
        kernel_mode: str,
        mock_execute_duration_s: float,
        output_dir: Path | None,
        wait_timeout_s: float,
    ) -> None:
        self._workspace_root = workspace_root
        self._catalog_path = catalog_path
        self._host = host
        self._port = port
        self._log_level = log_level
        self._kernel_mode = kernel_mode
        self._mock_execute_duration_s = mock_execute_duration_s
        self._output_dir = output_dir
        self._wait_timeout_s = wait_timeout_s
        self._process: subprocess.Popen[str] | None = None
        self._stdout_handle = None
        self._stderr_handle = None

    def __enter__(self) -> "ManagedComposerProcess":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop()

    @property
    def base_url(self) -> str:
        return f"http://{self._host}:{self._port}"

    # Grace window after first readiness 200 before trusting it: a foreign
    # composer already on the port answers immediately while our bind-failed
    # child takes a moment to exit.
    _POST_READY_GRACE_S = 0.5

    def _refuse_foreign_composer(self) -> None:
        """Fail closed when the target port is already served.

        The readiness probe cannot distinguish the spawned worker from a
        composer that was already listening, so it would silently adopt and
        drive whatever is on the port.
        """
        try:
            with httpx.Client(base_url=self.base_url, timeout=2.0) as client:
                response = client.get("/api/health")
        except Exception:
            return  # nothing answering: the port is ours to take
        raise RuntimeError(
            f"Refusing to launch: {self.base_url} already serves a composer this "
            f"harness did not spawn (HTTP {response.status_code} from /api/health). "
            "Stop it or pass a dedicated --port."
        )

    def start(self) -> None:
        self._refuse_foreign_composer()
        env = os.environ.copy()
        allowed_roots = [str(self._workspace_root), "/workspace", "/tmp"]
        env["MANYFORGE_FS_ROOTS"] = ",".join(dict.fromkeys(allowed_roots))
        env.setdefault("PYTHONDONTWRITEBYTECODE", "1")
        cmd = [
            sys.executable,
            "-m",
            "manyforge_composer",
            "--catalog-path",
            str(self._catalog_path),
            "--host",
            self._host,
            "--port",
            str(self._port),
            "--log-level",
            self._log_level,
            "--kernel-mode",
            self._kernel_mode,
            "--mock-execute-duration-s",
            str(self._mock_execute_duration_s),
        ]
        if self._output_dir is not None:
            self._output_dir.mkdir(parents=True, exist_ok=True)
            self._stdout_handle = (self._output_dir / "composer.stdout.log").open("w", encoding="utf-8")
            self._stderr_handle = (self._output_dir / "composer.stderr.log").open("w", encoding="utf-8")
            stdout = self._stdout_handle
            stderr = self._stderr_handle
        else:
            stdout = subprocess.DEVNULL
            stderr = subprocess.DEVNULL

        self._process = subprocess.Popen(
            cmd,
            cwd=self._workspace_root,
            env=env,
            stdout=stdout,
            stderr=stderr,
            text=True,
        )
        try:
            deadline = time.monotonic() + self._wait_timeout_s
            with httpx.Client(base_url=self.base_url, timeout=2.0) as client:
                while time.monotonic() < deadline:
                    if self._process.poll() is not None:
                        raise RuntimeError(
                            f"Composer exited early with code {self._process.returncode}"
                        )
                    healthy = False
                    try:
                        response = client.get("/api/health")
                        healthy = response.status_code == 200
                    except Exception:
                        pass
                    if healthy:
                        time.sleep(self._POST_READY_GRACE_S)
                        if self._process.poll() is not None:
                            raise RuntimeError(
                                "Composer exited with code "
                                f"{self._process.returncode} immediately after "
                                "readiness; the health answer likely came from "
                                f"a foreign composer already on {self.base_url}"
                            )
                        return
                    time.sleep(0.1)
            raise RuntimeError("Timed out waiting for Composer health endpoint")
        except Exception:
            self.stop()
            raise

    def stop(self) -> None:
        if self._process is not None and self._process.poll() is None:
            self._process.terminate()
            try:
                self._process.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait(timeout=5.0)
        for handle in (self._stdout_handle, self._stderr_handle):
            if handle is not None:
                handle.close()


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "scenarios",
        nargs="*",
        help="Scenario YAML file(s) to execute",
    )
    parser.add_argument(
        "--matrix",
        default=None,
        help="Scenario matrix YAML file",
    )
    parser.add_argument(
        "--base-url",
        default="http://127.0.0.1:9000",
        help="Composer backend base URL when not launching a local process",
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Optional directory for retained JSON artifacts",
    )
    parser.add_argument(
        "--archive-output",
        default=None,
        help="Optional tar.gz path for archived retained artifacts",
    )
    parser.add_argument(
        "--workspace-root",
        default=str(Path(__file__).resolve().parents[1]),
        help="Workspace root used to resolve relative paths",
    )
    parser.add_argument(
        "--launch",
        choices=["none", "local-process"],
        default="none",
        help="Optionally launch Composer automatically for the qualification run",
    )
    parser.add_argument(
        "--catalog-path",
        default="manyforge_behavior/resources/node_catalog.yaml",
        help="Catalog path used when launching a local Composer process",
    )
    parser.add_argument("--host", default="127.0.0.1", help="Host for launched Composer")
    parser.add_argument("--port", type=int, default=9000, help="Port for launched Composer")
    parser.add_argument(
        "--wait-timeout-s",
        type=float,
        default=20.0,
        help="Health-check timeout for launched Composer",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Composer log level when launching locally",
    )
    parser.add_argument(
        "--kernel-mode",
        default="mock",
        choices=["auto", "mock"],
        help="Kernel mode for self-launched Composer processes",
    )
    parser.add_argument(
        "--mock-execute-duration-s",
        type=float,
        default=0.2,
        help="Per-move execution duration for mock-kernel qualification runs",
    )
    return parser.parse_args()


def _load_scenarios(args: argparse.Namespace, workspace_root: Path):
    matrix_definition = None
    definitions = []
    if args.matrix:
        matrix_definition = load_matrix_definition((workspace_root / args.matrix).resolve())
        definitions.extend(expand_matrix_scenarios(matrix_definition, workspace_root=workspace_root))
    for path in args.scenarios:
        candidate = Path(path)
        if not candidate.is_absolute():
            candidate = (workspace_root / candidate).resolve()
        else:
            candidate = candidate.resolve()
        definitions.append(load_scenario_definition(candidate))
    if not definitions:
        raise SystemExit("No scenarios specified. Provide scenario paths or --matrix.")
    return matrix_definition, definitions


def main() -> int:
    args = _parse_args()
    workspace_root = Path(args.workspace_root).resolve()
    output_dir = Path(args.output_dir).resolve() if args.output_dir else None
    archive_output = Path(args.archive_output).resolve() if args.archive_output else None
    matrix_definition, definitions = _load_scenarios(args, workspace_root)
    if archive_output is None and output_dir is not None and matrix_definition is not None:
        archive_output = output_dir.with_suffix(".tar.gz")

    with ExitStack() as stack:
        base_url = args.base_url
        if args.launch == "local-process":
            launcher = ManagedComposerProcess(
                workspace_root=workspace_root,
                catalog_path=(workspace_root / args.catalog_path).resolve(),
                host=args.host,
                port=args.port,
                log_level=args.log_level,
                kernel_mode=args.kernel_mode,
                mock_execute_duration_s=args.mock_execute_duration_s,
                output_dir=output_dir,
                wait_timeout_s=args.wait_timeout_s,
            )
            stack.enter_context(launcher)
            base_url = launcher.base_url

        with httpx.Client(base_url=base_url, timeout=10.0) as client:
            runner = ScenarioRunner(client, workspace_root=workspace_root)
            summary = runner.run_many(
                definitions,
                matrix_definition=matrix_definition,
                output_dir=output_dir,
                archive_path=archive_output,
            )

    for result in summary["results"]:
        print(f"[scenario] {result['scenarioId']}: success")
    for failure in summary["failures"]:
        print(f"[scenario] {failure['scenarioId']}: failed - {failure['error']['message']}")

    if summary["matrixId"]:
        print(
            f"[matrix] {summary['matrixId']}: "
            f"{summary['passedCount']} passed / {summary['scenarioCount']} total"
        )
        if summary.get("archivePath"):
            print(f"[matrix] artifacts archived at {summary['archivePath']}")
    return 1 if summary["failedCount"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
