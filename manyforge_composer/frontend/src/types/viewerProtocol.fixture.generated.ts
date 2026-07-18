// THIS FILE IS GENERATED. DO NOT EDIT.
// Source: scripts/regen-public-contracts.py:gen_viewer_typescript_fixture

import type { ViewerFrameV1, ViewerManifestV1 } from './viewerProtocol.generated';

export const viewerProtocolFixture = {
  "expected_legacy": {
    "frames": {
      "frameGizmoSizeM": 0.08,
      "frames": [
        {
          "name": "universe",
          "poseAvailable": true,
          "poseInWorld": {
            "orientation_xyzw": [
              0.0,
              0.0,
              0.0,
              1.0
            ],
            "position_m": [
              0.0,
              0.0,
              0.0
            ]
          },
          "roles": [
            "world"
          ],
          "viewerVisible": true
        },
        {
          "name": "tool0",
          "poseAvailable": true,
          "poseInWorld": {
            "orientation_xyzw": [
              0.0,
              0.0,
              0.7071067811865476,
              0.7071067811865476
            ],
            "position_m": [
              0.4,
              -0.2,
              0.7
            ]
          },
          "roles": [
            "tcp"
          ],
          "viewerVisible": true
        }
      ],
      "timestampNs": 12345678900,
      "viewerAvailable": true,
      "visualStateDiagnostics": {
        "mode": "viewer_data_plane_v1",
        "served_from_visual_snapshot": true,
        "slow_steps": {},
        "snapshot_sequence_number": 31,
        "snapshot_stale": false,
        "timings_ms": {},
        "total_ms": 0
      }
    },
    "scene": {
      "grippers": [
        {
          "actuation_kind": "position",
          "command_id": 4,
          "commanded_position": 0.8,
          "commanded_state": "closed",
          "completion_policy": "time_and_observation",
          "current_position": 0.4,
          "derived_joints": [
            "left_finger",
            "right_finger"
          ],
          "error": null,
          "evidence": [
            "command_accepted"
          ],
          "evidence_state": "pending",
          "generation": 9,
          "grasp_confirmation_policy": "position",
          "gripper_id": "gripper-a",
          "joint_name": "finger_joint",
          "limit_lower": 0.0,
          "limit_upper": 0.8,
          "motion_time_s": 0.5,
          "observed_position": 0.39,
          "projection_reason": "",
          "projection_state": "available",
          "reason_code": null,
          "revision": 11,
          "srdf_presets": {
            "closed": 0.8,
            "open": 0.0
          },
          "state": "executing",
          "state_authority": "backend",
          "target_position": 0.8,
          "transport_kind": "native",
          "visualization": null
        }
      ],
      "link_poses": {
        "base_link": {
          "orientation_xyzw": [
            0.0,
            0.0,
            0.0,
            1.0
          ],
          "position_m": [
            0.0,
            0.0,
            0.0
          ]
        },
        "tool0": {
          "orientation_xyzw": [
            0.0,
            0.0,
            0.7071067811865476,
            0.7071067811865476
          ],
          "position_m": [
            0.4,
            -0.2,
            0.7
          ]
        }
      },
      "object_axes_gizmo_size_m": 0.12,
      "objects": [
        {
          "attached": false,
          "attached_to": null,
          "object_id": "fixture-box",
          "persistable": true,
          "pose_in_universe": {
            "orientation_xyzw": [
              0.0,
              0.0,
              0.0,
              1.0
            ],
            "position_m": [
              0.7,
              0.1,
              0.2
            ]
          },
          "shape": {
            "box_dimensions_m": [
              0.2,
              0.3,
              0.4
            ],
            "mesh_asset_status": null,
            "mesh_asset_url": null,
            "type": "box"
          },
          "source": "authored",
          "support_contacts": [],
          "viewer_frame_visible": true
        },
        {
          "attached": true,
          "attached_to": "tool0",
          "object_id": "fixture-tool",
          "persistable": false,
          "pose_in_universe": {
            "orientation_xyzw": [
              0.0,
              0.0,
              0.7071067811865476,
              0.7071067811865476
            ],
            "position_m": [
              0.4,
              -0.2,
              0.8
            ]
          },
          "shape": {
            "mesh_asset_status": "ready",
            "mesh_asset_url": "/api/scene/meshes/fixture-tool",
            "mesh_resource_uri": "package://fixture/tool.stl",
            "mesh_scale": [
              1.0,
              1.0,
              1.0
            ],
            "type": "mesh"
          },
          "source": "attached",
          "support_contacts": [
            {
              "link": "tool0"
            }
          ],
          "viewer_frame_visible": false
        }
      ],
      "pose_sources": [
        {
          "description": "fixture source",
          "enabled": true,
          "frame_id": "universe",
          "kind": "foundation_pose",
          "max_update_hz": 30.0,
          "measured_robot": null,
          "measured_tcp": null,
          "objects": [
            {
              "age_s": 0.01,
              "apply_policy": "continuous",
              "attached_drift": null,
              "entity": "fixture-box",
              "frame_id": "universe",
              "measured_state_ghost": {
                "enabled": true,
                "object": true
              },
              "object_id": "fixture-box",
              "rejection": null,
              "scene_admission": {
                "mode": "snapshot"
              },
              "status": "fresh",
              "update": {
                "apply_policy": "continuous",
                "entity": "fixture-box",
                "monotonic_s": 12.345,
                "object_id": "fixture-box",
                "pose": {
                  "frame_id": "universe",
                  "orientation_xyzw": [
                    0.0,
                    0.0,
                    0.0,
                    1.0
                  ],
                  "position_xyz": [
                    0.7,
                    0.1,
                    0.2
                  ],
                  "stamp_sec": 12.3
                },
                "pose_source_id": "camera-a"
              }
            }
          ],
          "pose_source_id": "camera-a",
          "received_at_ns": 12345670000,
          "stale_after_s": 0.5,
          "status": "fresh"
        }
      ],
      "robot": {
        "base_frame": "base_link",
        "base_pose": {
          "frame_id": "universe",
          "orientation_xyzw": [
            0.0,
            0.0,
            0.0,
            1.0
          ],
          "position_m": [
            0.0,
            0.0,
            0.0
          ]
        },
        "group_id": "arm",
        "joint_names": [
          "joint_1",
          "joint_2"
        ],
        "joint_positions": {
          "joint_1": 0.25,
          "joint_2": -0.5
        },
        "link_order": [
          "base_link",
          "tool0"
        ],
        "robot_id": "robot-a",
        "source_age_ns": 8900,
        "source_timestamp_ns": 12345670000,
        "state_source": "composer_backend",
        "tcp_frame": "tool0",
        "tcp_pose_in_base": {
          "orientation_xyzw": [
            0.0,
            0.0,
            0.7071067811865476,
            0.7071067811865476
          ],
          "position_m": [
            0.4,
            -0.2,
            0.7
          ]
        },
        "urdf_model_key": "fixture-model-key"
      },
      "timestamp_ns": 12345678900,
      "visual_state_diagnostics": {
        "mode": "viewer_data_plane_v1",
        "served_from_visual_snapshot": true,
        "slow_steps": {},
        "snapshot_sequence_number": 31,
        "snapshot_stale": false,
        "timings_ms": {},
        "total_ms": 0
      }
    }
  },
  "frame": {
    "entity_transforms": [
      {
        "orientation_xyzw": [
          0.0,
          0.0,
          0.0,
          1.0
        ],
        "position_m": [
          0.7,
          0.1,
          0.2
        ]
      },
      {
        "orientation_xyzw": [
          0.0,
          0.0,
          0.7071067811865476,
          0.7071067811865476
        ],
        "position_m": [
          0.4,
          -0.2,
          0.8
        ]
      }
    ],
    "frame_sequence": 19,
    "frame_transforms": [
      {
        "orientation_xyzw": [
          0.0,
          0.0,
          0.0,
          1.0
        ],
        "position_m": [
          0.0,
          0.0,
          0.0
        ]
      },
      {
        "orientation_xyzw": [
          0.0,
          0.0,
          0.7071067811865476,
          0.7071067811865476
        ],
        "position_m": [
          0.4,
          -0.2,
          0.7
        ]
      }
    ],
    "gripper_states": [
      {
        "actuation_kind": "position",
        "command_id": 4,
        "commanded_position": 0.8,
        "commanded_state": "closed",
        "completion_policy": "time_and_observation",
        "current_position": 0.4,
        "derived_joints": [
          "left_finger",
          "right_finger"
        ],
        "error": null,
        "evidence": [
          "command_accepted"
        ],
        "evidence_state": "pending",
        "generation": 9,
        "grasp_confirmation_policy": "position",
        "gripper_id": "gripper-a",
        "joint_name": "finger_joint",
        "limit_lower": 0.0,
        "limit_upper": 0.8,
        "motion_time_s": 0.5,
        "observed_position": 0.39,
        "projection_reason": "",
        "projection_state": "available",
        "reason_code": null,
        "revision": 11,
        "srdf_presets": {
          "closed": 0.8,
          "open": 0.0
        },
        "state": "executing",
        "state_authority": "backend",
        "target_position": 0.8,
        "transport_kind": "native",
        "visualization": null
      }
    ],
    "manifest_revision": 7,
    "pose_source_states": [
      {
        "measured_robot": null,
        "measured_tcp": null,
        "objects": [
          {
            "age_s": 0.01,
            "attached_drift": null,
            "object_id": "fixture-box",
            "rejection": null,
            "status": "fresh",
            "update": {
              "apply_policy": "continuous",
              "entity": "fixture-box",
              "monotonic_s": 12.345,
              "object_id": "fixture-box",
              "pose": {
                "frame_id": "universe",
                "orientation_xyzw": [
                  0.0,
                  0.0,
                  0.0,
                  1.0
                ],
                "position_xyz": [
                  0.7,
                  0.1,
                  0.2
                ],
                "stamp_sec": 12.3
              },
              "pose_source_id": "camera-a"
            }
          }
        ],
        "pose_source_id": "camera-a",
        "received_at_ns": 12345670000,
        "status": "fresh"
      }
    ],
    "producer_epoch": "fixture-epoch-0001",
    "render_revision": 13,
    "robot_blocks": [
      {
        "joint_positions": [
          0.25,
          -0.5
        ],
        "link_transforms": [
          {
            "orientation_xyzw": [
              0.0,
              0.0,
              0.0,
              1.0
            ],
            "position_m": [
              0.0,
              0.0,
              0.0
            ]
          },
          {
            "orientation_xyzw": [
              0.0,
              0.0,
              0.7071067811865476,
              0.7071067811865476
            ],
            "position_m": [
              0.4,
              -0.2,
              0.7
            ]
          }
        ],
        "robot_id": "robot-a",
        "source_age_ns": 8900,
        "source_timestamp_ns": 12345670000,
        "state_source": "composer_backend",
        "tcp_pose_in_base": {
          "orientation_xyzw": [
            0.0,
            0.0,
            0.7071067811865476,
            0.7071067811865476
          ],
          "position_m": [
            0.4,
            -0.2,
            0.7
          ]
        }
      }
    ],
    "schema_version": "manyforge.viewer.frame.v1",
    "source_sequence": 31,
    "stale": false,
    "timestamp_ns": 12345678900,
    "type": "frame"
  },
  "manifest": {
    "capabilities": {
      "measured_objects": true,
      "measured_robot": true,
      "measured_tcp": true,
      "multi_robot_shape": true,
      "point_cloud_channel": true,
      "snapshot_capture": true
    },
    "content_hash": "bcaa4276077e6b18ecf08aed4836e5a3a6b734c741372781e2a2cfdee415a289",
    "entities": [
      {
        "attached": false,
        "attached_to": null,
        "object_id": "fixture-box",
        "persistable": true,
        "shape": {
          "box_dimensions_m": [
            0.2,
            0.3,
            0.4
          ],
          "cylinder_height_m": null,
          "cylinder_radius_m": null,
          "mesh_asset_status": null,
          "mesh_asset_url": null,
          "mesh_resource_uri": null,
          "mesh_scale": null,
          "sphere_radius_m": null,
          "type": "box"
        },
        "source": "authored",
        "support_contacts": [],
        "viewer_frame_visible": true
      },
      {
        "attached": true,
        "attached_to": "tool0",
        "object_id": "fixture-tool",
        "persistable": false,
        "shape": {
          "box_dimensions_m": null,
          "cylinder_height_m": null,
          "cylinder_radius_m": null,
          "mesh_asset_status": "ready",
          "mesh_asset_url": "/api/scene/meshes/fixture-tool",
          "mesh_resource_uri": "package://fixture/tool.stl",
          "mesh_scale": [
            1.0,
            1.0,
            1.0
          ],
          "sphere_radius_m": null,
          "type": "mesh"
        },
        "source": "attached",
        "support_contacts": [
          {
            "link": "tool0"
          }
        ],
        "viewer_frame_visible": false
      }
    ],
    "frame_axes_gizmo_size_m": 0.08,
    "frames": [
      {
        "name": "universe",
        "roles": [
          "world"
        ],
        "viewer_visible": true
      },
      {
        "name": "tool0",
        "roles": [
          "tcp"
        ],
        "viewer_visible": true
      }
    ],
    "grippers": [
      {
        "actuation_kind": "position",
        "completion_policy": "time_and_observation",
        "derived_joints": [
          "left_finger",
          "right_finger"
        ],
        "grasp_confirmation_policy": "position",
        "gripper_id": "gripper-a",
        "joint_name": "finger_joint",
        "limit_lower": 0.0,
        "limit_upper": 0.8,
        "motion_time_s": 0.5,
        "srdf_presets": {
          "closed": 0.8,
          "open": 0.0
        },
        "state_authority": "backend",
        "transport_kind": "native",
        "visualization": null
      }
    ],
    "manifest_revision": 7,
    "object_axes_gizmo_size_m": 0.12,
    "orders": {
      "entities": [
        "fixture-box",
        "fixture-tool"
      ],
      "frames": [
        "universe",
        "tool0"
      ],
      "grippers": [
        "gripper-a"
      ],
      "links": [
        "base_link",
        "tool0"
      ],
      "pose_sources": [
        "camera-a"
      ]
    },
    "pose_sources": [
      {
        "description": "fixture source",
        "enabled": true,
        "frame_id": "universe",
        "kind": "foundation_pose",
        "max_update_hz": 30.0,
        "objects": [
          {
            "apply_policy": "continuous",
            "entity": "fixture-box",
            "frame_id": "universe",
            "measured_state_ghost": {
              "enabled": true,
              "object": true
            },
            "object_id": "fixture-box",
            "scene_admission": {
              "mode": "snapshot"
            }
          }
        ],
        "pose_source_id": "camera-a",
        "stale_after_s": 0.5
      }
    ],
    "producer_epoch": "fixture-epoch-0001",
    "robots": [
      {
        "base_frame": "base_link",
        "base_pose": {
          "frame_id": "universe",
          "orientation_xyzw": [
            0.0,
            0.0,
            0.0,
            1.0
          ],
          "position_m": [
            0.0,
            0.0,
            0.0
          ]
        },
        "group_id": "arm",
        "joint_names": [
          "joint_1",
          "joint_2"
        ],
        "link_order": [
          "base_link",
          "tool0"
        ],
        "robot_id": "robot-a",
        "tcp_frame": "tool0",
        "urdf_model_key": "fixture-model-key"
      }
    ],
    "schema_version": "manyforge.viewer.manifest.v1",
    "type": "manifest",
    "world_frame": "universe"
  },
  "schema_version": "manyforge.viewer.preservation_fixture.v1"
} satisfies {
  schema_version: 'manyforge.viewer.preservation_fixture.v1';
  manifest: ViewerManifestV1;
  frame: ViewerFrameV1;
  expected_legacy: unknown;
};
