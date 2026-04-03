"""ComposerState - Holds the runtime state shared across all API routes."""
from __future__ import annotations

from pathlib import Path
from typing import Optional

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program
from manyforge_behavior.deployment import Deployment
from manyforge_behavior.cycle_manager import CycleManager
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_behavior.kernel_client import KernelClient


class ComposerState:
    """Holds the runtime state shared across all API routes."""
    
    def __init__(self, catalog_path: str, kernel: KernelClient):
        self.catalog = NodeCatalog(catalog_path)
        self._catalog_path = str(Path(catalog_path).resolve())
        self._kernel = kernel
        self.program: Optional[Program] = None
        self.deployment: Optional[Deployment] = None
        self.cycle_manager: Optional[CycleManager] = None
    
    def load_program(self, program_path: str, deployment_path: Optional[str] = None) -> Program:
        """Load a program (and optional deployment). Creates new runner + CycleManager."""
        # Stop existing cycle manager if running
        if self.cycle_manager is not None:
            self.cycle_manager.reset()
        
        # Load deployment first (if provided)
        deployment = None
        if deployment_path:
            deployment = Deployment(deployment_path)
            self.deployment = deployment
        
        # Create runner
        runner = OrchestratorRunner(
            catalog_path=self._catalog_path,
            program_path=program_path,
            kernel=self._kernel,
            deployment_path=deployment_path,
        )
        runner.load()
        
        self.program = runner.program
        self.cycle_manager = CycleManager(runner, self._kernel)
        return self.program
