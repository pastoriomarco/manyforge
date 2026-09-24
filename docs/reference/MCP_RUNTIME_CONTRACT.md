<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py mcp
  Last regenerated: 2026-09-24 13:29:10 UTC
  Generator source: scripts/regen-public-contracts.py:gen_mcp_api
-->

# ManyForge MCP HTTP contract

Optional Streamable HTTP at `/mcp/`, enabled with Composer's `--mcp-http` and the `manyforge_composer[mcp]` extra. Disabled by default. The adapter is stateless: no MCP session ID, replay store, SSE fallback, heartbeat or background polling. It accepts the SDK initialize/tools/list/tools/call protocol; separate calls do not share execution authority.

Each action delegates to the listed existing REST route. Supply its exact body and current owner-issued identities/confirmation/freshness; the adapter does not manufacture these, infer a target, retry or weaken admission. Read current backend state after commands. A lost tool response never proves no effect and must not trigger a new motion command. Kernel and Program Runner retain lifecycle ownership and reserved Stop capacity.

Requests are limited to 1 MiB; owner response/query bounds also apply. Configure `MANYFORGE_MCP_ALLOWED_HOSTS` for remote Host names/IPs and `MANYFORGE_CORS_ORIGINS` for browser origins. Default MCP hosts are loopback. These are transport bounds, not caller authorization. Use the deployment's trusted network/perimeter; request attribution is not authentication.

JSON route bodies are returned as text with `isError` for HTTP failures; viewer snapshots return MCP image content and require an existing matching viewer. Tools list only configured routes; capability availability is still determined by their owners. This is the current wire contract, not full historical-functional or provider qualification. Historical v0 signatures are preserved in git at `08222188293df8e12f90dc72e204c21f22f92ea1`.

## manyforge_scene_state

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": false,
    "idempotentHint": true,
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {},
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/scene",
        "properties": {
          "action": {
            "const": "read",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_scene_state"
}
```

## manyforge_scene_edit

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "AddSceneObjectRequest": {
        "additionalProperties": false,
        "properties": {
          "frame_ref": {
            "discriminator": {
              "mapping": {
                "cell_frame": "#/components/schemas/CellSceneFrameRequest",
                "component_frame": "#/components/schemas/ComponentSceneFrameRequest",
                "passive_model_frame": "#/components/schemas/PassiveModelSceneFrameRequest",
                "scene_resource": "#/components/schemas/SceneResourceSceneFrameRequest"
              },
              "propertyName": "kind"
            },
            "oneOf": [
              {
                "$ref": "#/$defs/CellSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/ComponentSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/PassiveModelSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/SceneResourceSceneFrameRequest"
              }
            ],
            "title": "Frame Ref"
          },
          "kind": {
            "const": "add_object",
            "title": "Kind",
            "type": "string"
          },
          "pose": {
            "$ref": "#/$defs/Pose3Request"
          },
          "scene_resource_id": {
            "title": "Scene Resource Id",
            "type": "string"
          },
          "shape": {
            "discriminator": {
              "mapping": {
                "box": "#/components/schemas/BoxShapeRequest",
                "cylinder": "#/components/schemas/CylinderShapeRequest",
                "mesh": "#/components/schemas/MeshShapeRequest",
                "sphere": "#/components/schemas/SphereShapeRequest"
              },
              "propertyName": "type"
            },
            "oneOf": [
              {
                "$ref": "#/$defs/BoxShapeRequest"
              },
              {
                "$ref": "#/$defs/SphereShapeRequest"
              },
              {
                "$ref": "#/$defs/CylinderShapeRequest"
              },
              {
                "$ref": "#/$defs/MeshShapeRequest"
              }
            ],
            "title": "Shape"
          },
          "support_contacts": {
            "items": {
              "$ref": "#/$defs/SceneSupportContactRequest"
            },
            "maxItems": 4096,
            "title": "Support Contacts",
            "type": "array"
          }
        },
        "required": [
          "kind",
          "scene_resource_id",
          "shape",
          "pose",
          "frame_ref"
        ],
        "title": "AddSceneObjectRequest",
        "type": "object"
      },
      "AttachCarriedObjectRequest": {
        "additionalProperties": false,
        "properties": {
          "carried_object_attachment_id": {
            "title": "Carried Object Attachment Id",
            "type": "string"
          },
          "command_target_id": {
            "title": "Command Target Id",
            "type": "string"
          },
          "controllable_component_id": {
            "title": "Controllable Component Id",
            "type": "string"
          },
          "expected_component_attachment_revisions": {
            "items": {
              "pattern": "^sha256:[0-9a-f]{64}$",
              "type": "string"
            },
            "maxItems": 4096,
            "title": "Expected Component Attachment Revisions",
            "type": "array"
          },
          "frame_role_id": {
            "title": "Frame Role Id",
            "type": "string"
          },
          "kind": {
            "const": "attach_carried_object",
            "title": "Kind",
            "type": "string"
          },
          "scene_resource_id": {
            "title": "Scene Resource Id",
            "type": "string"
          },
          "touch_body_roles": {
            "items": {
              "type": "string"
            },
            "maxItems": 4096,
            "title": "Touch Body Roles",
            "type": "array"
          }
        },
        "required": [
          "kind",
          "carried_object_attachment_id",
          "scene_resource_id",
          "controllable_component_id",
          "command_target_id",
          "frame_role_id",
          "touch_body_roles",
          "expected_component_attachment_revisions"
        ],
        "title": "AttachCarriedObjectRequest",
        "type": "object"
      },
      "BoxShapeRequest": {
        "additionalProperties": false,
        "properties": {
          "box_dimensions_m": {
            "$ref": "#/$defs/Vector3Request"
          },
          "type": {
            "const": "box",
            "title": "Type",
            "type": "string"
          }
        },
        "required": [
          "type",
          "box_dimensions_m"
        ],
        "title": "BoxShapeRequest",
        "type": "object"
      },
      "CallerAttributionRequest": {
        "additionalProperties": false,
        "properties": {
          "correlation_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Correlation Id"
          },
          "request_id": {
            "title": "Request Id",
            "type": "string"
          },
          "request_owner_epoch": {
            "title": "Request Owner Epoch",
            "type": "string"
          },
          "source_label": {
            "title": "Source Label",
            "type": "string"
          }
        },
        "required": [
          "request_id",
          "request_owner_epoch",
          "source_label"
        ],
        "title": "CallerAttributionRequest",
        "type": "object"
      },
      "CellSceneFrameRequest": {
        "additionalProperties": false,
        "properties": {
          "cell_id": {
            "title": "Cell Id",
            "type": "string"
          },
          "kind": {
            "const": "cell_frame",
            "title": "Kind",
            "type": "string"
          }
        },
        "required": [
          "kind",
          "cell_id"
        ],
        "title": "CellSceneFrameRequest",
        "type": "object"
      },
      "CollisionPairRequest": {
        "additionalProperties": false,
        "properties": {
          "body_a": {
            "title": "Body A",
            "type": "string"
          },
          "body_b": {
            "title": "Body B",
            "type": "string"
          }
        },
        "required": [
          "body_a",
          "body_b"
        ],
        "title": "CollisionPairRequest",
        "type": "object"
      },
      "CollisionPolicyRequest": {
        "additionalProperties": false,
        "properties": {
          "allowed_static_static_pairs": {
            "items": {
              "$ref": "#/$defs/CollisionPairRequest"
            },
            "maxItems": 4096,
            "title": "Allowed Static Static Pairs",
            "type": "array"
          },
          "allowed_touch_link_pairs": {
            "items": {
              "$ref": "#/$defs/CollisionPairRequest"
            },
            "maxItems": 4096,
            "title": "Allowed Touch Link Pairs",
            "type": "array"
          },
          "contact_grasps": {
            "items": {
              "$ref": "#/$defs/ContactGraspPolicyRequest"
            },
            "maxItems": 4096,
            "title": "Contact Grasps",
            "type": "array"
          },
          "policy_id": {
            "title": "Policy Id",
            "type": "string"
          },
          "require_full_diagnostics": {
            "title": "Require Full Diagnostics",
            "type": "boolean"
          },
          "revision": {
            "$ref": "#/$defs/CollisionPolicyRevisionRequest"
          },
          "support_contacts": {
            "items": {
              "$ref": "#/$defs/SupportContactPolicyRequest"
            },
            "maxItems": 4096,
            "title": "Support Contacts",
            "type": "array"
          }
        },
        "required": [
          "policy_id",
          "revision",
          "allowed_static_static_pairs",
          "allowed_touch_link_pairs",
          "support_contacts",
          "contact_grasps",
          "require_full_diagnostics"
        ],
        "title": "CollisionPolicyRequest",
        "type": "object"
      },
      "CollisionPolicyRevisionRequest": {
        "additionalProperties": false,
        "properties": {
          "content_hash": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Content Hash",
            "type": "string"
          },
          "revision_id": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Revision Id",
            "type": "integer"
          }
        },
        "required": [
          "revision_id",
          "content_hash"
        ],
        "title": "CollisionPolicyRevisionRequest",
        "type": "object"
      },
      "ComponentSceneFrameRequest": {
        "additionalProperties": false,
        "properties": {
          "controllable_component_id": {
            "title": "Controllable Component Id",
            "type": "string"
          },
          "frame_role_id": {
            "title": "Frame Role Id",
            "type": "string"
          },
          "kind": {
            "const": "component_frame",
            "title": "Kind",
            "type": "string"
          },
          "model_instance_id": {
            "title": "Model Instance Id",
            "type": "string"
          }
        },
        "required": [
          "kind",
          "controllable_component_id",
          "model_instance_id",
          "frame_role_id"
        ],
        "title": "ComponentSceneFrameRequest",
        "type": "object"
      },
      "ComposerSceneTransactionRequest": {
        "additionalProperties": false,
        "description": "Scene-owner request with a Draft fence only for authored mutations.",
        "properties": {
          "expected_admission_generation": {
            "anyOf": [
              {
                "maximum": 9007199254740991.0,
                "minimum": 1.0,
                "type": "integer"
              },
              {
                "type": "null"
              }
            ],
            "title": "Expected Admission Generation"
          },
          "expected_draft_id": {
            "anyOf": [
              {
                "maxLength": 256,
                "minLength": 1,
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Expected Draft Id"
          },
          "expected_draft_revision": {
            "anyOf": [
              {
                "maximum": 9007199254740991.0,
                "minimum": 0.0,
                "type": "integer"
              },
              {
                "type": "null"
              }
            ],
            "title": "Expected Draft Revision"
          },
          "expected_projection_revision": {
            "$ref": "#/$defs/ProjectionRevisionRequest"
          },
          "operation": {
            "discriminator": {
              "mapping": {
                "add_object": "#/components/schemas/AddSceneObjectRequest",
                "attach_carried_object": "#/components/schemas/AttachCarriedObjectRequest",
                "detach_carried_object": "#/components/schemas/DetachCarriedObjectRequest",
                "detach_carried_object_at_current_pose": "#/components/schemas/DetachCarriedObjectAtCurrentPoseRequest",
                "remove_object": "#/components/schemas/RemoveSceneObjectRequest",
                "set_collision_policy": "#/components/schemas/SetCollisionPolicyRequest",
                "update_object_pose": "#/components/schemas/UpdateSceneObjectPoseRequest",
                "upsert_object": "#/components/schemas/UpsertSceneObjectRequest"
              },
              "propertyName": "kind"
            },
            "oneOf": [
              {
                "$ref": "#/$defs/AddSceneObjectRequest"
              },
              {
                "$ref": "#/$defs/UpsertSceneObjectRequest"
              },
              {
                "$ref": "#/$defs/RemoveSceneObjectRequest"
              },
              {
                "$ref": "#/$defs/UpdateSceneObjectPoseRequest"
              },
              {
                "$ref": "#/$defs/AttachCarriedObjectRequest"
              },
              {
                "$ref": "#/$defs/DetachCarriedObjectRequest"
              },
              {
                "$ref": "#/$defs/DetachCarriedObjectAtCurrentPoseRequest"
              },
              {
                "$ref": "#/$defs/SetCollisionPolicyRequest"
              }
            ],
            "title": "Operation"
          },
          "program_node_evidence": {
            "anyOf": [
              {
                "$ref": "#/$defs/ProgramNodeAdmissionEvidenceRequest"
              },
              {
                "type": "null"
              }
            ]
          },
          "program_reset_scene_authorization": {
            "anyOf": [
              {
                "$ref": "#/$defs/ProgramResetSceneAuthorizationRequest"
              },
              {
                "type": "null"
              }
            ]
          },
          "request_context": {
            "$ref": "#/$defs/CallerAttributionRequest"
          }
        },
        "required": [
          "request_context",
          "expected_projection_revision",
          "operation"
        ],
        "title": "ComposerSceneTransactionRequest",
        "type": "object"
      },
      "ContactGraspPolicyRequest": {
        "additionalProperties": false,
        "properties": {
          "allowed_robot_links": {
            "items": {
              "type": "string"
            },
            "maxItems": 4096,
            "title": "Allowed Robot Links",
            "type": "array"
          },
          "max_penetration_m": {
            "minimum": 0.0,
            "title": "Max Penetration M",
            "type": "number"
          },
          "policy_id": {
            "title": "Policy Id",
            "type": "string"
          },
          "selected_object_id": {
            "title": "Selected Object Id",
            "type": "string"
          },
          "terminal_window_start_fraction": {
            "maximum": 1.0,
            "minimum": 0.0,
            "title": "Terminal Window Start Fraction",
            "type": "number"
          }
        },
        "required": [
          "policy_id",
          "selected_object_id",
          "allowed_robot_links",
          "max_penetration_m",
          "terminal_window_start_fraction"
        ],
        "title": "ContactGraspPolicyRequest",
        "type": "object"
      },
      "CylinderShapeRequest": {
        "additionalProperties": false,
        "properties": {
          "cylinder_height_m": {
            "exclusiveMinimum": 0.0,
            "title": "Cylinder Height M",
            "type": "number"
          },
          "cylinder_radius_m": {
            "exclusiveMinimum": 0.0,
            "title": "Cylinder Radius M",
            "type": "number"
          },
          "type": {
            "const": "cylinder",
            "title": "Type",
            "type": "string"
          }
        },
        "required": [
          "type",
          "cylinder_radius_m",
          "cylinder_height_m"
        ],
        "title": "CylinderShapeRequest",
        "type": "object"
      },
      "DetachCarriedObjectAtCurrentPoseRequest": {
        "additionalProperties": false,
        "properties": {
          "carried_object_attachment_id": {
            "title": "Carried Object Attachment Id",
            "type": "string"
          },
          "expected_carried_object_attachment_revision": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Expected Carried Object Attachment Revision",
            "type": "string"
          },
          "expected_command_target_id": {
            "title": "Expected Command Target Id",
            "type": "string"
          },
          "expected_controllable_component_id": {
            "title": "Expected Controllable Component Id",
            "type": "string"
          },
          "expected_scene_resource_id": {
            "title": "Expected Scene Resource Id",
            "type": "string"
          },
          "kind": {
            "const": "detach_carried_object_at_current_pose",
            "title": "Kind",
            "type": "string"
          }
        },
        "required": [
          "kind",
          "carried_object_attachment_id",
          "expected_carried_object_attachment_revision",
          "expected_scene_resource_id",
          "expected_controllable_component_id",
          "expected_command_target_id"
        ],
        "title": "DetachCarriedObjectAtCurrentPoseRequest",
        "type": "object"
      },
      "DetachCarriedObjectRequest": {
        "additionalProperties": false,
        "properties": {
          "carried_object_attachment_id": {
            "title": "Carried Object Attachment Id",
            "type": "string"
          },
          "expected_carried_object_attachment_revision": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Expected Carried Object Attachment Revision",
            "type": "string"
          },
          "frame_ref": {
            "discriminator": {
              "mapping": {
                "cell_frame": "#/components/schemas/CellSceneFrameRequest",
                "component_frame": "#/components/schemas/ComponentSceneFrameRequest",
                "passive_model_frame": "#/components/schemas/PassiveModelSceneFrameRequest",
                "scene_resource": "#/components/schemas/SceneResourceSceneFrameRequest"
              },
              "propertyName": "kind"
            },
            "oneOf": [
              {
                "$ref": "#/$defs/CellSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/ComponentSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/PassiveModelSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/SceneResourceSceneFrameRequest"
              }
            ],
            "title": "Frame Ref"
          },
          "kind": {
            "const": "detach_carried_object",
            "title": "Kind",
            "type": "string"
          },
          "pose": {
            "$ref": "#/$defs/Pose3Request"
          }
        },
        "required": [
          "kind",
          "carried_object_attachment_id",
          "expected_carried_object_attachment_revision",
          "pose",
          "frame_ref"
        ],
        "title": "DetachCarriedObjectRequest",
        "type": "object"
      },
      "MeshShapeRequest": {
        "additionalProperties": false,
        "properties": {
          "mesh_resource_uri": {
            "title": "Mesh Resource Uri",
            "type": "string"
          },
          "mesh_scale": {
            "$ref": "#/$defs/Vector3Request"
          },
          "type": {
            "const": "mesh",
            "title": "Type",
            "type": "string"
          }
        },
        "required": [
          "type",
          "mesh_resource_uri",
          "mesh_scale"
        ],
        "title": "MeshShapeRequest",
        "type": "object"
      },
      "PassiveModelSceneFrameRequest": {
        "additionalProperties": false,
        "properties": {
          "frame_role_id": {
            "title": "Frame Role Id",
            "type": "string"
          },
          "kind": {
            "const": "passive_model_frame",
            "title": "Kind",
            "type": "string"
          },
          "model_instance_id": {
            "title": "Model Instance Id",
            "type": "string"
          }
        },
        "required": [
          "kind",
          "model_instance_id",
          "frame_role_id"
        ],
        "title": "PassiveModelSceneFrameRequest",
        "type": "object"
      },
      "Pose3Request": {
        "additionalProperties": false,
        "properties": {
          "orientation": {
            "$ref": "#/$defs/QuaternionRequest"
          },
          "position_m": {
            "$ref": "#/$defs/Vector3Request"
          }
        },
        "required": [
          "position_m",
          "orientation"
        ],
        "title": "Pose3Request",
        "type": "object"
      },
      "ProgramNodeAdmissionEvidenceRequest": {
        "additionalProperties": false,
        "description": "Exact runtime-node attribution; the Kernel checks the sealed owner API.",
        "properties": {
          "node_attempt_id": {
            "title": "Node Attempt Id",
            "type": "string"
          },
          "node_instance_id": {
            "title": "Node Instance Id",
            "type": "string"
          },
          "owner_api": {
            "enum": [
              "scene.transaction",
              "scene.carried_attachment"
            ],
            "title": "Owner Api",
            "type": "string"
          },
          "parent_program_cycle_claim_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Parent Program Cycle Claim Generation",
            "type": "integer"
          },
          "program_run_id": {
            "title": "Program Run Id",
            "type": "string"
          },
          "program_runner_owner_epoch": {
            "title": "Program Runner Owner Epoch",
            "type": "string"
          }
        },
        "required": [
          "program_runner_owner_epoch",
          "program_run_id",
          "parent_program_cycle_claim_generation",
          "node_instance_id",
          "node_attempt_id",
          "owner_api"
        ],
        "title": "ProgramNodeAdmissionEvidenceRequest",
        "type": "object"
      },
      "ProgramResetSceneAuthorizationRequest": {
        "additionalProperties": false,
        "description": "Internal exact scope for one backend-owned Reset scene mutation.",
        "properties": {
          "cycle_claim_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Cycle Claim Generation",
            "type": "integer"
          },
          "program_run_id": {
            "title": "Program Run Id",
            "type": "string"
          },
          "program_runner_owner_epoch": {
            "title": "Program Runner Owner Epoch",
            "type": "string"
          },
          "terminal_or_settlement_reference": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Terminal Or Settlement Reference",
            "type": "string"
          }
        },
        "required": [
          "program_runner_owner_epoch",
          "program_run_id",
          "cycle_claim_generation",
          "terminal_or_settlement_reference"
        ],
        "title": "ProgramResetSceneAuthorizationRequest",
        "type": "object"
      },
      "ProjectionRevisionRequest": {
        "additionalProperties": false,
        "properties": {
          "graph_revision": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Graph Revision",
            "type": "string"
          },
          "retained_bundle_id": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Retained Bundle Id",
            "type": "string"
          },
          "scene_revision": {
            "$ref": "#/$defs/SceneRevisionRequest"
          }
        },
        "required": [
          "retained_bundle_id",
          "graph_revision",
          "scene_revision"
        ],
        "title": "ProjectionRevisionRequest",
        "type": "object"
      },
      "QuaternionRequest": {
        "additionalProperties": false,
        "properties": {
          "w": {
            "title": "W",
            "type": "number"
          },
          "x": {
            "title": "X",
            "type": "number"
          },
          "y": {
            "title": "Y",
            "type": "number"
          },
          "z": {
            "title": "Z",
            "type": "number"
          }
        },
        "required": [
          "x",
          "y",
          "z",
          "w"
        ],
        "title": "QuaternionRequest",
        "type": "object"
      },
      "RemoveSceneObjectRequest": {
        "additionalProperties": false,
        "properties": {
          "kind": {
            "const": "remove_object",
            "title": "Kind",
            "type": "string"
          },
          "scene_resource_id": {
            "title": "Scene Resource Id",
            "type": "string"
          }
        },
        "required": [
          "kind",
          "scene_resource_id"
        ],
        "title": "RemoveSceneObjectRequest",
        "type": "object"
      },
      "SceneResourceSceneFrameRequest": {
        "additionalProperties": false,
        "properties": {
          "kind": {
            "const": "scene_resource",
            "title": "Kind",
            "type": "string"
          },
          "scene_resource_id": {
            "title": "Scene Resource Id",
            "type": "string"
          }
        },
        "required": [
          "kind",
          "scene_resource_id"
        ],
        "title": "SceneResourceSceneFrameRequest",
        "type": "object"
      },
      "SceneRevisionRequest": {
        "additionalProperties": false,
        "properties": {
          "content_hash": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Content Hash",
            "type": "string"
          },
          "revision_id": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Revision Id",
            "type": "integer"
          },
          "scene_id": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Scene Id",
            "type": "string"
          }
        },
        "required": [
          "scene_id",
          "revision_id",
          "content_hash"
        ],
        "title": "SceneRevisionRequest",
        "type": "object"
      },
      "SceneSupportContactRequest": {
        "additionalProperties": false,
        "description": "Portable object-to-object support intent, never a native body pair.",
        "properties": {
          "allow_recontact": {
            "default": false,
            "title": "Allow Recontact",
            "type": "boolean"
          },
          "enforce_max_start_penetration": {
            "default": true,
            "title": "Enforce Max Start Penetration",
            "type": "boolean"
          },
          "max_contact_penetration_m": {
            "default": 0.0,
            "minimum": 0.0,
            "title": "Max Contact Penetration M",
            "type": "number"
          },
          "max_start_penetration_m": {
            "default": 0.0,
            "minimum": 0.0,
            "title": "Max Start Penetration M",
            "type": "number"
          },
          "min_release_progress_m": {
            "default": 0.0,
            "minimum": 0.0,
            "title": "Min Release Progress M",
            "type": "number"
          },
          "mode": {
            "default": "contact_release",
            "enum": [
              "contact_release",
              "bounded_penetration",
              "contact_process"
            ],
            "title": "Mode",
            "type": "string"
          },
          "release_evidence_mode": {
            "default": "signed_penetration",
            "enum": [
              "signed_penetration",
              "binary_contact"
            ],
            "title": "Release Evidence Mode",
            "type": "string"
          },
          "release_tolerance_m": {
            "default": 1e-05,
            "minimum": 0.0,
            "title": "Release Tolerance M",
            "type": "number"
          },
          "require_separation_by_end": {
            "default": true,
            "title": "Require Separation By End",
            "type": "boolean"
          },
          "require_strictly_decreasing_penetration": {
            "default": true,
            "title": "Require Strictly Decreasing Penetration",
            "type": "boolean"
          },
          "support_object_id": {
            "title": "Support Object Id",
            "type": "string"
          }
        },
        "required": [
          "support_object_id"
        ],
        "title": "SceneSupportContactRequest",
        "type": "object"
      },
      "SetCollisionPolicyRequest": {
        "additionalProperties": false,
        "properties": {
          "configured_policy": {
            "$ref": "#/$defs/CollisionPolicyRequest"
          },
          "expected_configured_policy_revision": {
            "$ref": "#/$defs/CollisionPolicyRevisionRequest"
          },
          "kind": {
            "const": "set_collision_policy",
            "title": "Kind",
            "type": "string"
          }
        },
        "required": [
          "kind",
          "configured_policy",
          "expected_configured_policy_revision"
        ],
        "title": "SetCollisionPolicyRequest",
        "type": "object"
      },
      "SphereShapeRequest": {
        "additionalProperties": false,
        "properties": {
          "sphere_radius_m": {
            "exclusiveMinimum": 0.0,
            "title": "Sphere Radius M",
            "type": "number"
          },
          "type": {
            "const": "sphere",
            "title": "Type",
            "type": "string"
          }
        },
        "required": [
          "type",
          "sphere_radius_m"
        ],
        "title": "SphereShapeRequest",
        "type": "object"
      },
      "SupportContactPolicyRequest": {
        "additionalProperties": false,
        "properties": {
          "allow_recontact": {
            "title": "Allow Recontact",
            "type": "boolean"
          },
          "enforce_max_start_penetration": {
            "title": "Enforce Max Start Penetration",
            "type": "boolean"
          },
          "max_contact_penetration_m": {
            "minimum": 0.0,
            "title": "Max Contact Penetration M",
            "type": "number"
          },
          "max_start_penetration_m": {
            "minimum": 0.0,
            "title": "Max Start Penetration M",
            "type": "number"
          },
          "min_release_progress_m": {
            "minimum": 0.0,
            "title": "Min Release Progress M",
            "type": "number"
          },
          "mode": {
            "enum": [
              "contact_release",
              "bounded_penetration",
              "contact_process"
            ],
            "title": "Mode",
            "type": "string"
          },
          "pair": {
            "$ref": "#/$defs/CollisionPairRequest"
          },
          "release_evidence_mode": {
            "enum": [
              "signed_penetration",
              "binary_contact"
            ],
            "title": "Release Evidence Mode",
            "type": "string"
          },
          "release_tolerance_m": {
            "minimum": 0.0,
            "title": "Release Tolerance M",
            "type": "number"
          },
          "require_separation_by_end": {
            "title": "Require Separation By End",
            "type": "boolean"
          },
          "require_strictly_decreasing_penetration": {
            "title": "Require Strictly Decreasing Penetration",
            "type": "boolean"
          }
        },
        "required": [
          "pair",
          "mode",
          "release_evidence_mode",
          "enforce_max_start_penetration",
          "max_start_penetration_m",
          "release_tolerance_m",
          "min_release_progress_m",
          "max_contact_penetration_m",
          "require_separation_by_end",
          "require_strictly_decreasing_penetration",
          "allow_recontact"
        ],
        "title": "SupportContactPolicyRequest",
        "type": "object"
      },
      "UpdateSceneObjectPoseRequest": {
        "additionalProperties": false,
        "properties": {
          "expected_attached": {
            "anyOf": [
              {
                "type": "boolean"
              },
              {
                "type": "null"
              }
            ],
            "title": "Expected Attached"
          },
          "frame_ref": {
            "discriminator": {
              "mapping": {
                "cell_frame": "#/components/schemas/CellSceneFrameRequest",
                "component_frame": "#/components/schemas/ComponentSceneFrameRequest",
                "passive_model_frame": "#/components/schemas/PassiveModelSceneFrameRequest",
                "scene_resource": "#/components/schemas/SceneResourceSceneFrameRequest"
              },
              "propertyName": "kind"
            },
            "oneOf": [
              {
                "$ref": "#/$defs/CellSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/ComponentSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/PassiveModelSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/SceneResourceSceneFrameRequest"
              }
            ],
            "title": "Frame Ref"
          },
          "kind": {
            "const": "update_object_pose",
            "title": "Kind",
            "type": "string"
          },
          "pose": {
            "$ref": "#/$defs/Pose3Request"
          },
          "provider_evidence": {
            "title": "Provider Evidence",
            "type": "null"
          },
          "scene_resource_id": {
            "title": "Scene Resource Id",
            "type": "string"
          }
        },
        "required": [
          "kind",
          "scene_resource_id",
          "pose",
          "frame_ref"
        ],
        "title": "UpdateSceneObjectPoseRequest",
        "type": "object"
      },
      "UpsertSceneObjectRequest": {
        "additionalProperties": false,
        "properties": {
          "frame_ref": {
            "discriminator": {
              "mapping": {
                "cell_frame": "#/components/schemas/CellSceneFrameRequest",
                "component_frame": "#/components/schemas/ComponentSceneFrameRequest",
                "passive_model_frame": "#/components/schemas/PassiveModelSceneFrameRequest",
                "scene_resource": "#/components/schemas/SceneResourceSceneFrameRequest"
              },
              "propertyName": "kind"
            },
            "oneOf": [
              {
                "$ref": "#/$defs/CellSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/ComponentSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/PassiveModelSceneFrameRequest"
              },
              {
                "$ref": "#/$defs/SceneResourceSceneFrameRequest"
              }
            ],
            "title": "Frame Ref"
          },
          "kind": {
            "const": "upsert_object",
            "title": "Kind",
            "type": "string"
          },
          "pose": {
            "$ref": "#/$defs/Pose3Request"
          },
          "scene_resource_id": {
            "title": "Scene Resource Id",
            "type": "string"
          },
          "shape": {
            "discriminator": {
              "mapping": {
                "box": "#/components/schemas/BoxShapeRequest",
                "cylinder": "#/components/schemas/CylinderShapeRequest",
                "mesh": "#/components/schemas/MeshShapeRequest",
                "sphere": "#/components/schemas/SphereShapeRequest"
              },
              "propertyName": "type"
            },
            "oneOf": [
              {
                "$ref": "#/$defs/BoxShapeRequest"
              },
              {
                "$ref": "#/$defs/SphereShapeRequest"
              },
              {
                "$ref": "#/$defs/CylinderShapeRequest"
              },
              {
                "$ref": "#/$defs/MeshShapeRequest"
              }
            ],
            "title": "Shape"
          },
          "support_contacts": {
            "items": {
              "$ref": "#/$defs/SceneSupportContactRequest"
            },
            "maxItems": 4096,
            "title": "Support Contacts",
            "type": "array"
          }
        },
        "required": [
          "kind",
          "scene_resource_id",
          "shape",
          "pose",
          "frame_ref"
        ],
        "title": "UpsertSceneObjectRequest",
        "type": "object"
      },
      "Vector3Request": {
        "additionalProperties": false,
        "properties": {
          "x_m": {
            "title": "X M",
            "type": "number"
          },
          "y_m": {
            "title": "Y M",
            "type": "number"
          },
          "z_m": {
            "title": "Z M",
            "type": "number"
          }
        },
        "required": [
          "x_m",
          "y_m",
          "z_m"
        ],
        "title": "Vector3Request",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "POST /api/scene/transactions",
        "properties": {
          "action": {
            "const": "transact",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/ComposerSceneTransactionRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_scene_edit"
}
```

## manyforge_program_load

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "ApplyDraftRequest": {
        "additionalProperties": false,
        "properties": {
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision"
        ],
        "title": "ApplyDraftRequest",
        "type": "object"
      },
      "NewDraftRequest": {
        "additionalProperties": false,
        "properties": {
          "confirmDiscard": {
            "default": false,
            "title": "Confirmdiscard",
            "type": "boolean"
          },
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision"
        ],
        "title": "NewDraftRequest",
        "type": "object"
      },
      "OpenDraftRequest": {
        "additionalProperties": false,
        "properties": {
          "confirmDiscard": {
            "default": false,
            "title": "Confirmdiscard",
            "type": "boolean"
          },
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "path": {
            "maxLength": 4096,
            "minLength": 1,
            "title": "Path",
            "type": "string"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision",
          "path"
        ],
        "title": "OpenDraftRequest",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "POST /api/program/open-draft",
        "properties": {
          "action": {
            "const": "open_draft",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/OpenDraftRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/new-draft",
        "properties": {
          "action": {
            "const": "new_draft",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/NewDraftRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/apply",
        "properties": {
          "action": {
            "const": "apply",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/ApplyDraftRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_program_load"
}
```

## manyforge_program_tree

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "DraftMutationRequest": {
        "additionalProperties": false,
        "properties": {
          "arguments": {
            "additionalProperties": true,
            "title": "Arguments",
            "type": "object"
          },
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "mutation": {
            "maxLength": 128,
            "minLength": 1,
            "title": "Mutation",
            "type": "string"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision",
          "mutation",
          "arguments"
        ],
        "title": "DraftMutationRequest",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/program/draft",
        "properties": {
          "action": {
            "const": "read",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/draft/mutations",
        "properties": {
          "action": {
            "const": "mutate",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/DraftMutationRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_program_tree"
}
```

## manyforge_program_validate

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": false,
    "idempotentHint": true,
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "ValidateProgramSourceRequest": {
        "additionalProperties": false,
        "properties": {
          "authoring": {
            "additionalProperties": true,
            "title": "Authoring",
            "type": "object"
          }
        },
        "required": [
          "authoring"
        ],
        "title": "ValidateProgramSourceRequest",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/program/draft",
        "properties": {
          "action": {
            "const": "current",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/validate",
        "properties": {
          "action": {
            "const": "source",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/ValidateProgramSourceRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_program_validate"
}
```

## manyforge_cycle_control

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "ControlFreshnessContextDTO": {
        "additionalProperties": false,
        "properties": {
          "context_id": {
            "title": "Context Id",
            "type": "string"
          },
          "execution_control_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Execution Control Revision",
            "type": "integer"
          },
          "issued_server_sequence": {
            "title": "Issued Server Sequence",
            "type": "string"
          },
          "receiver_incarnation": {
            "title": "Receiver Incarnation",
            "type": "string"
          },
          "semantic_owner_epoch": {
            "title": "Semantic Owner Epoch",
            "type": "string"
          },
          "valid_for_ns": {
            "const": 750000000,
            "title": "Valid For Ns",
            "type": "integer"
          }
        },
        "required": [
          "context_id",
          "receiver_incarnation",
          "semantic_owner_epoch",
          "execution_control_revision",
          "issued_server_sequence",
          "valid_for_ns"
        ],
        "title": "ControlFreshnessContextDTO",
        "type": "object"
      },
      "ExecutionCommandEnvelopeDTO": {
        "additionalProperties": false,
        "properties": {
          "client_sequence": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Client Sequence"
          },
          "command_kind": {
            "enum": [
              "program_run",
              "program_motion_start",
              "program_motion_stop",
              "program_motion_resume",
              "program_full_stop",
              "program_stop",
              "program_reset",
              "program_resume_full_stop",
              "program_discard_full_stop",
              "program_runtime_override_stage",
              "program_runtime_override_activate",
              "program_runtime_override_clear",
              "program_runtime_override_promote",
              "program_runtime_blackboard_write",
              "program_manual_result_stage",
              "program_manual_handover",
              "gripper_confirm",
              "operation_start",
              "operation_stop",
              "operation_pause",
              "operation_resume",
              "operation_reset",
              "operation_acknowledge_completed",
              "cell_full_stop",
              "execution_recovery",
              "stop_recovery",
              "runtime_scale"
            ],
            "title": "Command Kind",
            "type": "string"
          },
          "control_session_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Control Session Id"
          },
          "correlation_id": {
            "title": "Correlation Id",
            "type": "string"
          },
          "expected_execution_control_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Execution Control Revision",
            "type": "integer"
          },
          "expected_semantic_owner_epoch": {
            "title": "Expected Semantic Owner Epoch",
            "type": "string"
          },
          "freshness": {
            "$ref": "#/$defs/ControlFreshnessContextDTO"
          },
          "schema_version": {
            "const": "manyforge.execution_command.v1",
            "title": "Schema Version",
            "type": "string"
          },
          "source_label": {
            "title": "Source Label",
            "type": "string"
          }
        },
        "required": [
          "schema_version",
          "command_kind",
          "correlation_id",
          "source_label",
          "freshness",
          "expected_semantic_owner_epoch",
          "expected_execution_control_revision"
        ],
        "title": "ExecutionCommandEnvelopeDTO",
        "type": "object"
      },
      "ExecutionCommandRequestDTO": {
        "additionalProperties": false,
        "description": "One strict metadata envelope plus one command-specific strict payload.",
        "properties": {
          "command_envelope": {
            "$ref": "#/$defs/ExecutionCommandEnvelopeDTO"
          },
          "payload": {
            "additionalProperties": true,
            "title": "Payload",
            "type": "object"
          }
        },
        "required": [
          "command_envelope",
          "payload"
        ],
        "title": "ExecutionCommandRequestDTO",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/control/state",
        "properties": {
          "action": {
            "const": "state",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/control/commands",
        "properties": {
          "action": {
            "const": "command",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/ExecutionCommandRequestDTO"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_cycle_control"
}
```

## manyforge_capabilities

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": false,
    "idempotentHint": true,
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {},
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/node-environment",
        "properties": {
          "action": {
            "const": "environment",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/execution/targets",
        "properties": {
          "action": {
            "const": "targets",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_capabilities"
}
```

## manyforge_skills

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": false,
    "idempotentHint": true,
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "SkillExpansionRequest": {
        "additionalProperties": false,
        "description": "Effect-free authoring expansion against one selected build environment.",
        "properties": {
          "arguments": {
            "additionalProperties": true,
            "maxProperties": 32,
            "title": "Arguments",
            "type": "object"
          },
          "component_roles": {
            "additionalProperties": {
              "type": "string"
            },
            "maxProperties": 32,
            "title": "Component Roles",
            "type": "object"
          },
          "expected_environment_digest": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Expected Environment Digest",
            "type": "string"
          },
          "instance_prefix": {
            "pattern": "^[a-z][a-z0-9_.-]{0,127}$",
            "title": "Instance Prefix",
            "type": "string"
          },
          "program_context": {
            "additionalProperties": true,
            "description": "Portable Program declarations without tree/revision or Composer forest/layout fields.",
            "title": "Program Context",
            "type": "object"
          },
          "schema_version": {
            "const": "manyforge.skill_expansion_request.v1",
            "title": "Schema Version",
            "type": "string"
          },
          "skill_id": {
            "pattern": "^[a-z][a-z0-9_.-]{0,127}$",
            "title": "Skill Id",
            "type": "string"
          },
          "target_roles": {
            "additionalProperties": {
              "type": "string"
            },
            "maxProperties": 32,
            "title": "Target Roles",
            "type": "object"
          }
        },
        "required": [
          "schema_version",
          "expected_environment_digest",
          "skill_id",
          "instance_prefix",
          "arguments",
          "component_roles",
          "target_roles",
          "program_context"
        ],
        "title": "SkillExpansionRequest",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/skills",
        "properties": {
          "action": {
            "const": "catalog",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/skills/expand",
        "properties": {
          "action": {
            "const": "expand",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/SkillExpansionRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/assistant/modes",
        "properties": {
          "action": {
            "const": "assistant_modes",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_skills"
}
```

## manyforge_intervention

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "CycleRecoveryActionRequest": {
        "additionalProperties": false,
        "description": "Consume exactly one current owner-issued cycle recovery offer.",
        "properties": {
          "action_id": {
            "enum": [
              "retry_motion_from_current_state",
              "acknowledge_and_continue",
              "retry_after_scene_repair",
              "replan_from_stopped_state",
              "retry_runtime_step",
              "reset_cycle"
            ],
            "title": "Action Id",
            "type": "string"
          },
          "client_sequence": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Client Sequence"
          },
          "correlation_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Correlation Id"
          },
          "expected_owner_epoch": {
            "title": "Expected Owner Epoch",
            "type": "string"
          },
          "offer_id": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Offer Id",
            "type": "string"
          },
          "offer_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Offer Revision",
            "type": "integer"
          },
          "reason": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Reason"
          },
          "request_id": {
            "title": "Request Id",
            "type": "string"
          },
          "request_owner_epoch": {
            "title": "Request Owner Epoch",
            "type": "string"
          },
          "session_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Session Id"
          },
          "source_label": {
            "title": "Source Label",
            "type": "string"
          }
        },
        "required": [
          "request_id",
          "request_owner_epoch",
          "source_label",
          "expected_owner_epoch",
          "offer_id",
          "offer_revision",
          "action_id"
        ],
        "title": "CycleRecoveryActionRequest",
        "type": "object"
      },
      "ManualResultStageRequest": {
        "additionalProperties": false,
        "properties": {
          "client_sequence": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Client Sequence"
          },
          "correlation_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Correlation Id"
          },
          "expected_owner_epoch": {
            "title": "Expected Owner Epoch",
            "type": "string"
          },
          "expected_run_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Run Revision",
            "type": "integer"
          },
          "node_attempt_id": {
            "title": "Node Attempt Id",
            "type": "string"
          },
          "node_instance_id": {
            "title": "Node Instance Id",
            "type": "string"
          },
          "outcome": {
            "enum": [
              "succeeded",
              "failed"
            ],
            "title": "Outcome",
            "type": "string"
          },
          "output": {
            "title": "Output"
          },
          "program_run_id": {
            "title": "Program Run Id",
            "type": "string"
          },
          "request_id": {
            "title": "Request Id",
            "type": "string"
          },
          "request_owner_epoch": {
            "title": "Request Owner Epoch",
            "type": "string"
          },
          "session_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Session Id"
          },
          "source_label": {
            "title": "Source Label",
            "type": "string"
          }
        },
        "required": [
          "request_id",
          "request_owner_epoch",
          "source_label",
          "program_run_id",
          "expected_owner_epoch",
          "expected_run_revision",
          "node_instance_id",
          "node_attempt_id",
          "outcome"
        ],
        "title": "ManualResultStageRequest",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/cycle/status",
        "properties": {
          "action": {
            "const": "state",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/cycle/recovery-actions",
        "properties": {
          "action": {
            "const": "recover",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/CycleRecoveryActionRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/runs/{program_run_id}/manual-result",
        "properties": {
          "action": {
            "const": "manual_result",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/ManualResultStageRequest"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "program_run_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "program_run_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path",
          "body"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_intervention"
}
```

## manyforge_runtime_override

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "RuntimeBlackboardWriteRequest": {
        "additionalProperties": false,
        "properties": {
          "client_sequence": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Client Sequence"
          },
          "correlation_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Correlation Id"
          },
          "expected_owner_epoch": {
            "title": "Expected Owner Epoch",
            "type": "string"
          },
          "expected_run_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Run Revision",
            "type": "integer"
          },
          "key": {
            "title": "Key",
            "type": "string"
          },
          "program_run_id": {
            "title": "Program Run Id",
            "type": "string"
          },
          "request_id": {
            "title": "Request Id",
            "type": "string"
          },
          "request_owner_epoch": {
            "title": "Request Owner Epoch",
            "type": "string"
          },
          "session_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Session Id"
          },
          "source_label": {
            "title": "Source Label",
            "type": "string"
          },
          "value": {
            "title": "Value"
          }
        },
        "required": [
          "request_id",
          "request_owner_epoch",
          "source_label",
          "program_run_id",
          "expected_owner_epoch",
          "expected_run_revision",
          "key",
          "value"
        ],
        "title": "RuntimeBlackboardWriteRequest",
        "type": "object"
      },
      "RuntimeOverrideActionRequest": {
        "additionalProperties": false,
        "properties": {
          "client_sequence": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Client Sequence"
          },
          "correlation_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Correlation Id"
          },
          "expected_override_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Override Revision",
            "type": "integer"
          },
          "expected_owner_epoch": {
            "title": "Expected Owner Epoch",
            "type": "string"
          },
          "expected_run_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Run Revision",
            "type": "integer"
          },
          "override_id": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Override Id",
            "type": "string"
          },
          "program_run_id": {
            "title": "Program Run Id",
            "type": "string"
          },
          "request_id": {
            "title": "Request Id",
            "type": "string"
          },
          "request_owner_epoch": {
            "title": "Request Owner Epoch",
            "type": "string"
          },
          "session_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Session Id"
          },
          "source_label": {
            "title": "Source Label",
            "type": "string"
          }
        },
        "required": [
          "request_id",
          "request_owner_epoch",
          "source_label",
          "program_run_id",
          "expected_owner_epoch",
          "expected_run_revision",
          "override_id",
          "expected_override_revision"
        ],
        "title": "RuntimeOverrideActionRequest",
        "type": "object"
      },
      "RuntimeOverrideMaterializationRequest": {
        "additionalProperties": false,
        "properties": {
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "programRunId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Programrunid",
            "type": "string"
          },
          "proposalId": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Proposalid",
            "type": "string"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision",
          "programRunId",
          "proposalId"
        ],
        "title": "RuntimeOverrideMaterializationRequest",
        "type": "object"
      },
      "RuntimeOverridePromotionRequest": {
        "additionalProperties": false,
        "properties": {
          "client_sequence": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Client Sequence"
          },
          "correlation_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Correlation Id"
          },
          "expected_draft_id": {
            "title": "Expected Draft Id",
            "type": "string"
          },
          "expected_draft_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Draft Revision",
            "type": "integer"
          },
          "expected_override_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Override Revision",
            "type": "integer"
          },
          "expected_owner_epoch": {
            "title": "Expected Owner Epoch",
            "type": "string"
          },
          "expected_run_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Run Revision",
            "type": "integer"
          },
          "override_id": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Override Id",
            "type": "string"
          },
          "program_run_id": {
            "title": "Program Run Id",
            "type": "string"
          },
          "request_id": {
            "title": "Request Id",
            "type": "string"
          },
          "request_owner_epoch": {
            "title": "Request Owner Epoch",
            "type": "string"
          },
          "session_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Session Id"
          },
          "source_label": {
            "title": "Source Label",
            "type": "string"
          }
        },
        "required": [
          "request_id",
          "request_owner_epoch",
          "source_label",
          "program_run_id",
          "expected_owner_epoch",
          "expected_run_revision",
          "override_id",
          "expected_override_revision",
          "expected_draft_id",
          "expected_draft_revision"
        ],
        "title": "RuntimeOverridePromotionRequest",
        "type": "object"
      },
      "RuntimeOverrideStageRequest": {
        "additionalProperties": false,
        "properties": {
          "client_sequence": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Client Sequence"
          },
          "correlation_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Correlation Id"
          },
          "expected_owner_epoch": {
            "title": "Expected Owner Epoch",
            "type": "string"
          },
          "expected_run_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Run Revision",
            "type": "integer"
          },
          "node_attempt_id": {
            "title": "Node Attempt Id",
            "type": "string"
          },
          "node_instance_id": {
            "title": "Node Instance Id",
            "type": "string"
          },
          "parameter_id": {
            "title": "Parameter Id",
            "type": "string"
          },
          "program_run_id": {
            "title": "Program Run Id",
            "type": "string"
          },
          "request_id": {
            "title": "Request Id",
            "type": "string"
          },
          "request_owner_epoch": {
            "title": "Request Owner Epoch",
            "type": "string"
          },
          "session_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Session Id"
          },
          "source_label": {
            "title": "Source Label",
            "type": "string"
          },
          "source_observation_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Source Observation Id"
          },
          "source_owner_generation": {
            "anyOf": [
              {
                "maximum": 9007199254740991.0,
                "minimum": 1.0,
                "type": "integer"
              },
              {
                "type": "null"
              }
            ],
            "title": "Source Owner Generation"
          },
          "value": {
            "title": "Value"
          }
        },
        "required": [
          "request_id",
          "request_owner_epoch",
          "source_label",
          "program_run_id",
          "expected_owner_epoch",
          "expected_run_revision",
          "node_instance_id",
          "node_attempt_id",
          "parameter_id",
          "value"
        ],
        "title": "RuntimeOverrideStageRequest",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/program/runs/{program_run_id}/runtime-controls",
        "properties": {
          "action": {
            "const": "read",
            "type": "string"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "program_run_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "program_run_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/runs/{program_run_id}/runtime-overrides",
        "properties": {
          "action": {
            "const": "stage",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/RuntimeOverrideStageRequest"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "program_run_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "program_run_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/runs/{program_run_id}/runtime-overrides/activate",
        "properties": {
          "action": {
            "const": "activate",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/RuntimeOverrideActionRequest"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "program_run_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "program_run_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/runs/{program_run_id}/runtime-overrides/clear",
        "properties": {
          "action": {
            "const": "clear",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/RuntimeOverrideActionRequest"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "program_run_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "program_run_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/runs/{program_run_id}/runtime-overrides/promote",
        "properties": {
          "action": {
            "const": "promote",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/RuntimeOverridePromotionRequest"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "program_run_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "program_run_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/draft/runtime-override-promotions",
        "properties": {
          "action": {
            "const": "materialize",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/RuntimeOverrideMaterializationRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/runs/{program_run_id}/blackboard-values",
        "properties": {
          "action": {
            "const": "blackboard",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/RuntimeBlackboardWriteRequest"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "program_run_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "program_run_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path",
          "body"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_runtime_override"
}
```

## manyforge_execution_status

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": false,
    "idempotentHint": true,
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {},
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/control/state",
        "properties": {
          "action": {
            "const": "state",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/program/runs/{program_run_id}",
        "properties": {
          "action": {
            "const": "run",
            "type": "string"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "program_run_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "program_run_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/execution/operations/{operation_id}",
        "properties": {
          "action": {
            "const": "operation",
            "type": "string"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "operation_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "operation_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_execution_status"
}
```

## manyforge_debug_export

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": false,
    "idempotentHint": true,
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {},
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/debug/export",
        "properties": {
          "action": {
            "const": "export",
            "type": "string"
          },
          "query": {
            "additionalProperties": false,
            "properties": {
              "action_span_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Action Span Id"
              },
              "activation_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Activation Id"
              },
              "alarm_condition_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Alarm Condition Id"
              },
              "alarm_phase": {
                "anyOf": [
                  {
                    "enum": [
                      "all",
                      "activated",
                      "returned",
                      "acknowledged",
                      "raw"
                    ],
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Alarm Phase"
              },
              "before_sequence": {
                "anyOf": [
                  {
                    "maximum": 9007199254740991,
                    "minimum": 1,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Before Sequence"
              },
              "capture_id": {
                "anyOf": [
                  {
                    "maximum": 9007199254740991,
                    "minimum": 1,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Capture Id"
              },
              "channel": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Channel"
              },
              "clock_owner": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Clock Owner"
              },
              "components": {
                "default": [],
                "items": {
                  "type": "string"
                },
                "maxItems": 256,
                "title": "Components",
                "type": "array"
              },
              "condition_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Condition Id"
              },
              "contact_policy_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Contact Policy Id"
              },
              "end_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "End Ns"
              },
              "end_utc_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "End Utc Ns"
              },
              "event_types": {
                "default": [],
                "items": {
                  "type": "string"
                },
                "maxItems": 256,
                "title": "Event Types",
                "type": "array"
              },
              "incident_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Incident Id"
              },
              "intervention_session_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Intervention Session Id"
              },
              "limit": {
                "default": 32,
                "maximum": 32,
                "minimum": 1,
                "title": "Limit",
                "type": "integer"
              },
              "minimum_severity": {
                "anyOf": [
                  {
                    "enum": [
                      "DEBUG",
                      "INFO",
                      "WARN",
                      "ERROR",
                      "CRITICAL"
                    ],
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Minimum Severity"
              },
              "node_attempt_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Node Attempt Id"
              },
              "node_instance_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Node Instance Id"
              },
              "operation_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Operation Id"
              },
              "parent_node_attempt_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Parent Node Attempt Id"
              },
              "phase": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Phase"
              },
              "program_run_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Program Run Id"
              },
              "recent_seconds": {
                "anyOf": [
                  {
                    "maximum": 2678400,
                    "minimum": 1,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Recent Seconds"
              },
              "resource_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Resource Id"
              },
              "run_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Run Id"
              },
              "search": {
                "default": "",
                "maxLength": 256,
                "title": "Search",
                "type": "string"
              },
              "severities": {
                "default": [],
                "items": {
                  "enum": [
                    "DEBUG",
                    "INFO",
                    "WARN",
                    "ERROR",
                    "CRITICAL"
                  ],
                  "type": "string"
                },
                "maxItems": 5,
                "title": "Severities",
                "type": "array"
              },
              "source_owner_epoch": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Source Owner Epoch"
              },
              "start_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Start Ns"
              },
              "start_utc_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Start Utc Ns"
              },
              "stop_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Stop Id"
              }
            },
            "required": [],
            "type": "object"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/debug/export/ros2-medkit",
        "properties": {
          "action": {
            "const": "trace",
            "type": "string"
          },
          "query": {
            "additionalProperties": false,
            "properties": {
              "action_span_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Action Span Id"
              },
              "activation_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Activation Id"
              },
              "alarm_condition_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Alarm Condition Id"
              },
              "alarm_phase": {
                "anyOf": [
                  {
                    "enum": [
                      "all",
                      "activated",
                      "returned",
                      "acknowledged",
                      "raw"
                    ],
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Alarm Phase"
              },
              "before_sequence": {
                "anyOf": [
                  {
                    "maximum": 9007199254740991,
                    "minimum": 1,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Before Sequence"
              },
              "capture_id": {
                "anyOf": [
                  {
                    "maximum": 9007199254740991,
                    "minimum": 1,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Capture Id"
              },
              "channel": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Channel"
              },
              "clock_owner": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Clock Owner"
              },
              "components": {
                "default": [],
                "items": {
                  "type": "string"
                },
                "maxItems": 256,
                "title": "Components",
                "type": "array"
              },
              "condition_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Condition Id"
              },
              "contact_policy_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Contact Policy Id"
              },
              "end_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "End Ns"
              },
              "end_utc_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "End Utc Ns"
              },
              "event_types": {
                "default": [],
                "items": {
                  "type": "string"
                },
                "maxItems": 256,
                "title": "Event Types",
                "type": "array"
              },
              "incident_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Incident Id"
              },
              "intervention_session_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Intervention Session Id"
              },
              "limit": {
                "default": 32,
                "maximum": 32,
                "minimum": 1,
                "title": "Limit",
                "type": "integer"
              },
              "minimum_severity": {
                "anyOf": [
                  {
                    "enum": [
                      "DEBUG",
                      "INFO",
                      "WARN",
                      "ERROR",
                      "CRITICAL"
                    ],
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Minimum Severity"
              },
              "node_attempt_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Node Attempt Id"
              },
              "node_instance_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Node Instance Id"
              },
              "operation_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Operation Id"
              },
              "parent_node_attempt_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Parent Node Attempt Id"
              },
              "phase": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Phase"
              },
              "program_run_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Program Run Id"
              },
              "recent_seconds": {
                "anyOf": [
                  {
                    "maximum": 2678400,
                    "minimum": 1,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Recent Seconds"
              },
              "resource_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Resource Id"
              },
              "run_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Run Id"
              },
              "search": {
                "default": "",
                "maxLength": 256,
                "title": "Search",
                "type": "string"
              },
              "severities": {
                "default": [],
                "items": {
                  "enum": [
                    "DEBUG",
                    "INFO",
                    "WARN",
                    "ERROR",
                    "CRITICAL"
                  ],
                  "type": "string"
                },
                "maxItems": 5,
                "title": "Severities",
                "type": "array"
              },
              "source_owner_epoch": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Source Owner Epoch"
              },
              "start_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Start Ns"
              },
              "start_utc_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Start Utc Ns"
              },
              "stop_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Stop Id"
              }
            },
            "required": [],
            "type": "object"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/execution/history",
        "properties": {
          "action": {
            "const": "history",
            "type": "string"
          },
          "query": {
            "additionalProperties": false,
            "properties": {
              "action_span_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Action Span Id"
              },
              "activation_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Activation Id"
              },
              "alarm_condition_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Alarm Condition Id"
              },
              "alarm_phase": {
                "anyOf": [
                  {
                    "enum": [
                      "all",
                      "activated",
                      "returned",
                      "acknowledged",
                      "raw"
                    ],
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Alarm Phase"
              },
              "before_sequence": {
                "anyOf": [
                  {
                    "maximum": 9007199254740991,
                    "minimum": 1,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Before Sequence"
              },
              "capture_id": {
                "anyOf": [
                  {
                    "maximum": 9007199254740991,
                    "minimum": 1,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Capture Id"
              },
              "channel": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Channel"
              },
              "clock_owner": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Clock Owner"
              },
              "components": {
                "default": [],
                "items": {
                  "type": "string"
                },
                "maxItems": 256,
                "title": "Components",
                "type": "array"
              },
              "condition_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Condition Id"
              },
              "contact_policy_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Contact Policy Id"
              },
              "end_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "End Ns"
              },
              "end_utc_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "End Utc Ns"
              },
              "event_types": {
                "default": [],
                "items": {
                  "type": "string"
                },
                "maxItems": 256,
                "title": "Event Types",
                "type": "array"
              },
              "incident_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Incident Id"
              },
              "intervention_session_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Intervention Session Id"
              },
              "limit": {
                "default": 32,
                "maximum": 32,
                "minimum": 1,
                "title": "Limit",
                "type": "integer"
              },
              "minimum_severity": {
                "anyOf": [
                  {
                    "enum": [
                      "DEBUG",
                      "INFO",
                      "WARN",
                      "ERROR",
                      "CRITICAL"
                    ],
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Minimum Severity"
              },
              "node_attempt_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Node Attempt Id"
              },
              "node_instance_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Node Instance Id"
              },
              "operation_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Operation Id"
              },
              "parent_node_attempt_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Parent Node Attempt Id"
              },
              "phase": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Phase"
              },
              "program_run_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Program Run Id"
              },
              "recent_seconds": {
                "anyOf": [
                  {
                    "maximum": 2678400,
                    "minimum": 1,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Recent Seconds"
              },
              "resource_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Resource Id"
              },
              "search": {
                "default": "",
                "maxLength": 256,
                "title": "Search",
                "type": "string"
              },
              "severities": {
                "default": [],
                "items": {
                  "enum": [
                    "DEBUG",
                    "INFO",
                    "WARN",
                    "ERROR",
                    "CRITICAL"
                  ],
                  "type": "string"
                },
                "maxItems": 5,
                "title": "Severities",
                "type": "array"
              },
              "source_owner_epoch": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Source Owner Epoch"
              },
              "start_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Start Ns"
              },
              "start_utc_ns": {
                "anyOf": [
                  {
                    "maximum": 9223372036854775807,
                    "minimum": 0,
                    "type": "integer"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Start Utc Ns"
              },
              "stop_id": {
                "anyOf": [
                  {
                    "maxLength": 1024,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Stop Id"
              }
            },
            "required": [],
            "type": "object"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/execution/history/{sequence}",
        "properties": {
          "action": {
            "const": "event",
            "type": "string"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "sequence": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "sequence"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/execution/history/health",
        "properties": {
          "action": {
            "const": "health",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/program/diagnostics/health",
        "properties": {
          "action": {
            "const": "program_recorder",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_debug_export"
}
```

## manyforge_catalog

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": false,
    "idempotentHint": true,
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {},
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/catalog",
        "properties": {
          "action": {
            "const": "list",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/catalog/{entry_id}",
        "properties": {
          "action": {
            "const": "entry",
            "type": "string"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "entry_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "entry_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_catalog"
}
```

## manyforge_scene_snapshot

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": false,
    "idempotentHint": true,
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {},
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/scene/snapshot/render",
        "properties": {
          "action": {
            "const": "capture",
            "type": "string"
          },
          "query": {
            "additionalProperties": false,
            "properties": {
              "format": {
                "default": "jpeg",
                "pattern": "^(jpeg|png)$",
                "title": "Format",
                "type": "string"
              },
              "height": {
                "default": 480,
                "maximum": 1080,
                "minimum": 64,
                "title": "Height",
                "type": "integer"
              },
              "timeout_ms": {
                "default": 2000,
                "maximum": 2000,
                "minimum": 1,
                "title": "Timeout Ms",
                "type": "integer"
              },
              "width": {
                "default": 640,
                "maximum": 1920,
                "minimum": 64,
                "title": "Width",
                "type": "integer"
              }
            },
            "required": [],
            "type": "object"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/scene/snapshot/status",
        "properties": {
          "action": {
            "const": "status",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_scene_snapshot"
}
```

## manyforge_program_save

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "SaveDraftAsRequest": {
        "additionalProperties": false,
        "properties": {
          "confirmOverwrite": {
            "default": false,
            "title": "Confirmoverwrite",
            "type": "boolean"
          },
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "path": {
            "maxLength": 4096,
            "minLength": 1,
            "title": "Path",
            "type": "string"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision",
          "path"
        ],
        "title": "SaveDraftAsRequest",
        "type": "object"
      },
      "SaveDraftRequest": {
        "additionalProperties": false,
        "properties": {
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision"
        ],
        "title": "SaveDraftRequest",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "POST /api/program/save",
        "properties": {
          "action": {
            "const": "save",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/SaveDraftRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/save-as",
        "properties": {
          "action": {
            "const": "save_as",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/SaveDraftAsRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_program_save"
}
```

## manyforge_files

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": false,
    "idempotentHint": true,
    "openWorldHint": false,
    "readOnlyHint": true
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {},
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/files/browse",
        "properties": {
          "action": {
            "const": "browse",
            "type": "string"
          },
          "query": {
            "additionalProperties": false,
            "properties": {
              "kind": {
                "default": "program",
                "pattern": "^(program|deployment|mesh)$",
                "title": "Kind",
                "type": "string"
              },
              "path": {
                "anyOf": [
                  {
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Path"
              }
            },
            "required": [],
            "type": "object"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_files"
}
```

## manyforge_deployment

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "ApplyDeploymentCandidateRequest": {
        "additionalProperties": false,
        "properties": {
          "confirmationToken": {
            "anyOf": [
              {
                "maxLength": 71,
                "minLength": 71,
                "pattern": "^sha256:[0-9a-f]{64}$",
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Confirmationtoken"
          },
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "preparationRequestId": {
            "anyOf": [
              {
                "maxLength": 256,
                "minLength": 1,
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Preparationrequestid"
          },
          "preparationRequestOwnerEpoch": {
            "anyOf": [
              {
                "maxLength": 256,
                "minLength": 1,
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Preparationrequestownerepoch"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision"
        ],
        "title": "ApplyDeploymentCandidateRequest",
        "type": "object"
      },
      "CCLCControlBindingRequest": {
        "additionalProperties": false,
        "properties": {
          "control_binding_id": {
            "title": "Control Binding Id",
            "type": "string"
          },
          "control_binding_revision": {
            "title": "Control Binding Revision",
            "type": "string"
          },
          "control_route_id": {
            "title": "Control Route Id",
            "type": "string"
          }
        },
        "required": [
          "control_binding_id",
          "control_binding_revision",
          "control_route_id"
        ],
        "title": "CCLCControlBindingRequest",
        "type": "object"
      },
      "CCLCDesiredControlBindingRequest": {
        "additionalProperties": false,
        "properties": {
          "control_binding_id": {
            "title": "Control Binding Id",
            "type": "string"
          },
          "control_binding_revision": {
            "title": "Control Binding Revision",
            "type": "string"
          },
          "control_route_id": {
            "title": "Control Route Id",
            "type": "string"
          }
        },
        "required": [
          "control_binding_id",
          "control_binding_revision",
          "control_route_id"
        ],
        "title": "CCLCDesiredControlBindingRequest",
        "type": "object"
      },
      "CCLCDesiredControlConfigurationRequest": {
        "additionalProperties": false,
        "properties": {
          "activation_set_id": {
            "title": "Activation Set Id",
            "type": "string"
          },
          "binding_routes": {
            "items": {
              "$ref": "#/$defs/CCLCDesiredControlBindingRequest"
            },
            "maxItems": 4096,
            "minItems": 1,
            "title": "Binding Routes",
            "type": "array"
          },
          "control_domain_id": {
            "title": "Control Domain Id",
            "type": "string"
          },
          "control_domain_revision": {
            "title": "Control Domain Revision",
            "type": "string"
          },
          "controller_topology_id": {
            "title": "Controller Topology Id",
            "type": "string"
          },
          "topology_revision": {
            "title": "Topology Revision",
            "type": "string"
          }
        },
        "required": [
          "control_domain_id",
          "control_domain_revision",
          "controller_topology_id",
          "topology_revision",
          "activation_set_id",
          "binding_routes"
        ],
        "title": "CCLCDesiredControlConfigurationRequest",
        "type": "object"
      },
      "CCLCExpectedActiveConfigurationRequest": {
        "additionalProperties": false,
        "properties": {
          "active_control_configuration_generation": {
            "anyOf": [
              {
                "maximum": 9007199254740991.0,
                "minimum": 1.0,
                "type": "integer"
              },
              {
                "type": "null"
              }
            ],
            "title": "Active Control Configuration Generation"
          },
          "active_control_configuration_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Active Control Configuration Id"
          },
          "presence": {
            "enum": [
              "absent",
              "present"
            ],
            "title": "Presence",
            "type": "string"
          }
        },
        "required": [
          "presence"
        ],
        "title": "CCLCExpectedActiveConfigurationRequest",
        "type": "object"
      },
      "CCLCExpectedControlDomainRequest": {
        "additionalProperties": false,
        "properties": {
          "control_domain_id": {
            "title": "Control Domain Id",
            "type": "string"
          },
          "control_domain_revision": {
            "title": "Control Domain Revision",
            "type": "string"
          },
          "controller_topology_id": {
            "title": "Controller Topology Id",
            "type": "string"
          },
          "expected_active_control_configuration": {
            "$ref": "#/$defs/CCLCExpectedActiveConfigurationRequest"
          },
          "expected_control_bindings": {
            "items": {
              "$ref": "#/$defs/CCLCControlBindingRequest"
            },
            "maxItems": 4096,
            "minItems": 1,
            "title": "Expected Control Bindings",
            "type": "array"
          },
          "topology_revision": {
            "title": "Topology Revision",
            "type": "string"
          }
        },
        "required": [
          "control_domain_id",
          "control_domain_revision",
          "controller_topology_id",
          "topology_revision",
          "expected_active_control_configuration",
          "expected_control_bindings"
        ],
        "title": "CCLCExpectedControlDomainRequest",
        "type": "object"
      },
      "CCLCRouteComponentExpectationRequest": {
        "additionalProperties": false,
        "properties": {
          "controllable_component_id": {
            "title": "Controllable Component Id",
            "type": "string"
          },
          "expected_component_lifecycle_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Component Lifecycle Generation",
            "type": "integer"
          },
          "expected_component_lifecycle_state": {
            "enum": [
              "prepared",
              "active"
            ],
            "title": "Expected Component Lifecycle State",
            "type": "string"
          }
        },
        "required": [
          "controllable_component_id",
          "expected_component_lifecycle_generation",
          "expected_component_lifecycle_state"
        ],
        "title": "CCLCRouteComponentExpectationRequest",
        "type": "object"
      },
      "CallerAttributionRequest": {
        "additionalProperties": false,
        "properties": {
          "correlation_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Correlation Id"
          },
          "request_id": {
            "title": "Request Id",
            "type": "string"
          },
          "request_owner_epoch": {
            "title": "Request Owner Epoch",
            "type": "string"
          },
          "source_label": {
            "title": "Source Label",
            "type": "string"
          }
        },
        "required": [
          "request_id",
          "request_owner_epoch",
          "source_label"
        ],
        "title": "CallerAttributionRequest",
        "type": "object"
      },
      "ComponentLifecycleRequest": {
        "additionalProperties": false,
        "properties": {
          "confirmed": {
            "title": "Confirmed",
            "type": "boolean"
          },
          "controllable_component_id": {
            "title": "Controllable Component Id",
            "type": "string"
          },
          "expected_admission_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Admission Generation",
            "type": "integer"
          },
          "expected_component_lifecycle_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Component Lifecycle Generation",
            "type": "integer"
          },
          "expected_control_domains": {
            "items": {
              "$ref": "#/$defs/CCLCExpectedControlDomainRequest"
            },
            "maxItems": 256,
            "minItems": 1,
            "title": "Expected Control Domains",
            "type": "array"
          },
          "expected_graph_revision": {
            "title": "Expected Graph Revision",
            "type": "string"
          },
          "intent": {
            "enum": [
              "prepare",
              "enable",
              "disable",
              "recover"
            ],
            "title": "Intent",
            "type": "string"
          },
          "program_node_evidence": {
            "anyOf": [
              {
                "$ref": "#/$defs/ProgramNodeAdmissionEvidenceRequest"
              },
              {
                "type": "null"
              }
            ]
          },
          "reason": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Reason"
          },
          "request_context": {
            "$ref": "#/$defs/CallerAttributionRequest"
          }
        },
        "required": [
          "request_context",
          "controllable_component_id",
          "intent",
          "expected_graph_revision",
          "expected_component_lifecycle_generation",
          "expected_control_domains",
          "confirmed",
          "expected_admission_generation"
        ],
        "title": "ComponentLifecycleRequest",
        "type": "object"
      },
      "DiscardDeploymentRequest": {
        "additionalProperties": false,
        "properties": {
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision"
        ],
        "title": "DiscardDeploymentRequest",
        "type": "object"
      },
      "OpenDeploymentRequest": {
        "additionalProperties": false,
        "properties": {
          "confirmDiscard": {
            "default": false,
            "title": "Confirmdiscard",
            "type": "boolean"
          },
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "path": {
            "maxLength": 4096,
            "minLength": 1,
            "title": "Path",
            "type": "string"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision",
          "path"
        ],
        "title": "OpenDeploymentRequest",
        "type": "object"
      },
      "PreviewDeploymentCandidateRequest": {
        "additionalProperties": false,
        "properties": {
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision"
        ],
        "title": "PreviewDeploymentCandidateRequest",
        "type": "object"
      },
      "ProgramNodeAdmissionEvidenceRequest": {
        "additionalProperties": false,
        "description": "Exact runtime-node attribution; the Kernel checks the sealed owner API.",
        "properties": {
          "node_attempt_id": {
            "title": "Node Attempt Id",
            "type": "string"
          },
          "node_instance_id": {
            "title": "Node Instance Id",
            "type": "string"
          },
          "owner_api": {
            "enum": [
              "scene.transaction",
              "scene.carried_attachment"
            ],
            "title": "Owner Api",
            "type": "string"
          },
          "parent_program_cycle_claim_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Parent Program Cycle Claim Generation",
            "type": "integer"
          },
          "program_run_id": {
            "title": "Program Run Id",
            "type": "string"
          },
          "program_runner_owner_epoch": {
            "title": "Program Runner Owner Epoch",
            "type": "string"
          }
        },
        "required": [
          "program_runner_owner_epoch",
          "program_run_id",
          "parent_program_cycle_claim_generation",
          "node_instance_id",
          "node_attempt_id",
          "owner_api"
        ],
        "title": "ProgramNodeAdmissionEvidenceRequest",
        "type": "object"
      },
      "RevertDeploymentRequest": {
        "additionalProperties": false,
        "properties": {
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision"
        ],
        "title": "RevertDeploymentRequest",
        "type": "object"
      },
      "RouteSelectionRequest": {
        "additionalProperties": false,
        "properties": {
          "component_expectations": {
            "items": {
              "$ref": "#/$defs/CCLCRouteComponentExpectationRequest"
            },
            "maxItems": 4096,
            "minItems": 1,
            "title": "Component Expectations",
            "type": "array"
          },
          "confirmed": {
            "title": "Confirmed",
            "type": "boolean"
          },
          "desired_configuration": {
            "$ref": "#/$defs/CCLCDesiredControlConfigurationRequest"
          },
          "expected_active_control_configuration": {
            "$ref": "#/$defs/CCLCExpectedActiveConfigurationRequest"
          },
          "expected_admission_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Admission Generation",
            "type": "integer"
          },
          "expected_graph_revision": {
            "title": "Expected Graph Revision",
            "type": "string"
          },
          "expected_retained_bundle_id": {
            "title": "Expected Retained Bundle Id",
            "type": "string"
          },
          "program_node_evidence": {
            "anyOf": [
              {
                "$ref": "#/$defs/ProgramNodeAdmissionEvidenceRequest"
              },
              {
                "type": "null"
              }
            ]
          },
          "request_context": {
            "$ref": "#/$defs/CallerAttributionRequest"
          }
        },
        "required": [
          "request_context",
          "expected_admission_generation",
          "expected_retained_bundle_id",
          "expected_graph_revision",
          "desired_configuration",
          "expected_active_control_configuration",
          "component_expectations",
          "confirmed"
        ],
        "title": "RouteSelectionRequest",
        "type": "object"
      },
      "SaveDeploymentAsRequest": {
        "additionalProperties": false,
        "properties": {
          "confirmOverwrite": {
            "default": false,
            "title": "Confirmoverwrite",
            "type": "boolean"
          },
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "path": {
            "maxLength": 4096,
            "minLength": 1,
            "title": "Path",
            "type": "string"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision",
          "path"
        ],
        "title": "SaveDeploymentAsRequest",
        "type": "object"
      },
      "SaveDeploymentRequest": {
        "additionalProperties": false,
        "properties": {
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision"
        ],
        "title": "SaveDeploymentRequest",
        "type": "object"
      },
      "UpdateControlledStopRequest": {
        "additionalProperties": false,
        "properties": {
          "command_target_id": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Command Target Id",
            "type": "string"
          },
          "controllable_component_id": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Controllable Component Id",
            "type": "string"
          },
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "profile": {
            "anyOf": [
              {
                "additionalProperties": true,
                "type": "object"
              },
              {
                "type": "null"
              }
            ],
            "title": "Profile"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          },
          "timing": {
            "anyOf": [
              {
                "additionalProperties": true,
                "type": "object"
              },
              {
                "type": "null"
              }
            ],
            "title": "Timing"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision",
          "controllable_component_id",
          "command_target_id",
          "profile",
          "timing"
        ],
        "title": "UpdateControlledStopRequest",
        "type": "object"
      },
      "UpdateDeploymentRequest": {
        "additionalProperties": false,
        "properties": {
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "operation": {
            "default": "set",
            "enum": [
              "set",
              "append",
              "remove"
            ],
            "title": "Operation",
            "type": "string"
          },
          "path": {
            "items": {
              "anyOf": [
                {
                  "type": "string"
                },
                {
                  "type": "integer"
                }
              ]
            },
            "maxItems": 32,
            "minItems": 1,
            "title": "Path",
            "type": "array"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          },
          "value": {
            "title": "Value"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision",
          "path"
        ],
        "title": "UpdateDeploymentRequest",
        "type": "object"
      },
      "UpdateMotionTargetLimitRequest": {
        "additionalProperties": false,
        "properties": {
          "command_target_id": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Command Target Id",
            "type": "string"
          },
          "controllable_component_id": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Controllable Component Id",
            "type": "string"
          },
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "maximum_tcp_linear_speed_mps": {
            "exclusiveMinimum": 0.0,
            "maximum": 1000.0,
            "title": "Maximum Tcp Linear Speed Mps",
            "type": "number"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision",
          "controllable_component_id",
          "command_target_id",
          "maximum_tcp_linear_speed_mps"
        ],
        "title": "UpdateMotionTargetLimitRequest",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/deployment/workspace",
        "properties": {
          "action": {
            "const": "workspace",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/deployment/open-draft",
        "properties": {
          "action": {
            "const": "open_draft",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/OpenDeploymentRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/deployment/draft/mutations",
        "properties": {
          "action": {
            "const": "mutate",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/UpdateDeploymentRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/deployment/draft/controlled-stop",
        "properties": {
          "action": {
            "const": "stop_policy",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/UpdateControlledStopRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/deployment/draft/motion-target-limit",
        "properties": {
          "action": {
            "const": "motion_limit",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/UpdateMotionTargetLimitRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/deployment/draft/preview",
        "properties": {
          "action": {
            "const": "preview",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/PreviewDeploymentCandidateRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/deployment/draft/apply",
        "properties": {
          "action": {
            "const": "apply",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/ApplyDeploymentCandidateRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/deployment/save",
        "properties": {
          "action": {
            "const": "save",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/SaveDeploymentRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/deployment/save-as",
        "properties": {
          "action": {
            "const": "save_as",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/SaveDeploymentAsRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/deployment/discard",
        "properties": {
          "action": {
            "const": "discard",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/DiscardDeploymentRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/deployment/revert",
        "properties": {
          "action": {
            "const": "revert",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/RevertDeploymentRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/manager/status",
        "properties": {
          "action": {
            "const": "manager",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/manager/graph",
        "properties": {
          "action": {
            "const": "graph",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/cclc/lifecycle/requests",
        "properties": {
          "action": {
            "const": "lifecycle",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/ComponentLifecycleRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/cclc/routes/requests",
        "properties": {
          "action": {
            "const": "route",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/RouteSelectionRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_deployment"
}
```

## manyforge_kernel

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "ActionSpanRequest": {
        "additionalProperties": false,
        "properties": {
          "action_source": {
            "enum": [
              "program_node",
              "direct_control",
              "robot_control",
              "mcp",
              "system"
            ],
            "title": "Action Source",
            "type": "string"
          },
          "action_span_id": {
            "title": "Action Span Id",
            "type": "string"
          },
          "node_attempt_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Node Attempt Id"
          },
          "node_instance_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Node Instance Id"
          },
          "parent_node_attempt_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Parent Node Attempt Id"
          },
          "program_run_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Program Run Id"
          }
        },
        "required": [
          "action_source",
          "action_span_id"
        ],
        "title": "ActionSpanRequest",
        "type": "object"
      },
      "AngularEffortRequest": {
        "additionalProperties": false,
        "properties": {
          "kind": {
            "const": "angular_newton_meter",
            "title": "Kind",
            "type": "string"
          },
          "value_nm": {
            "minimum": 0.0,
            "title": "Value Nm",
            "type": "number"
          }
        },
        "required": [
          "kind",
          "value_nm"
        ],
        "title": "AngularEffortRequest",
        "type": "object"
      },
      "AngularPositionRequest": {
        "additionalProperties": false,
        "properties": {
          "kind": {
            "const": "angular_rad",
            "title": "Kind",
            "type": "string"
          },
          "value_rad": {
            "title": "Value Rad",
            "type": "number"
          }
        },
        "required": [
          "kind",
          "value_rad"
        ],
        "title": "AngularPositionRequest",
        "type": "object"
      },
      "CallerAttributionRequest": {
        "additionalProperties": false,
        "properties": {
          "correlation_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Correlation Id"
          },
          "request_id": {
            "title": "Request Id",
            "type": "string"
          },
          "request_owner_epoch": {
            "title": "Request Owner Epoch",
            "type": "string"
          },
          "source_label": {
            "title": "Source Label",
            "type": "string"
          }
        },
        "required": [
          "request_id",
          "request_owner_epoch",
          "source_label"
        ],
        "title": "CallerAttributionRequest",
        "type": "object"
      },
      "CapabilityCommandParticipantRequest": {
        "additionalProperties": false,
        "properties": {
          "caller_options": {
            "items": {
              "$ref": "#/$defs/CommandDurationOptionRequest"
            },
            "maxItems": 64,
            "title": "Caller Options",
            "type": "array"
          },
          "command_schema_id": {
            "title": "Command Schema Id",
            "type": "string"
          },
          "command_schema_revision": {
            "title": "Command Schema Revision",
            "type": "string"
          },
          "command_value": {
            "$ref": "#/$defs/GripperPositionCommandValueRequest"
          },
          "completion_timeout_s": {
            "exclusiveMinimum": 0.0,
            "title": "Completion Timeout S",
            "type": "number"
          },
          "kind": {
            "const": "capability_command",
            "title": "Kind",
            "type": "string"
          }
        },
        "required": [
          "kind",
          "command_schema_id",
          "command_schema_revision",
          "command_value",
          "completion_timeout_s",
          "caller_options"
        ],
        "title": "CapabilityCommandParticipantRequest",
        "type": "object"
      },
      "CartesianPathRequest": {
        "additionalProperties": false,
        "properties": {
          "fallback_policy": {
            "enum": [
              "fail_closed",
              "allow_joint_space_fallback"
            ],
            "title": "Fallback Policy",
            "type": "string"
          },
          "kind": {
            "const": "cartesian_path",
            "title": "Kind",
            "type": "string"
          },
          "max_angular_acceleration_rad_s2": {
            "exclusiveMinimum": 0.0,
            "title": "Max Angular Acceleration Rad S2",
            "type": "number"
          },
          "max_angular_speed_rad_s": {
            "exclusiveMinimum": 0.0,
            "title": "Max Angular Speed Rad S",
            "type": "number"
          },
          "max_blend_deviation_rad": {
            "default": 0.01,
            "minimum": 0.0,
            "title": "Max Blend Deviation Rad",
            "type": "number"
          },
          "max_linear_acceleration_mps2": {
            "exclusiveMinimum": 0.0,
            "title": "Max Linear Acceleration Mps2",
            "type": "number"
          },
          "max_rotation_step_rad": {
            "exclusiveMinimum": 0.0,
            "title": "Max Rotation Step Rad",
            "type": "number"
          },
          "max_translation_step_m": {
            "exclusiveMinimum": 0.0,
            "title": "Max Translation Step M",
            "type": "number"
          },
          "speed_mode": {
            "const": "bounded",
            "title": "Speed Mode",
            "type": "string"
          },
          "waypoints": {
            "items": {
              "$ref": "#/$defs/Pose3Request"
            },
            "maxItems": 4096,
            "minItems": 1,
            "title": "Waypoints",
            "type": "array"
          }
        },
        "required": [
          "kind",
          "waypoints",
          "max_translation_step_m",
          "max_rotation_step_rad",
          "speed_mode",
          "max_angular_speed_rad_s",
          "max_linear_acceleration_mps2",
          "max_angular_acceleration_rad_s2",
          "fallback_policy"
        ],
        "title": "CartesianPathRequest",
        "type": "object"
      },
      "CommandDurationOptionRequest": {
        "additionalProperties": false,
        "properties": {
          "option_id": {
            "title": "Option Id",
            "type": "string"
          },
          "value_s": {
            "minimum": 0.0,
            "title": "Value S",
            "type": "number"
          }
        },
        "required": [
          "option_id",
          "value_s"
        ],
        "title": "CommandDurationOptionRequest",
        "type": "object"
      },
      "ComponentBodyRoleReferenceRequest": {
        "additionalProperties": false,
        "properties": {
          "collision_body_role_id": {
            "title": "Collision Body Role Id",
            "type": "string"
          },
          "type": {
            "const": "component_body_role",
            "title": "Type",
            "type": "string"
          }
        },
        "required": [
          "type",
          "collision_body_role_id"
        ],
        "title": "ComponentBodyRoleReferenceRequest",
        "type": "object"
      },
      "FrameRoleReferenceRequest": {
        "additionalProperties": false,
        "properties": {
          "frame_role_id": {
            "title": "Frame Role Id",
            "type": "string"
          },
          "type": {
            "const": "frame_role",
            "title": "Type",
            "type": "string"
          }
        },
        "required": [
          "type",
          "frame_role_id"
        ],
        "title": "FrameRoleReferenceRequest",
        "type": "object"
      },
      "FullStopIntentRequest": {
        "additionalProperties": false,
        "properties": {
          "expected_stop_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expected Stop Generation",
            "type": "integer"
          },
          "request_context": {
            "$ref": "#/$defs/CallerAttributionRequest"
          }
        },
        "required": [
          "request_context",
          "expected_stop_generation"
        ],
        "title": "FullStopIntentRequest",
        "type": "object"
      },
      "GripperPositionCommandValueRequest": {
        "additionalProperties": false,
        "properties": {
          "kind": {
            "const": "gripper_position",
            "title": "Kind",
            "type": "string"
          },
          "max_effort": {
            "anyOf": [
              {
                "discriminator": {
                  "mapping": {
                    "angular_newton_meter": "#/components/schemas/AngularEffortRequest",
                    "linear_newton": "#/components/schemas/LinearEffortRequest"
                  },
                  "propertyName": "kind"
                },
                "oneOf": [
                  {
                    "$ref": "#/$defs/AngularEffortRequest"
                  },
                  {
                    "$ref": "#/$defs/LinearEffortRequest"
                  }
                ]
              },
              {
                "type": "null"
              }
            ],
            "title": "Max Effort"
          },
          "position": {
            "discriminator": {
              "mapping": {
                "angular_rad": "#/components/schemas/AngularPositionRequest",
                "linear_m": "#/components/schemas/LinearPositionRequest"
              },
              "propertyName": "kind"
            },
            "oneOf": [
              {
                "$ref": "#/$defs/AngularPositionRequest"
              },
              {
                "$ref": "#/$defs/LinearPositionRequest"
              }
            ],
            "title": "Position"
          }
        },
        "required": [
          "kind",
          "position",
          "max_effort"
        ],
        "title": "GripperPositionCommandValueRequest",
        "type": "object"
      },
      "InlinePoseReferenceRequest": {
        "additionalProperties": false,
        "properties": {
          "pose": {
            "$ref": "#/$defs/Pose3Request"
          },
          "type": {
            "const": "pose",
            "title": "Type",
            "type": "string"
          }
        },
        "required": [
          "type",
          "pose"
        ],
        "title": "InlinePoseReferenceRequest",
        "type": "object"
      },
      "JointDynamicsLimitRequest": {
        "additionalProperties": false,
        "properties": {
          "max_acceleration_rad_s2": {
            "anyOf": [
              {
                "exclusiveMinimum": 0.0,
                "type": "number"
              },
              {
                "type": "null"
              }
            ],
            "title": "Max Acceleration Rad S2"
          },
          "max_jerk_rad_s3": {
            "anyOf": [
              {
                "exclusiveMinimum": 0.0,
                "type": "number"
              },
              {
                "type": "null"
              }
            ],
            "title": "Max Jerk Rad S3"
          },
          "max_velocity_rad_s": {
            "anyOf": [
              {
                "exclusiveMinimum": 0.0,
                "type": "number"
              },
              {
                "type": "null"
              }
            ],
            "title": "Max Velocity Rad S"
          }
        },
        "title": "JointDynamicsLimitRequest",
        "type": "object"
      },
      "JointGoalRequest": {
        "additionalProperties": false,
        "properties": {
          "joint_names": {
            "items": {
              "type": "string"
            },
            "maxItems": 4096,
            "minItems": 1,
            "title": "Joint Names",
            "type": "array"
          },
          "kind": {
            "const": "joint_goal",
            "title": "Kind",
            "type": "string"
          },
          "positions_rad": {
            "items": {
              "type": "number"
            },
            "maxItems": 4096,
            "minItems": 1,
            "title": "Positions Rad",
            "type": "array"
          }
        },
        "required": [
          "kind",
          "joint_names",
          "positions_rad"
        ],
        "title": "JointGoalRequest",
        "type": "object"
      },
      "LinearEffortRequest": {
        "additionalProperties": false,
        "properties": {
          "kind": {
            "const": "linear_newton",
            "title": "Kind",
            "type": "string"
          },
          "value_n": {
            "minimum": 0.0,
            "title": "Value N",
            "type": "number"
          }
        },
        "required": [
          "kind",
          "value_n"
        ],
        "title": "LinearEffortRequest",
        "type": "object"
      },
      "LinearPositionRequest": {
        "additionalProperties": false,
        "properties": {
          "kind": {
            "const": "linear_m",
            "title": "Kind",
            "type": "string"
          },
          "value_m": {
            "title": "Value M",
            "type": "number"
          }
        },
        "required": [
          "kind",
          "value_m"
        ],
        "title": "LinearPositionRequest",
        "type": "object"
      },
      "ManualProgramHandoverContext": {
        "additionalProperties": false,
        "description": "Kernel-held Program coordinates, not a client-issued permission.",
        "properties": {
          "claim_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Claim Generation",
            "type": "integer"
          },
          "program_run_id": {
            "title": "Program Run Id",
            "type": "string"
          },
          "program_runner_owner_epoch": {
            "title": "Program Runner Owner Epoch",
            "type": "string"
          },
          "selection_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Selection Revision",
            "type": "integer"
          }
        },
        "required": [
          "program_runner_owner_epoch",
          "program_run_id",
          "claim_generation",
          "selection_revision"
        ],
        "title": "ManualProgramHandoverContext",
        "type": "object"
      },
      "MotionParticipantRequest": {
        "additionalProperties": false,
        "properties": {
          "base_frame_role_id": {
            "title": "Base Frame Role Id",
            "type": "string"
          },
          "cache_mode": {
            "enum": [
              "default",
              "disable"
            ],
            "title": "Cache Mode",
            "type": "string"
          },
          "contact_policy_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Contact Policy Id"
          },
          "kind": {
            "const": "motion",
            "title": "Kind",
            "type": "string"
          },
          "motion": {
            "discriminator": {
              "mapping": {
                "cartesian_path": "#/components/schemas/CartesianPathRequest",
                "joint_goal": "#/components/schemas/JointGoalRequest",
                "pose_goal": "#/components/schemas/PoseGoalRequest"
              },
              "propertyName": "kind"
            },
            "oneOf": [
              {
                "$ref": "#/$defs/JointGoalRequest"
              },
              {
                "$ref": "#/$defs/PoseGoalRequest"
              },
              {
                "$ref": "#/$defs/CartesianPathRequest"
              }
            ],
            "title": "Motion"
          },
          "motion_interrupt_scopes": {
            "items": {
              "type": "null"
            },
            "maxItems": 0,
            "title": "Motion Interrupt Scopes",
            "type": "array"
          },
          "move_id": {
            "title": "Move Id",
            "type": "string"
          },
          "offset_reference": {
            "anyOf": [
              {
                "discriminator": {
                  "mapping": {
                    "component_body_role": "#/components/schemas/ComponentBodyRoleReferenceRequest",
                    "frame_role": "#/components/schemas/FrameRoleReferenceRequest",
                    "pose": "#/components/schemas/InlinePoseReferenceRequest",
                    "runtime_object": "#/components/schemas/RuntimeObjectReferenceRequest",
                    "scene_resource": "#/components/schemas/SceneResourceReferenceRequest"
                  },
                  "propertyName": "type"
                },
                "oneOf": [
                  {
                    "$ref": "#/$defs/FrameRoleReferenceRequest"
                  },
                  {
                    "$ref": "#/$defs/ComponentBodyRoleReferenceRequest"
                  },
                  {
                    "$ref": "#/$defs/SceneResourceReferenceRequest"
                  },
                  {
                    "$ref": "#/$defs/RuntimeObjectReferenceRequest"
                  },
                  {
                    "$ref": "#/$defs/InlinePoseReferenceRequest"
                  }
                ]
              },
              {
                "type": "null"
              }
            ],
            "title": "Offset Reference"
          },
          "per_joint_dynamics_limits": {
            "additionalProperties": {
              "$ref": "#/$defs/JointDynamicsLimitRequest"
            },
            "title": "Per Joint Dynamics Limits",
            "type": "object"
          },
          "planning_profile": {
            "enum": [
              "first_feasible",
              "balanced",
              "quality_bounded"
            ],
            "title": "Planning Profile",
            "type": "string"
          },
          "runtime_object_bindings": {
            "items": {
              "$ref": "#/$defs/RuntimeObjectReferenceBindingRequest"
            },
            "maxItems": 4096,
            "title": "Runtime Object Bindings",
            "type": "array"
          },
          "speed_profile": {
            "$ref": "#/$defs/MotionSpeedProfileRequest"
          },
          "target_mode": {
            "enum": [
              "absolute",
              "relative"
            ],
            "title": "Target Mode",
            "type": "string"
          },
          "target_reference": {
            "anyOf": [
              {
                "discriminator": {
                  "mapping": {
                    "component_body_role": "#/components/schemas/ComponentBodyRoleReferenceRequest",
                    "frame_role": "#/components/schemas/FrameRoleReferenceRequest",
                    "pose": "#/components/schemas/InlinePoseReferenceRequest",
                    "runtime_object": "#/components/schemas/RuntimeObjectReferenceRequest",
                    "scene_resource": "#/components/schemas/SceneResourceReferenceRequest"
                  },
                  "propertyName": "type"
                },
                "oneOf": [
                  {
                    "$ref": "#/$defs/FrameRoleReferenceRequest"
                  },
                  {
                    "$ref": "#/$defs/ComponentBodyRoleReferenceRequest"
                  },
                  {
                    "$ref": "#/$defs/SceneResourceReferenceRequest"
                  },
                  {
                    "$ref": "#/$defs/RuntimeObjectReferenceRequest"
                  },
                  {
                    "$ref": "#/$defs/InlinePoseReferenceRequest"
                  }
                ]
              },
              {
                "type": "null"
              }
            ],
            "title": "Target Reference"
          },
          "tcp_frame_role_id": {
            "title": "Tcp Frame Role Id",
            "type": "string"
          },
          "timeouts": {
            "$ref": "#/$defs/MotionTimeoutsRequest"
          },
          "tolerances": {
            "$ref": "#/$defs/MotionTolerancesRequest"
          }
        },
        "required": [
          "kind",
          "move_id",
          "base_frame_role_id",
          "tcp_frame_role_id",
          "tolerances",
          "speed_profile",
          "timeouts",
          "cache_mode",
          "planning_profile",
          "target_mode",
          "motion",
          "per_joint_dynamics_limits",
          "motion_interrupt_scopes"
        ],
        "title": "MotionParticipantRequest",
        "type": "object"
      },
      "MotionSpeedProfileRequest": {
        "additionalProperties": false,
        "properties": {
          "acceleration_scale": {
            "exclusiveMinimum": 0.0,
            "maximum": 1.0,
            "title": "Acceleration Scale",
            "type": "number"
          },
          "max_tcp_linear_speed_mps": {
            "anyOf": [
              {
                "exclusiveMinimum": 0.0,
                "type": "number"
              },
              {
                "type": "null"
              }
            ],
            "title": "Max Tcp Linear Speed Mps"
          },
          "velocity_scale": {
            "exclusiveMinimum": 0.0,
            "maximum": 1.0,
            "title": "Velocity Scale",
            "type": "number"
          }
        },
        "required": [
          "velocity_scale",
          "acceleration_scale"
        ],
        "title": "MotionSpeedProfileRequest",
        "type": "object"
      },
      "MotionTimeoutsRequest": {
        "additionalProperties": false,
        "properties": {
          "execution_timeout_s": {
            "exclusiveMinimum": 0.0,
            "title": "Execution Timeout S",
            "type": "number"
          },
          "planning_timeout_s": {
            "exclusiveMinimum": 0.0,
            "title": "Planning Timeout S",
            "type": "number"
          }
        },
        "required": [
          "planning_timeout_s",
          "execution_timeout_s"
        ],
        "title": "MotionTimeoutsRequest",
        "type": "object"
      },
      "MotionTolerancesRequest": {
        "additionalProperties": false,
        "properties": {
          "joint_position_tol_rad": {
            "minimum": 0.0,
            "title": "Joint Position Tol Rad",
            "type": "number"
          },
          "rotation_tol_rad": {
            "minimum": 0.0,
            "title": "Rotation Tol Rad",
            "type": "number"
          },
          "translation_tol_m": {
            "minimum": 0.0,
            "title": "Translation Tol M",
            "type": "number"
          }
        },
        "required": [
          "joint_position_tol_rad",
          "translation_tol_m",
          "rotation_tol_rad"
        ],
        "title": "MotionTolerancesRequest",
        "type": "object"
      },
      "OperationControlRequest": {
        "additionalProperties": false,
        "properties": {
          "expected_operation_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Operation Revision",
            "type": "integer"
          },
          "operation_id": {
            "title": "Operation Id",
            "type": "string"
          },
          "reason": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Reason"
          },
          "request_context": {
            "$ref": "#/$defs/CallerAttributionRequest"
          }
        },
        "required": [
          "request_context",
          "operation_id",
          "expected_operation_revision"
        ],
        "title": "OperationControlRequest",
        "type": "object"
      },
      "OperationIntentRequest": {
        "additionalProperties": false,
        "properties": {
          "coordination_policy": {
            "enum": [
              "independent",
              "coordinated_all"
            ],
            "title": "Coordination Policy",
            "type": "string"
          },
          "expected_graph_revision": {
            "title": "Expected Graph Revision",
            "type": "string"
          },
          "expected_retained_bundle_id": {
            "title": "Expected Retained Bundle Id",
            "type": "string"
          },
          "expected_scene_revision": {
            "anyOf": [
              {
                "$ref": "#/$defs/SceneRevisionRequest"
              },
              {
                "type": "null"
              }
            ]
          },
          "manual_handover": {
            "anyOf": [
              {
                "$ref": "#/$defs/ManualProgramHandoverContext"
              },
              {
                "type": "null"
              }
            ]
          },
          "participants": {
            "items": {
              "$ref": "#/$defs/OperationParticipantRequest"
            },
            "maxItems": 4096,
            "minItems": 1,
            "title": "Participants",
            "type": "array"
          },
          "program_binding_id": {
            "title": "Program Binding Id",
            "type": "string"
          },
          "program_id": {
            "title": "Program Id",
            "type": "string"
          },
          "program_revision": {
            "title": "Program Revision",
            "type": "string"
          },
          "program_run_id": {
            "title": "Program Run Id",
            "type": "string"
          },
          "request_context": {
            "$ref": "#/$defs/CallerAttributionRequest"
          }
        },
        "required": [
          "request_context",
          "program_id",
          "program_revision",
          "program_binding_id",
          "program_run_id",
          "expected_graph_revision",
          "expected_retained_bundle_id",
          "expected_scene_revision",
          "participants",
          "coordination_policy"
        ],
        "title": "OperationIntentRequest",
        "type": "object"
      },
      "OperationParticipantRequest": {
        "additionalProperties": false,
        "properties": {
          "action_span": {
            "anyOf": [
              {
                "$ref": "#/$defs/ActionSpanRequest"
              },
              {
                "type": "null"
              }
            ]
          },
          "component_role_id": {
            "title": "Component Role Id",
            "type": "string"
          },
          "payload": {
            "discriminator": {
              "mapping": {
                "capability_command": "#/components/schemas/CapabilityCommandParticipantRequest",
                "motion": "#/components/schemas/MotionParticipantRequest"
              },
              "propertyName": "kind"
            },
            "oneOf": [
              {
                "$ref": "#/$defs/MotionParticipantRequest"
              },
              {
                "$ref": "#/$defs/CapabilityCommandParticipantRequest"
              }
            ],
            "title": "Payload"
          },
          "required_capability_id": {
            "title": "Required Capability Id",
            "type": "string"
          },
          "target_role_id": {
            "title": "Target Role Id",
            "type": "string"
          }
        },
        "required": [
          "component_role_id",
          "target_role_id",
          "required_capability_id",
          "action_span",
          "payload"
        ],
        "title": "OperationParticipantRequest",
        "type": "object"
      },
      "Pose3Request": {
        "additionalProperties": false,
        "properties": {
          "orientation": {
            "$ref": "#/$defs/QuaternionRequest"
          },
          "position_m": {
            "$ref": "#/$defs/Vector3Request"
          }
        },
        "required": [
          "position_m",
          "orientation"
        ],
        "title": "Pose3Request",
        "type": "object"
      },
      "PoseGoalRequest": {
        "additionalProperties": false,
        "properties": {
          "goal": {
            "$ref": "#/$defs/Pose3Request"
          },
          "kind": {
            "const": "pose_goal",
            "title": "Kind",
            "type": "string"
          }
        },
        "required": [
          "kind",
          "goal"
        ],
        "title": "PoseGoalRequest",
        "type": "object"
      },
      "QuaternionRequest": {
        "additionalProperties": false,
        "properties": {
          "w": {
            "title": "W",
            "type": "number"
          },
          "x": {
            "title": "X",
            "type": "number"
          },
          "y": {
            "title": "Y",
            "type": "number"
          },
          "z": {
            "title": "Z",
            "type": "number"
          }
        },
        "required": [
          "x",
          "y",
          "z",
          "w"
        ],
        "title": "QuaternionRequest",
        "type": "object"
      },
      "RuntimeObjectReferenceBindingRequest": {
        "additionalProperties": false,
        "properties": {
          "runtime_object_id": {
            "title": "Runtime Object Id",
            "type": "string"
          },
          "scene_resource_id": {
            "title": "Scene Resource Id",
            "type": "string"
          }
        },
        "required": [
          "runtime_object_id",
          "scene_resource_id"
        ],
        "title": "RuntimeObjectReferenceBindingRequest",
        "type": "object"
      },
      "RuntimeObjectReferenceRequest": {
        "additionalProperties": false,
        "properties": {
          "runtime_object_id": {
            "title": "Runtime Object Id",
            "type": "string"
          },
          "type": {
            "const": "runtime_object",
            "title": "Type",
            "type": "string"
          }
        },
        "required": [
          "type",
          "runtime_object_id"
        ],
        "title": "RuntimeObjectReferenceRequest",
        "type": "object"
      },
      "RuntimeScaleControlRequest": {
        "additionalProperties": false,
        "properties": {
          "expected_control_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expected Control Generation",
            "type": "integer"
          },
          "request_context": {
            "$ref": "#/$defs/CallerAttributionRequest"
          },
          "scale": {
            "maximum": 1.0,
            "minimum": 0.0,
            "title": "Scale",
            "type": "number"
          },
          "selector": {
            "$ref": "#/$defs/RuntimeScaleControlSelectorRequest"
          }
        },
        "required": [
          "request_context",
          "selector",
          "expected_control_generation",
          "scale"
        ],
        "title": "RuntimeScaleControlRequest",
        "type": "object"
      },
      "RuntimeScaleControlSelectorRequest": {
        "additionalProperties": false,
        "properties": {
          "component_role_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Component Role Id"
          },
          "group_targets": {
            "anyOf": [
              {
                "items": {
                  "$ref": "#/$defs/RuntimeScaleControlTargetRequest"
                },
                "maxItems": 4096,
                "minItems": 2,
                "type": "array"
              },
              {
                "type": "null"
              }
            ],
            "title": "Group Targets"
          },
          "program_binding_id": {
            "title": "Program Binding Id",
            "type": "string"
          },
          "target_role_id": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Target Role Id"
          }
        },
        "required": [
          "program_binding_id"
        ],
        "title": "RuntimeScaleControlSelectorRequest",
        "type": "object"
      },
      "RuntimeScaleControlTargetRequest": {
        "additionalProperties": false,
        "properties": {
          "component_role_id": {
            "title": "Component Role Id",
            "type": "string"
          },
          "target_role_id": {
            "title": "Target Role Id",
            "type": "string"
          }
        },
        "required": [
          "component_role_id",
          "target_role_id"
        ],
        "title": "RuntimeScaleControlTargetRequest",
        "type": "object"
      },
      "SceneResourceReferenceRequest": {
        "additionalProperties": false,
        "properties": {
          "scene_resource_id": {
            "title": "Scene Resource Id",
            "type": "string"
          },
          "type": {
            "const": "scene_resource",
            "title": "Type",
            "type": "string"
          }
        },
        "required": [
          "type",
          "scene_resource_id"
        ],
        "title": "SceneResourceReferenceRequest",
        "type": "object"
      },
      "SceneRevisionRequest": {
        "additionalProperties": false,
        "properties": {
          "content_hash": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Content Hash",
            "type": "string"
          },
          "revision_id": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Revision Id",
            "type": "integer"
          },
          "scene_id": {
            "pattern": "^sha256:[0-9a-f]{64}$",
            "title": "Scene Id",
            "type": "string"
          }
        },
        "required": [
          "scene_id",
          "revision_id",
          "content_hash"
        ],
        "title": "SceneRevisionRequest",
        "type": "object"
      },
      "StopComponentTargetRequest": {
        "additionalProperties": false,
        "properties": {
          "controllable_component_id": {
            "title": "Controllable Component Id",
            "type": "string"
          },
          "expected_component_lifecycle_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Component Lifecycle Generation",
            "type": "integer"
          },
          "expected_graph_revision": {
            "title": "Expected Graph Revision",
            "type": "string"
          }
        },
        "required": [
          "controllable_component_id",
          "expected_graph_revision",
          "expected_component_lifecycle_generation"
        ],
        "title": "StopComponentTargetRequest",
        "type": "object"
      },
      "StopIntentRequest": {
        "additionalProperties": false,
        "properties": {
          "components": {
            "anyOf": [
              {
                "items": {
                  "$ref": "#/$defs/StopComponentTargetRequest"
                },
                "maxItems": 4096,
                "minItems": 1,
                "type": "array"
              },
              {
                "type": "null"
              }
            ],
            "title": "Components"
          },
          "operations": {
            "anyOf": [
              {
                "items": {
                  "$ref": "#/$defs/StopOperationTargetRequest"
                },
                "maxItems": 4096,
                "minItems": 1,
                "type": "array"
              },
              {
                "type": "null"
              }
            ],
            "title": "Operations"
          },
          "reason": {
            "anyOf": [
              {
                "type": "string"
              },
              {
                "type": "null"
              }
            ],
            "title": "Reason"
          },
          "request_context": {
            "$ref": "#/$defs/CallerAttributionRequest"
          },
          "resources": {
            "anyOf": [
              {
                "items": {
                  "$ref": "#/$defs/StopResourceTargetRequest"
                },
                "maxItems": 4096,
                "minItems": 1,
                "type": "array"
              },
              {
                "type": "null"
              }
            ],
            "title": "Resources"
          }
        },
        "required": [
          "request_context"
        ],
        "title": "StopIntentRequest",
        "type": "object"
      },
      "StopOperationTargetRequest": {
        "additionalProperties": false,
        "properties": {
          "expected_operation_revision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expected Operation Revision",
            "type": "integer"
          },
          "expected_stop_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expected Stop Generation",
            "type": "integer"
          },
          "operation_id": {
            "title": "Operation Id",
            "type": "string"
          }
        },
        "required": [
          "operation_id",
          "expected_operation_revision",
          "expected_stop_generation"
        ],
        "title": "StopOperationTargetRequest",
        "type": "object"
      },
      "StopResourceTargetRequest": {
        "additionalProperties": false,
        "properties": {
          "expected_graph_revision": {
            "title": "Expected Graph Revision",
            "type": "string"
          },
          "expected_operation_resource_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expected Operation Resource Generation",
            "type": "integer"
          },
          "expected_stop_generation": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expected Stop Generation",
            "type": "integer"
          },
          "resource_id": {
            "title": "Resource Id",
            "type": "string"
          }
        },
        "required": [
          "resource_id",
          "expected_graph_revision",
          "expected_operation_resource_generation",
          "expected_stop_generation"
        ],
        "title": "StopResourceTargetRequest",
        "type": "object"
      },
      "Vector3Request": {
        "additionalProperties": false,
        "properties": {
          "x_m": {
            "title": "X M",
            "type": "number"
          },
          "y_m": {
            "title": "Y M",
            "type": "number"
          },
          "z_m": {
            "title": "Z M",
            "type": "number"
          }
        },
        "required": [
          "x_m",
          "y_m",
          "z_m"
        ],
        "title": "Vector3Request",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/execution/identity",
        "properties": {
          "action": {
            "const": "identity",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/execution/targets",
        "properties": {
          "action": {
            "const": "targets",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/execution/control-state",
        "properties": {
          "action": {
            "const": "state",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/execution/diagnostics",
        "properties": {
          "action": {
            "const": "diagnostics",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/execution/motion-previews",
        "properties": {
          "action": {
            "const": "motion_preview",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/OperationIntentRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/execution/capability-command-previews",
        "properties": {
          "action": {
            "const": "capability_preview",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/OperationIntentRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/execution/operations",
        "properties": {
          "action": {
            "const": "submit",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/OperationIntentRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/execution/operations/{operation_id}",
        "properties": {
          "action": {
            "const": "operation",
            "type": "string"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "operation_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "operation_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/execution/operations/{operation_id}/pause",
        "properties": {
          "action": {
            "const": "pause",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/OperationControlRequest"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "operation_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "operation_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/execution/operations/{operation_id}/resume",
        "properties": {
          "action": {
            "const": "resume",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/OperationControlRequest"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "operation_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "operation_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/execution/operations/{operation_id}/reset",
        "properties": {
          "action": {
            "const": "reset",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/OperationControlRequest"
          },
          "path": {
            "additionalProperties": false,
            "properties": {
              "operation_id": {
                "maxLength": 512,
                "minLength": 1,
                "type": "string"
              }
            },
            "required": [
              "operation_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "path",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/execution/stops",
        "properties": {
          "action": {
            "const": "stop",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/StopIntentRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/execution/full-stop",
        "properties": {
          "action": {
            "const": "full_stop",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/FullStopIntentRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/execution/runtime-scale-control",
        "properties": {
          "action": {
            "const": "scale",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/RuntimeScaleControlRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "GET /api/execution/runtime-scale-control",
        "properties": {
          "action": {
            "const": "read_scale",
            "type": "string"
          },
          "query": {
            "additionalProperties": false,
            "properties": {
              "component_role_id": {
                "anyOf": [
                  {
                    "maxLength": 256,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Component Role Id"
              },
              "group_targets": {
                "anyOf": [
                  {
                    "maxLength": 2097152,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Group Targets"
              },
              "program_binding_id": {
                "maxLength": 256,
                "minLength": 1,
                "title": "Program Binding Id",
                "type": "string"
              },
              "target_role_id": {
                "anyOf": [
                  {
                    "maxLength": 256,
                    "minLength": 1,
                    "type": "string"
                  },
                  {
                    "type": "null"
                  }
                ],
                "title": "Target Role Id"
              }
            },
            "required": [
              "program_binding_id"
            ],
            "type": "object"
          }
        },
        "required": [
          "action",
          "query"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_kernel"
}
```

## manyforge_session

Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.

```json
{
  "annotations": {
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false,
    "readOnlyHint": false
  },
  "description": "Thin ManyForge API adapter. Submit once; observe owner state. Bodies use the unchanged REST DTOs. No implicit Apply, retry, confirmation or identity selection. See each action's schema.",
  "inputSchema": {
    "$defs": {
      "DiscardDraftRequest": {
        "additionalProperties": false,
        "properties": {
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision"
        ],
        "title": "DiscardDraftRequest",
        "type": "object"
      },
      "RevertDraftRequest": {
        "additionalProperties": false,
        "properties": {
          "draftId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Draftid",
            "type": "string"
          },
          "expectedAdmissionGeneration": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedadmissiongeneration",
            "type": "integer"
          },
          "expectedDraftRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 0.0,
            "title": "Expecteddraftrevision",
            "type": "integer"
          },
          "expectedWorkspaceEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Expectedworkspaceepoch",
            "type": "string"
          },
          "expectedWorkspaceRevision": {
            "maximum": 9007199254740991.0,
            "minimum": 1.0,
            "title": "Expectedworkspacerevision",
            "type": "integer"
          },
          "requestId": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestid",
            "type": "string"
          },
          "requestOwnerEpoch": {
            "maxLength": 256,
            "minLength": 1,
            "title": "Requestownerepoch",
            "type": "string"
          }
        },
        "required": [
          "draftId",
          "expectedDraftRevision",
          "expectedAdmissionGeneration",
          "requestOwnerEpoch",
          "requestId",
          "expectedWorkspaceEpoch",
          "expectedWorkspaceRevision"
        ],
        "title": "RevertDraftRequest",
        "type": "object"
      }
    },
    "oneOf": [
      {
        "additionalProperties": false,
        "description": "GET /api/program/workspace",
        "properties": {
          "action": {
            "const": "status",
            "type": "string"
          }
        },
        "required": [
          "action"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/discard",
        "properties": {
          "action": {
            "const": "discard",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/DiscardDraftRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      },
      {
        "additionalProperties": false,
        "description": "POST /api/program/revert",
        "properties": {
          "action": {
            "const": "revert",
            "type": "string"
          },
          "body": {
            "$ref": "#/$defs/RevertDraftRequest"
          }
        },
        "required": [
          "action",
          "body"
        ],
        "type": "object"
      }
    ],
    "type": "object"
  },
  "name": "manyforge_session"
}
```
