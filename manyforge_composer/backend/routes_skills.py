"""Capability and skill-discovery routes."""
from __future__ import annotations

import time

from fastapi import APIRouter, HTTPException

from .models import (
    CapabilitiesResponse,
    SkillCatalogResponse,
    SkillParameterResponse,
    SkillResponse,
)
from .state import ComposerState


def _derive_contact_tier(capability_keys: set[str]) -> str:
    if any(
        key.startswith("execution.contact.force_torque")
        or key.startswith("sensing.force_torque")
        or key.startswith("execution.admittance")
        for key in capability_keys
    ):
        return "force_backed"
    if any(key.startswith("execution.contact.") for key in capability_keys):
        return "geometry_only"
    return "unsupported"


def build_capabilities_payload(state: ComposerState) -> dict:
    if state.deployment is None:
        raise HTTPException(status_code=404, detail="No deployment loaded")

    categories: dict[str, set[str]] = {
        "planning": set(),
        "context": set(),
        "scene": set(),
        "execution": set(),
        "sensing": set(),
        "other": set(),
    }
    all_keys: set[str] = set()
    adapter_ids: dict[str, str] = {}

    for slot, adapter in state.deployment.adapters.items():
        adapter_ids[slot] = adapter.adapter_id
        for key in adapter.capability_keys:
            all_keys.add(key)
            prefix = key.split(".", 1)[0]
            if prefix in categories:
                categories[prefix].add(key)
            else:
                categories["other"].add(key)

    return CapabilitiesResponse(
        planning=sorted(categories["planning"]),
        context=sorted(categories["context"]),
        scene=sorted(categories["scene"]),
        execution=sorted(categories["execution"]),
        sensing=sorted(categories["sensing"]),
        other=sorted(categories["other"]),
        adapterIds=adapter_ids,
        programProfile=state.deployment.program_profile,
        contactTier=_derive_contact_tier(all_keys),
        timestampNs=time.time_ns(),
    ).model_dump(by_alias=True)


def create_skills_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    @router.get("/capabilities", response_model=CapabilitiesResponse, response_model_by_alias=True)
    def get_capabilities() -> dict:
        return build_capabilities_payload(state)

    @router.get("/skills", response_model=SkillCatalogResponse, response_model_by_alias=True)
    def get_skills() -> dict:
        """Return the skill catalog for LLM composition."""
        skills = []
        for skill in state.skill_catalog.skills:
            skills.append(SkillResponse(
                id=skill.id,
                version=skill.version,
                name=skill.name,
                description=skill.description,
                compatibleProfiles=skill.compatible_profiles,
                requires=skill.requires,
                provides=skill.provides,
                modifies=skill.modifies,
                requiresCapabilities=skill.requires_capabilities,
                requiresTools=skill.requires_tools,
                parameters=[
                    SkillParameterResponse(
                        name=p.name,
                        type=p.type,
                        required=p.required,
                        default=p.default,
                        description=p.description,
                    )
                    for p in skill.parameters
                ],
                body=skill.body,
                expandsTo=skill.expands_to,
                example=skill.example,
            ))
        return SkillCatalogResponse(
            skills=skills,
            timestampNs=time.time_ns(),
        ).model_dump(by_alias=True)

    return router
