"""ComposerState - Holds the runtime state shared across all API routes."""
from __future__ import annotations

import logging
from pathlib import Path
from typing import Any, Callable, Optional

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program
from manyforge_behavior.deployment import Deployment
from manyforge_behavior.cycle_manager import CycleManager
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_behavior.kernel_client import KernelClient

logger = logging.getLogger(__name__)


class ComposerState:
    """Holds the runtime state shared across all API routes."""

    def __init__(
        self,
        catalog_path: str,
        kernel: KernelClient,
        kernel_factory: Optional[Callable[[Optional[Deployment]], tuple[KernelClient, Any]]] = None,
        kernel_bundle: Any = None,
    ):
        self.catalog = NodeCatalog(catalog_path)
        self._catalog_path = str(Path(catalog_path).resolve())
        self._kernel = kernel
        self._kernel_factory = kernel_factory
        self.kernel_bundle = kernel_bundle
        self.program: Optional[Program] = None
        self.program_source_path: str = ""
        self.deployment: Optional[Deployment] = None
        self.cycle_manager: Optional[CycleManager] = None
        self.hmi_server = None
        self._on_program_loaded: Optional[Callable[["ComposerState"], None]] = None

    def load_program(self, program_path: str, deployment_path: Optional[str] = None) -> Program:
        """Load a program (and optional deployment). Creates new runner + CycleManager."""
        # Stop existing cycle manager if running
        if self.cycle_manager is not None:
            self.cycle_manager.reset()

        # Load deployment first (if provided)
        deployment = self.deployment
        if deployment_path:
            deployment = Deployment(deployment_path)
            self.deployment = deployment

        if deployment is not None and self._kernel_factory is not None:
            self._kernel, self.kernel_bundle = self._kernel_factory(deployment)

        # Create runner
        runner = OrchestratorRunner(
            catalog_path=self._catalog_path,
            program_path=program_path,
            kernel=self._kernel,
            deployment_path=deployment_path,
        )
        runner.load()

        self.program = runner.program
        self.program_source_path = program_path
        self.cycle_manager = CycleManager(runner, self._kernel)

        # Notify listeners (e.g. Viser HMI)
        if self._on_program_loaded:
            try:
                self._on_program_loaded(self)
            except Exception:
                logger.exception("on_program_loaded callback failed")

        return self.program
