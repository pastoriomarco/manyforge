"""Program and deployment routes - /api/program/* and /api/deployment/*"""
from __future__ import annotations

from fastapi import APIRouter, HTTPException
from manyforge_behavior.deployment import Deployment
from manyforge_behavior.program import Program

from .models import (
    LoadProgramRequest,
    ProgramResponse,
    ProgramTreeResponse,
    TreeNodeResponse,
    RobotResponse,
    LoadDeploymentRequest,
    DeploymentResponse,
    RobotConfigResponse,
    SaveProgramRequest,
    UpdateTreeRequest,
    UpdateDeploymentRequest,
    SaveDeploymentRequest,
    FullDeploymentResponse,
    ApplySessionRequest,
    ApplySessionResponse,
)
from .state import ComposerState


def _tree_to_response(node) -> dict:
    """Convert a TreeNode to response dict."""
    return TreeNodeResponse(
        id=node.id,
        name=node.name,
        params=node.params,
        children=[_tree_to_response(child) for child in node.children],
    ).model_dump(by_alias=True)


def _deployment_to_full_response(deployment: Deployment, source_path: str) -> dict:
    return FullDeploymentResponse(
        apiVersion=deployment.api_version,
        schemaVersion=deployment.schema_version,
        name=deployment.name,
        description=deployment.description,
        programProfile=deployment.program_profile,
        robotConfig=RobotConfigResponse(
            robotId=deployment.robot_config.robot_id,
            groupId=deployment.robot_config.group_id,
            urdfPath=deployment.robot_config.urdf_path,
            globalMaxTcpLinearSpeedMps=deployment.robot_config.global_max_tcp_linear_speed_mps,
        ),
        adapters={
            slot: {
                "adapterId": adapter.adapter_id,
                "adapterVersion": adapter.adapter_version,
                "config": adapter.config,
                "supportedProfiles": adapter.supported_profiles,
                "capabilityKeys": adapter.capability_keys,
            }
            for slot, adapter in deployment.adapters.items()
        },
        orchestrator={
            "engine": deployment.orchestrator.engine,
            "config": deployment.orchestrator.config,
        },
        launch={
            "runner": {
                "module": deployment.launch.runner.module,
                "args": deployment.launch.runner.args,
            },
            "uiBackend": (
                {
                    "module": deployment.launch.ui_backend.module,
                    "args": deployment.launch.ui_backend.args,
                    "enabled": deployment.launch.ui_backend.enabled,
                }
                if deployment.launch and deployment.launch.ui_backend
                else None
            ),
        },
        sourcePath=source_path,
    ).model_dump(by_alias=True)


def create_program_router(state: ComposerState) -> APIRouter:
    """Create the program router with the given state."""
    router = APIRouter()
    
    @router.post("/program/load", response_model=ProgramResponse, response_model_by_alias=True)
    def load_program(request: LoadProgramRequest) -> dict:
        """Load a program from the given path."""
        try:
            program = state.load_program(request.path, request.deployment_path)
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e))
        
        robot = program.robot
        return ProgramResponse(
            name=program.name,
            description=program.description,
            programProfile=program.program_profile,
            robot=RobotResponse(
                robotId=robot.robot_id,
                groupId=robot.group_id,
                baseFrame=robot.base_frame,
                tcpFrame=robot.tcp_frame,
            ),
            sceneCount=len(program.scene),
            parameterCount=len(program.parameters),
            sourcePath=state.program_source_path,
        ).model_dump(by_alias=True)

    @router.get("/program", response_model=ProgramResponse, response_model_by_alias=True)
    def get_program() -> dict:
        """Get the currently loaded program."""
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")

        program = state.program
        robot = program.robot
        return ProgramResponse(
            name=program.name,
            description=program.description,
            programProfile=program.program_profile,
            robot=RobotResponse(
                robotId=robot.robot_id,
                groupId=robot.group_id,
                baseFrame=robot.base_frame,
                tcpFrame=robot.tcp_frame,
            ),
            sceneCount=len(program.scene),
            parameterCount=len(program.parameters),
            sourcePath=state.program_source_path,
        ).model_dump(by_alias=True)
    
    @router.get("/program/tree", response_model=ProgramTreeResponse, response_model_by_alias=True)
    def get_program_tree() -> dict:
        """Get the behavior tree of the currently loaded program."""
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        
        return ProgramTreeResponse(
            tree=_tree_to_response(state.program.tree),
        ).model_dump(by_alias=True)

    @router.get("/program/saved/tree", response_model=ProgramTreeResponse, response_model_by_alias=True)
    def get_saved_program_tree() -> dict:
        """Get the behavior tree from the saved program artifact on disk."""
        if not state.program_source_path:
            raise HTTPException(status_code=404, detail="No saved program path available")
        try:
            saved_program = Program(state.program_source_path, state.catalog)
        except Exception as e:
            raise HTTPException(status_code=500, detail=str(e))
        return ProgramTreeResponse(
            tree=_tree_to_response(saved_program.tree),
        ).model_dump(by_alias=True)
    
    @router.post("/deployment/load", response_model=DeploymentResponse, response_model_by_alias=True)
    def load_deployment(request: LoadDeploymentRequest) -> dict:
        """Load a deployment from the given path."""
        try:
            deployment = Deployment(request.path)
            state.deployment = deployment
            state.deployment_source_path = request.path
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e))
        
        deployment = state.deployment
        robot_config = deployment.robot_config
        return DeploymentResponse(
            name=deployment.name,
            description=deployment.description,
            programProfile=deployment.program_profile,
            robotConfig=RobotConfigResponse(
                robotId=robot_config.robot_id,
                groupId=robot_config.group_id,
                urdfPath=robot_config.urdf_path,
                globalMaxTcpLinearSpeedMps=robot_config.global_max_tcp_linear_speed_mps,
            ),
        ).model_dump(by_alias=True)
    
    @router.get("/deployment", response_model=DeploymentResponse, response_model_by_alias=True)
    def get_deployment() -> dict:
        """Get the currently loaded deployment."""
        if state.deployment is None:
            raise HTTPException(status_code=404, detail="No deployment loaded")
        
        deployment = state.deployment
        robot_config = deployment.robot_config
        return DeploymentResponse(
            name=deployment.name,
            description=deployment.description,
            programProfile=deployment.program_profile,
            robotConfig=RobotConfigResponse(
                robotId=robot_config.robot_id,
                groupId=robot_config.group_id,
                urdfPath=robot_config.urdf_path,
                globalMaxTcpLinearSpeedMps=robot_config.global_max_tcp_linear_speed_mps,
            ),
        ).model_dump(by_alias=True)

    @router.get("/deployment/full", response_model=FullDeploymentResponse, response_model_by_alias=True)
    def get_full_deployment() -> dict:
        """Get the currently loaded full deployment artifact."""
        if state.deployment is None:
            raise HTTPException(status_code=404, detail="No deployment loaded")
        return _deployment_to_full_response(state.deployment, state.deployment_source_path)

    @router.get("/deployment/saved/full", response_model=FullDeploymentResponse, response_model_by_alias=True)
    def get_saved_full_deployment() -> dict:
        """Get the saved deployment artifact from disk."""
        if not state.deployment_source_path:
            raise HTTPException(status_code=404, detail="No saved deployment path available")
        try:
            saved_deployment = Deployment(state.deployment_source_path)
        except Exception as e:
            raise HTTPException(status_code=500, detail=str(e))
        return _deployment_to_full_response(saved_deployment, state.deployment_source_path)

    @router.put("/program/tree")
    def update_program_tree(request: UpdateTreeRequest) -> dict:
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        if state.cycle_manager is not None and state.cycle_manager.state.value == "running":
            raise HTTPException(status_code=409, detail="Cannot apply program changes while running")
        try:
            program_data = state.program.to_dict()
            program_data["tree"] = request.tree.model_dump()
            updated_program = Program.from_dict(
                program_data,
                state.catalog,
                path=state.program_source_path or "<memory>",
            )
            deployment = state.deployment
            if deployment is not None:
                deployment.validate_against_program(updated_program)
                deployment.validate_capabilities()
            state.apply_models(updated_program, deployment)
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e))
        return {"status": "ok"}

    @router.post("/program/save")
    def save_program(request: SaveProgramRequest) -> dict:
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        try:
            if request.tree is None:
                state.program.save(request.path)
            else:
                program_data = state.program.to_dict()
                program_data["tree"] = request.tree.model_dump()
                Program.from_dict(
                    program_data,
                    state.catalog,
                    path=request.path,
                ).save(request.path)
        except Exception as e:
            raise HTTPException(status_code=500, detail=str(e))
        return {"status": "ok", "path": request.path}

    @router.put("/deployment")
    def update_deployment(request: UpdateDeploymentRequest) -> dict:
        if state.deployment is None:
            raise HTTPException(status_code=404, detail="No deployment loaded")
        if state.cycle_manager is not None and state.cycle_manager.state.value == "running":
            raise HTTPException(status_code=409, detail="Cannot apply deployment changes while running")
        try:
            deployment_dict = state.deployment.to_dict()
            patch_dict = request.model_dump(exclude_unset=True)
            for key, value in patch_dict.items():
                deployment_dict[key] = value
            updated_deployment = Deployment.from_dict(
                deployment_dict,
                path=state.deployment_source_path or "<memory>",
            )
            if state.program is not None:
                updated_deployment.validate_against_program(state.program)
                updated_deployment.validate_capabilities()
                state.apply_models(state.program, updated_deployment)
            else:
                state.deployment = updated_deployment
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e))
        return {"status": "ok"}

    @router.post("/deployment/save")
    def save_deployment(request: SaveDeploymentRequest) -> dict:
        if state.deployment is None:
            raise HTTPException(status_code=404, detail="No deployment loaded")
        try:
            if request.deployment is None:
                state.deployment.save(request.path)
            else:
                deployment_data = request.deployment.model_dump()
                candidate = Deployment.from_dict(deployment_data, path=request.path)
                if state.program is not None:
                    candidate.validate_against_program(state.program)
                    candidate.validate_capabilities()
                candidate.save(request.path)
        except Exception as e:
            raise HTTPException(status_code=500, detail=str(e))
        return {"status": "ok", "path": request.path}

    @router.post("/session/apply", response_model=ApplySessionResponse)
    def apply_session(request: ApplySessionRequest) -> dict:
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        if state.cycle_manager is not None and state.cycle_manager.state.value == "running":
            raise HTTPException(status_code=409, detail="Cannot apply changes while running")

        try:
            program = state.program
            if request.tree is not None:
                program_data = state.program.to_dict()
                program_data["tree"] = request.tree.model_dump()
                program = Program.from_dict(
                    program_data,
                    state.catalog,
                    path=state.program_source_path or "<memory>",
                )

            deployment = state.deployment
            if request.deployment is not None:
                deployment = Deployment.from_dict(
                    request.deployment.model_dump(),
                    path=state.deployment_source_path or "<memory>",
                )

            if deployment is not None:
                deployment.validate_against_program(program)
                deployment.validate_capabilities()

            state.apply_models(program, deployment)
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e))

        return {"status": "ok"}

    return router
