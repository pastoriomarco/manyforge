"""Deployment artifact loader and validator for ManyForge."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import yaml

EXPECTED_API_VERSION = "manyforge.deployment.v0"


@dataclass
class RobotConfig:
    robot_id: str
    group_id: str
    urdf_path: str
    srdf_path: str = ""
    global_max_tcp_linear_speed_mps: float = 0.0


@dataclass
class AdapterConfig:
    adapter_id: str
    adapter_version: str
    config: dict[str, Any] = field(default_factory=dict)


@dataclass
class OrchestratorConfig:
    engine: str
    config: dict[str, Any] = field(default_factory=dict)


@dataclass
class RunnerConfig:
    module: str
    args: list[str] = field(default_factory=list)


@dataclass
class UiBackendConfig:
    module: str = ""
    args: list[str] = field(default_factory=list)
    enabled: bool = True


@dataclass
class LaunchConfig:
    runner: RunnerConfig
    ui_backend: Optional[UiBackendConfig] = None


class DeploymentValidationError(Exception):
    """Raised when deployment artifact validation fails."""
    pass


class Deployment:
    """Loads and validates a ManyForge deployment artifact from YAML."""

    def __init__(self, path: str | Path):
        self._path = Path(path)
        self.name: str = ""
        self.description: str = ""
        self.api_version: str = ""
        self.schema_version: str = ""
        self.program_profile: str = ""
        self.robot_config: Optional[RobotConfig] = None
        self.adapters: dict[str, AdapterConfig] = {}
        self.orchestrator: Optional[OrchestratorConfig] = None
        self.launch: Optional[LaunchConfig] = None
        self._load_and_validate()

    def _load_and_validate(self) -> None:
        with open(self._path) as f:
            data = yaml.safe_load(f)

        if not isinstance(data, dict):
            raise DeploymentValidationError("Deployment must be a YAML mapping")

        # Schema validation
        api_version = data.get("api_version")
        if api_version != EXPECTED_API_VERSION:
            raise DeploymentValidationError(
                f"Expected api_version '{EXPECTED_API_VERSION}', got '{api_version}'"
            )
        self.api_version = api_version

        self.schema_version = data.get("schema_version", "")
        if not self.schema_version:
            raise DeploymentValidationError("schema_version is required")

        self.program_profile = data.get("program_profile", "")
        if not self.program_profile:
            raise DeploymentValidationError("program_profile is required")

        self.name = data.get("name", "")
        self.description = data.get("description", "")

        # Robot config
        robot_data = data.get("robot_config")
        if not robot_data:
            raise DeploymentValidationError("robot_config section is required")
        for field_name in ("robot_id", "group_id", "urdf_path"):
            if field_name not in robot_data:
                raise DeploymentValidationError(
                    f"robot_config.{field_name} is required"
                )
        tcp_speed = robot_data.get("global_max_tcp_linear_speed_mps", 0.0)
        if tcp_speed <= 0:
            raise DeploymentValidationError(
                "robot_config.global_max_tcp_linear_speed_mps must be > 0"
            )
        self.robot_config = RobotConfig(
            robot_id=robot_data["robot_id"],
            group_id=robot_data["group_id"],
            urdf_path=robot_data["urdf_path"],
            srdf_path=robot_data.get("srdf_path", ""),
            global_max_tcp_linear_speed_mps=tcp_speed,
        )

        # Adapters
        adapters_data = data.get("adapters", {})
        required_adapter_slots = ("planning", "context", "execution")
        for slot in required_adapter_slots:
            if slot not in adapters_data:
                raise DeploymentValidationError(
                    f"adapters.{slot} section is required"
                )
            adapter_data = adapters_data[slot]
            if "adapter_id" not in adapter_data:
                raise DeploymentValidationError(
                    f"adapters.{slot}.adapter_id is required"
                )
            if "adapter_version" not in adapter_data:
                raise DeploymentValidationError(
                    f"adapters.{slot}.adapter_version is required"
                )
            self.adapters[slot] = AdapterConfig(
                adapter_id=adapter_data["adapter_id"],
                adapter_version=adapter_data["adapter_version"],
                config=adapter_data.get("config", {}),
            )

        # Orchestrator
        orchestrator_data = data.get("orchestrator")
        if not orchestrator_data:
            raise DeploymentValidationError("orchestrator section is required")
        if "engine" not in orchestrator_data:
            raise DeploymentValidationError(
                "orchestrator.engine is required"
            )
        self.orchestrator = OrchestratorConfig(
            engine=orchestrator_data["engine"],
            config=orchestrator_data.get("config", {}),
        )

        # Launch
        launch_data = data.get("launch")
        if not launch_data:
            raise DeploymentValidationError("launch section is required")
        runner_data = launch_data.get("runner")
        if not runner_data:
            raise DeploymentValidationError("launch.runner section is required")
        if "module" not in runner_data:
            raise DeploymentValidationError(
                "launch.runner.module is required"
            )
        runner = RunnerConfig(
            module=runner_data["module"],
            args=runner_data.get("args", []),
        )

        # UI backend (optional)
        ui_backend: Optional[UiBackendConfig] = None
        ui_data = launch_data.get("ui_backend")
        if ui_data is not None:
            ui_backend = UiBackendConfig(
                module=ui_data.get("module", ""),
                args=ui_data.get("args", []),
                enabled=ui_data.get("enabled", True),
            )

        self.launch = LaunchConfig(runner=runner, ui_backend=ui_backend)

    def validate_against_program(self, program: Any) -> None:
        """Cross-artifact validation against a Program.

        Raises DeploymentValidationError if:
        - program_profile doesn't match
        - robot_config.robot_id doesn't match program.robot.robot_id
        - robot_config.group_id doesn't match program.robot.group_id
        """
        if self.program_profile != program.program_profile:
            raise DeploymentValidationError(
                f"program_profile mismatch: deployment has '{self.program_profile}', "
                f"program has '{program.program_profile}'"
            )

        if self.robot_config.robot_id != program.robot.robot_id:
            raise DeploymentValidationError(
                f"robot_id mismatch: deployment has '{self.robot_config.robot_id}', "
                f"program has '{program.robot.robot_id}'"
            )

        if self.robot_config.group_id != program.robot.group_id:
            raise DeploymentValidationError(
                f"group_id mismatch: deployment has '{self.robot_config.group_id}', "
                f"program has '{program.robot.group_id}'"
            )
