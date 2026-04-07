"""Pydantic request/response models for ManyForge Composer API."""
from __future__ import annotations

from typing import Any, Optional, List, Dict
from pydantic import BaseModel, ConfigDict, Field

_ALIAS_CFG = ConfigDict(populate_by_name=True)


# --- Cycle ---
class CycleStatusResponse(BaseModel):
    model_config = _ALIAS_CFG
    state: str
    velocity_scale: float = Field(alias="velocityScale")


class VelocityScaleRequest(BaseModel):
    value: float


class VelocityScaleResponse(BaseModel):
    model_config = _ALIAS_CFG
    velocity_scale: float = Field(alias="velocityScale")


class CycleCommandResponse(BaseModel):
    state: str


# --- Catalog ---
class CatalogParameterResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    type: str
    required: bool
    default: Any = None
    enum_values: List[str] = Field(default_factory=list, alias="enumValues")
    constraints: Dict[str, Any] = Field(default_factory=dict)
    parameterizable: bool = False
    description: str = ""


class CatalogEntryResponse(BaseModel):
    model_config = _ALIAS_CFG
    id: str
    name: str
    category: str
    description: str
    kind: str
    children_min: int = Field(alias="childrenMin")
    children_max: int = Field(alias="childrenMax")
    parameters: List[CatalogParameterResponse] = Field(default_factory=list)
    required_capabilities: List[str] = Field(default_factory=list, alias="requiredCapabilities")


class CatalogResponse(BaseModel):
    version: str
    entries: List[CatalogEntryResponse]


# --- Program ---
class LoadProgramRequest(BaseModel):
    model_config = _ALIAS_CFG
    path: str
    deployment_path: Optional[str] = Field(None, alias="deploymentPath")


class RobotResponse(BaseModel):
    model_config = _ALIAS_CFG
    robot_id: str = Field(alias="robotId")
    group_id: str = Field(alias="groupId")
    base_frame: str = Field(alias="baseFrame")
    tcp_frame: str = Field(alias="tcpFrame")


class TreeNodeResponse(BaseModel):
    id: str
    name: str
    params: Dict[str, Any] = Field(default_factory=dict)
    children: List["TreeNodeResponse"] = Field(default_factory=list)


class ProgramResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    description: str
    program_profile: str = Field(alias="programProfile")
    robot: RobotResponse
    scene_count: int = Field(alias="sceneCount")
    parameter_count: int = Field(alias="parameterCount")
    source_path: str = Field(alias="sourcePath", default="")


class ProgramTreeResponse(BaseModel):
    tree: TreeNodeResponse


class SaveProgramRequest(BaseModel):
    path: str


class UpdateTreeRequest(BaseModel):
    tree: TreeNodeResponse


# --- Deployment ---
class LoadDeploymentRequest(BaseModel):
    path: str


class RobotConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    robot_id: str = Field(alias="robotId")
    group_id: str = Field(alias="groupId")
    urdf_path: str = Field(alias="urdfPath")
    global_max_tcp_linear_speed_mps: float = Field(alias="globalMaxTcpLinearSpeedMps")


class DeploymentResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    description: str
    program_profile: str = Field(alias="programProfile")
    robot_config: RobotConfigResponse = Field(alias="robotConfig")


class AdapterConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    adapter_id: str = Field(alias="adapterId")
    adapter_version: str = Field(alias="adapterVersion")
    config: Dict[str, Any] = Field(default_factory=dict)
    supported_profiles: List[str] = Field(default_factory=list, alias="supportedProfiles")
    capability_keys: List[str] = Field(default_factory=list, alias="capabilityKeys")


class OrchestratorConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    engine: str
    config: Dict[str, Any] = Field(default_factory=dict)


class LaunchRunnerResponse(BaseModel):
    model_config = _ALIAS_CFG
    module: str
    args: List[str] = Field(default_factory=list)


class LaunchUiBackendResponse(BaseModel):
    model_config = _ALIAS_CFG
    module: str = ""
    args: List[str] = Field(default_factory=list)
    enabled: bool = True


class LaunchConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    runner: LaunchRunnerResponse
    ui_backend: Optional[LaunchUiBackendResponse] = Field(None, alias="uiBackend")


class FullDeploymentResponse(BaseModel):
    model_config = _ALIAS_CFG
    api_version: str = Field(alias="apiVersion")
    schema_version: str = Field(alias="schemaVersion")
    name: str
    description: str
    program_profile: str = Field(alias="programProfile")
    robot_config: RobotConfigResponse = Field(alias="robotConfig")
    adapters: Dict[str, AdapterConfigResponse]
    orchestrator: OrchestratorConfigResponse
    launch: LaunchConfigResponse


class UpdateDeploymentRequest(BaseModel):
    model_config = _ALIAS_CFG
    name: Optional[str] = None
    description: Optional[str] = None
    program_profile: Optional[str] = Field(None, alias="programProfile")
    robot_config: Optional[Dict[str, Any]] = Field(None, alias="robotConfig")
    adapters: Optional[Dict[str, Dict[str, Any]]] = None
    orchestrator: Optional[Dict[str, Any]] = None
    launch: Optional[Dict[str, Any]] = None


class SaveDeploymentRequest(BaseModel):
    path: str


# --- WebSocket ---
class StateUpdate(BaseModel):
    model_config = _ALIAS_CFG
    state: str
    velocity_scale: float = Field(alias="velocityScale")
    node_statuses: Dict[str, str] = Field(default_factory=dict, alias="nodeStatuses")
    timestamp: float
