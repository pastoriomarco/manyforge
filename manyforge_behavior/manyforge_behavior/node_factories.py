"""Registry factories for context-aware built-in py_trees nodes."""

from __future__ import annotations

from typing import Any

import py_trees

from manyforge_behavior.node_registry import NodeFactoryContext
from manyforge_behavior.nodes.control_flow import RepeatDecorator, RetryDecorator
from manyforge_behavior.nodes.model import QueryModelNode
from manyforge_behavior.nodes.recovery import FallbackOverrideNode, RecoverySkillNode


def sequence(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
    return py_trees.composites.Sequence(
        name=context.name, memory=True, children=list(context.children)
    )


def fallback(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
    return py_trees.composites.Selector(
        name=context.name, memory=True, children=list(context.children)
    )


def parallel(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
    value = str(context.params.get("success_policy", "success_on_all") or "").strip()
    if value in {"", "success_on_all"}:
        policy = py_trees.common.ParallelPolicy.SuccessOnAll()
    elif value == "success_on_one":
        policy = py_trees.common.ParallelPolicy.SuccessOnOne()
    else:
        raise ValueError(
            "Parallel success_policy must be one of: success_on_all, success_on_one"
        )
    return py_trees.composites.Parallel(
        name=context.name, policy=policy, children=list(context.children)
    )


def inverter(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
    return py_trees.decorators.Inverter(name=context.name, child=context.children[0])


def repeat(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
    return RepeatDecorator(
        name=context.name,
        child=context.children[0],
        num_cycles=int(context.params.get("num_cycles", -1)),
    )


def retry(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
    return RetryDecorator(
        name=context.name,
        child=context.children[0],
        num_attempts=int(context.params.get("num_attempts", -1)),
    )


def timer(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
    return py_trees.timers.Timer(
        name=context.name,
        duration=float(context.params.get("duration_s", 1.0)),
    )


def fallback_override(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
    return FallbackOverrideNode(
        name=context.name,
        child=context.children[0],
        fallback_params=dict(context.params.get("fallback_params", {})),
        max_retries=int(context.params.get("max_retries", 1)),
    )


def recovery_skill(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
    return RecoverySkillNode(
        name=context.name,
        child=context.children[0],
        recovery_skill_id=str(context.params.get("recovery_skill_id", "")),
        recovery_params=dict(context.params.get("recovery_params", {})),
        max_retries=int(context.params.get("max_retries", 1)),
        skill_expander=context.services["skill_expander"],
    )


def query_model(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
    params = context.params
    deployment = context.services.get("deployment")
    endpoint_alias = str(params.get("model_endpoint_alias", ""))
    template_id = str(params.get("prompt_template_id", ""))
    endpoint = deployment.model_endpoints.get(endpoint_alias) if deployment else None
    template = deployment.prompt_templates.get(template_id) if deployment else None
    audit_policy = deployment.model_audit_policy if deployment else None
    intervention = context.services.get("intervention_context") or {}
    return QueryModelNode(
        name=context.name,
        model_endpoint_alias=endpoint_alias,
        prompt_template_id=template_id,
        template_variables=params.get("template_variables", {}),
        output_key_prefix=params.get("output_key_prefix", f"model/{context.name}"),
        timeout_s=float(params.get("timeout_s", 30.0)),
        min_confidence=float(params.get("min_confidence", 0.0)),
        retry_on_parse_failure=bool(params.get("retry_on_parse_failure", False)),
        include_scene_snapshot=bool(params.get("include_scene_snapshot", False)),
        scene_snapshot_transport=str(
            params.get("scene_snapshot_transport", "inline_data_url")
        ),
        scene_snapshot_width=int(params.get("scene_snapshot_width", 640)),
        scene_snapshot_height=int(params.get("scene_snapshot_height", 480)),
        scene_snapshot_format=str(params.get("scene_snapshot_format", "jpeg")),
        model_runtime_failure_policy=str(
            params.get("model_runtime_failure_policy", "pause_for_operator")
        ),
        model_low_confidence_policy=str(
            params.get("model_low_confidence_policy", "pause_for_operator")
        ),
        endpoint=endpoint,
        template=template,
        event_recorder=context.services.get("event_recorder"),
        audit_policy=audit_policy,
        scene_snapshot_provider=context.services.get("scene_snapshot_provider"),
        scene_snapshot_url_publisher=context.services.get(
            "scene_snapshot_url_publisher"
        ),
        run_id=intervention.get("run_id"),
        session_id=intervention.get("session_id"),
        principal_id=intervention.get("principal_id"),
        parent_run_id=intervention.get("parent_run_id"),
        runtime_pause_registry=context.services["runtime_pause_registry"],
    )
