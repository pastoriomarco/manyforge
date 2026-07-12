<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd /workspace
    python3 scripts/regen-public-contracts.py
  Last regenerated: 2026-07-12 06:05:07 UTC
  Generator source: scripts/regen-public-contracts.py:gen_mcp_contract
-->

# ManyForge MCP Runtime Contract
ManyForge ships an MCP (Model Context Protocol) server that exposes Composer's program/scene/tree/blackboard tools to LLM agents over a stdio transport. Agents can `tools/list` to discover the surface and `tools/call` to invoke individual tools. The server enforces a deployment-scoped catalog so only mode-allowed tools are reachable.

Tool count: **19**.

## Tools (machine-introspected at regen time)

### `manyforge_capabilities`

Query the deployment-declared capability surface.

Input schema:
```json
{
  "properties": {},
  "title": "manyforge_capabilitiesArguments",
  "type": "object"
}
```

### `manyforge_catalog`

Query the behavior tree node catalog.

Input schema:
```json
{
  "properties": {
    "entry_id": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Entry Id"
    }
  },
  "title": "manyforge_catalogArguments",
  "type": "object"
}
```

### `manyforge_cycle_control`

Control program execution lifecycle.

Input schema:
```json
{
  "properties": {
    "command": {
      "title": "Command",
      "type": "string"
    },
    "velocity_scale": {
      "anyOf": [
        {
          "type": "number"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Velocity Scale"
    },
    "principal_id": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Principal Id"
    }
  },
  "required": [
    "command"
  ],
  "title": "manyforge_cycle_controlArguments",
  "type": "object"
}
```

### `manyforge_debug_export`

Export a full debug/trace bundle for post-mortem analysis.

Input schema:
```json
{
  "properties": {
    "run_id": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Run Id"
    }
  },
  "title": "manyforge_debug_exportArguments",
  "type": "object"
}
```

### `manyforge_deployment`

Get the currently loaded deployment configuration.

Input schema:
```json
{
  "properties": {},
  "title": "manyforge_deploymentArguments",
  "type": "object"
}
```

### `manyforge_execution_status`

Get execution status information.

Input schema:
```json
{
  "properties": {
    "include": {
      "default": "digest",
      "title": "Include",
      "type": "string"
    }
  },
  "title": "manyforge_execution_statusArguments",
  "type": "object"
}
```

### `manyforge_files`

Browse files on disk. Returns directory listing with file metadata.

Input schema:
```json
{
  "properties": {
    "path": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Path"
    },
    "kind": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Kind"
    }
  },
  "title": "manyforge_filesArguments",
  "type": "object"
}
```

### `manyforge_intervention`

Execute, monitor, or abort a temporary intervention subtree.

Input schema:
```json
{
  "properties": {
    "action": {
      "title": "Action",
      "type": "string"
    },
    "tree": {
      "anyOf": [
        {
          "additionalProperties": true,
          "type": "object"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Tree"
    },
    "lease_timeout_ms": {
      "anyOf": [
        {
          "type": "integer"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Lease Timeout Ms"
    },
    "resume_target": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Resume Target"
    },
    "promote_keys": {
      "anyOf": [
        {
          "additionalProperties": {
            "type": "string"
          },
          "type": "object"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Promote Keys"
    },
    "session_kind": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Session Kind"
    },
    "authority_source": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Authority Source"
    },
    "principal_id": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Principal Id"
    },
    "grant_type": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Grant Type"
    },
    "trigger_reason": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Trigger Reason"
    },
    "trigger_node_instance": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Trigger Node Instance"
    },
    "approval_mode": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Approval Mode"
    },
    "artifact_origin": {
      "default": "saved_program",
      "title": "Artifact Origin",
      "type": "string"
    }
  },
  "required": [
    "action"
  ],
  "title": "manyforge_interventionArguments",
  "type": "object"
}
```

### `manyforge_kernel`

Get kernel execution state and metrics.

Input schema:
```json
{
  "properties": {
    "include": {
      "default": "snapshot",
      "title": "Include",
      "type": "string"
    }
  },
  "title": "manyforge_kernelArguments",
  "type": "object"
}
```

### `manyforge_program_load`

Load a program (and optional deployment) from YAML files on disk.

Input schema:
```json
{
  "properties": {
    "program_path": {
      "title": "Program Path",
      "type": "string"
    },
    "deployment_path": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Deployment Path"
    },
    "force_discard_overrides": {
      "default": false,
      "title": "Force Discard Overrides",
      "type": "boolean"
    }
  },
  "required": [
    "program_path"
  ],
  "title": "manyforge_program_loadArguments",
  "type": "object"
}
```

### `manyforge_program_save`

Save the currently loaded program to a YAML file.

Input schema:
```json
{
  "properties": {
    "path": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Path"
    }
  },
  "title": "manyforge_program_saveArguments",
  "type": "object"
}
```

### `manyforge_program_tree`

Read or replace the current behavior tree.

Input schema:
```json
{
  "properties": {
    "action": {
      "default": "read",
      "title": "Action",
      "type": "string"
    },
    "tree": {
      "anyOf": [
        {
          "additionalProperties": true,
          "type": "object"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Tree"
    },
    "force_discard_overrides": {
      "default": false,
      "title": "Force Discard Overrides",
      "type": "boolean"
    }
  },
  "title": "manyforge_program_treeArguments",
  "type": "object"
}
```

### `manyforge_program_validate`

Validate a candidate behavior tree without applying it.

Input schema:
```json
{
  "properties": {
    "tree": {
      "additionalProperties": true,
      "title": "Tree",
      "type": "object"
    }
  },
  "required": [
    "tree"
  ],
  "title": "manyforge_program_validateArguments",
  "type": "object"
}
```

### `manyforge_runtime_override`

Inspect or mutate the runtime override ledger.

Input schema:
```json
{
  "properties": {
    "action": {
      "title": "Action",
      "type": "string"
    },
    "node_name": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Node Name"
    },
    "params": {
      "anyOf": [
        {
          "additionalProperties": true,
          "type": "object"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Params"
    },
    "include_staged": {
      "default": false,
      "title": "Include Staged",
      "type": "boolean"
    },
    "target": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Target"
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
      "default": null,
      "title": "Path"
    },
    "authority_source": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Authority Source"
    },
    "approval_mode": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Approval Mode"
    },
    "reason": {
      "default": "",
      "title": "Reason",
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
      "default": null,
      "title": "Session Id"
    },
    "trigger_node_instance": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Trigger Node Instance"
    }
  },
  "required": [
    "action"
  ],
  "title": "manyforge_runtime_overrideArguments",
  "type": "object"
}
```

### `manyforge_scene_edit`

Edit the collision scene at runtime.

Input schema:
```json
{
  "properties": {
    "operation": {
      "title": "Operation",
      "type": "string"
    },
    "object_id": {
      "title": "Object Id",
      "type": "string"
    },
    "principal_id": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Principal Id"
    },
    "shape_type": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Shape Type"
    },
    "box_dimensions_m": {
      "anyOf": [
        {
          "items": {
            "type": "number"
          },
          "type": "array"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Box Dimensions M"
    },
    "sphere_radius_m": {
      "anyOf": [
        {
          "type": "number"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Sphere Radius M"
    },
    "cylinder_radius_m": {
      "anyOf": [
        {
          "type": "number"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Cylinder Radius M"
    },
    "cylinder_height_m": {
      "anyOf": [
        {
          "type": "number"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Cylinder Height M"
    },
    "mesh_resource_uri": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Mesh Resource Uri"
    },
    "mesh_scale": {
      "anyOf": [
        {
          "items": {
            "type": "number"
          },
          "type": "array"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Mesh Scale"
    },
    "pose_position_m": {
      "anyOf": [
        {
          "items": {
            "type": "number"
          },
          "type": "array"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Pose Position M"
    },
    "pose_orientation_xyzw": {
      "anyOf": [
        {
          "items": {
            "type": "number"
          },
          "type": "array"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Pose Orientation Xyzw"
    },
    "frame_id": {
      "anyOf": [
        {
          "type": "string"
        },
        {
          "type": "null"
        }
      ],
      "default": null,
      "title": "Frame Id"
    }
  },
  "required": [
    "operation",
    "object_id"
  ],
  "title": "manyforge_scene_editArguments",
  "type": "object"
}
```

### `manyforge_scene_snapshot`

Render the 3D scene as a base64-encoded image.

Input schema:
```json
{
  "properties": {
    "width": {
      "default": 640,
      "title": "Width",
      "type": "integer"
    },
    "height": {
      "default": 480,
      "title": "Height",
      "type": "integer"
    },
    "format": {
      "default": "jpeg",
      "title": "Format",
      "type": "string"
    }
  },
  "title": "manyforge_scene_snapshotArguments",
  "type": "object"
}
```

### `manyforge_scene_state`

Get the full scene state including robot joint positions, TCP pose, and all collision objects.

Input schema:
```json
{
  "properties": {},
  "title": "manyforge_scene_stateArguments",
  "type": "object"
}
```

### `manyforge_session`

Manage the composer session state.

Input schema:
```json
{
  "properties": {
    "action": {
      "default": "status",
      "title": "Action",
      "type": "string"
    }
  },
  "title": "manyforge_sessionArguments",
  "type": "object"
}
```

### `manyforge_skills`

Get the skill catalog — validated subtree templates for common operations.

Input schema:
```json
{
  "properties": {},
  "title": "manyforge_skillsArguments",
  "type": "object"
}
```

## Code source

All tools are registered with `@mcp.tool()` decorators in `manyforge_composer/backend/mcp_server.py`. The decorator captures the tool's name, description (from the function's docstring), and JSON-Schema input shape from the function's type hints.
