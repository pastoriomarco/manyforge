import { describe, expect, it } from 'vitest';

import type {
  NodeEnvironmentResponse,
  SceneUrdfManifest,
  SourceModelProjection,
} from '../types/api';
import type { HeadlessRuntimeSnapshot, HeadlessSourceSection } from '../types/headless';
import {
  attachedScenePresentation,
  runtimeScenePresentation,
} from './runtimeScenePresentation';
import { sceneOwnerFacts } from './sceneAuthoring';

// Night change 9: live Isaac UR10e light scene (merge 6b), trimmed. The cheese
// box is carried by the robotiq_2f_140 tool mounted on the UR10e arm; the
// small KLT stays cell-fixed. Before the fix the gripper-relative row (policy
// "scene") was rejected, the scene owner facts went unavailable and every
// mesh object vanished from the viewer while the box was attached.
const CELL = 'c1_build_authority_cell';
const BUNDLE = 'sha256:9a6355d1b1ee8f2b79f8176739e267634fccf770aecf4dc3a4c63f9552911c9d';
const GRAPH = 'sha256:5b83362c6824f7f99d345196cd2cbfe591a476049df87438629ecc95f25fce95';
const ARM_MODEL_REVISION = 'sha256:4d5eac92cf13634032aa1a55621458c0d5b429038c5cbcdec60abc1fc73dc344';
const TOOL_MODEL_REVISION = 'sha256:0eddbd7507080aa8bd3c2fcb71663efd00f346cd535d642b9bc78a54329f8562';
const BASE_LINK = 'mf1_link_d1e7fbc98bf3e16e7f995f230627f6415449c6fde92b92489856e0b52d659c0d';
const TOOL0_LINK = 'mf1_link_b38adee212db65935dc7358d86b459c57897a27664f7bbbe8d86661642591f89';
const GRASP_LINK = 'mf1_link_f56ff2ccbdbdfeef34fd7b8bbf518a1e7542717a12ec4cdf9af87580f3147263';

const LOGICAL_SCENE_PAYLOAD: Record<string, unknown> = {
  "access": {
    "active_projection": {
      "graph_revision": "sha256:5b83362c6824f7f99d345196cd2cbfe591a476049df87438629ecc95f25fce95",
      "retained_bundle_id": "sha256:9a6355d1b1ee8f2b79f8176739e267634fccf770aecf4dc3a4c63f9552911c9d",
      "scene_revision": {
        "content_hash": "sha256:21eaaf251bd6a3ae05b7933a56c94b2ddfee9603176b1a4e6d3f00a67dbfa876",
        "revision_id": 461,
        "scene_id": "sha256:23704f0e4cc1d54f477fa692f66618d30577ddc2285aa3a0c95dbf2bc37f2a69"
      }
    },
    "phase": "maintenance_completed",
    "projection_leases_open": true,
    "scene_writes_open": true
  },
  "api_version": "manyforge.logical_scene_source.v1",
  "snapshot": {
    "carried_object_attachments": [
      {
        "carried_object_attachment_id": "carried-object:3c1d21b77b427cde02d138e3afd33b59c46421e0f5f9a185ae7c458d780a1b4e",
        "carried_object_attachment_revision": "sha256:38875cf9e9460afed9c685b1acb7b5269eff94ba33b384a11746116ca94f3872",
        "carrier_frame_ref": {
          "controllable_component_id": "robotiq_2f_140",
          "frame_role_id": "tcp",
          "kind": "component",
          "model_instance_id": "robotiq_2f_140.model"
        },
        "command_target_id": "gripper",
        "controllable_component_id": "robotiq_2f_140",
        "pose_in_carrier_frame": {
          "orientation_xyzw": [
            -0.7083416976378876,
            -0.007400778809739253,
            0.015716129015836265,
            0.705655915548961
          ],
          "position_m": {
            "x": -0.008478398524457304,
            "y": 0.0030433953970704,
            "z": 0.05605514822920987
          }
        },
        "scene_resource_id": "isaac_cheese_box",
        "touch_body_roles": [
          "gripper_collision"
        ]
      }
    ],
    "cell_id": "c1_build_authority_cell",
    "configured_collision_policy": {},
    "objects": [
      {
        "collision_shapes": [
          {
            "resource_uri": "examples/isaac_manipulator_scene_light/meshes/mac_n_cheese_centered_collider_cooked.obj",
            "scale": {
              "x": 1,
              "y": 1,
              "z": 1
            },
            "type": "mesh"
          }
        ],
        "placement": {
          "carried_object_attachment_id": "carried-object:3c1d21b77b427cde02d138e3afd33b59c46421e0f5f9a185ae7c458d780a1b4e",
          "carried_object_attachment_revision": "sha256:38875cf9e9460afed9c685b1acb7b5269eff94ba33b384a11746116ca94f3872",
          "kind": "carried"
        },
        "provider_evidence": {
          "provider_instance_id": "c1_build_authority_cell.logical_scene",
          "provider_manifest_id": "manyforge.provider.logical_scene",
          "provider_manifest_revision": "sha256:352903171f0341c0f5c018fd2a363eb7b3a69ecdb3180dbc41772ad86908ec60",
          "provider_source_epoch": "sha256:33266401dce90e4524148c09b3ed1874dfe934b90e1b1e9cac819e87d68351f7"
        },
        "scene_resource_id": "isaac_cheese_box",
        "support_contacts": []
      },
      {
        "collision_shapes": [
          {
            "resource_uri": "examples/isaac_manipulator_scene_light/meshes/World__small_KLT_collider_primitives.obj",
            "scale": {
              "x": 1,
              "y": 1,
              "z": 1
            },
            "type": "mesh"
          }
        ],
        "placement": {
          "cell_id": "c1_build_authority_cell",
          "kind": "cell_fixed",
          "pose_in_cell": {
            "orientation_xyzw": [
              -7.463636797056011e-07,
              0.0014223730063974037,
              5.845196084198735e-05,
              0.9999989867184076
            ],
            "position_m": {
              "x": -0.561839,
              "y": -0.367252,
              "z": 0.073459
            }
          }
        },
        "provider_evidence": null,
        "scene_resource_id": "isaac_small_klt",
        "support_contacts": []
      }
    ],
    "policy_epoch": 5,
    "revision": {
      "content_hash": "sha256:21eaaf251bd6a3ae05b7933a56c94b2ddfee9603176b1a4e6d3f00a67dbfa876",
      "revision_id": 461,
      "scene_id": "sha256:23704f0e4cc1d54f477fa692f66618d30577ddc2285aa3a0c95dbf2bc37f2a69"
    },
    "topology_epoch": 460,
    "transaction_sequence": 460
  }
};

const SCENE_RUNTIME_PAYLOAD: Record<string, unknown> = {
  "api_version": "manyforge.scene_runtime_source.v1",
  "cell_id": "c1_build_authority_cell",
  "state": {
    "active_replay_id": null,
    "api_version": "manyforge.scene_runtime_state.v2",
    "counters": {
      "buffer_replaced": 0,
      "buffered": 0,
      "closed": 0,
      "failed": 0,
      "opened": 0,
      "replayed": 0,
      "retired": 0,
      "stale_completion_rejected": 0
    },
    "deferred": false,
    "error": null,
    "initialization_generation": 4,
    "initialization_token": null,
    "pose_sources": [
      {
        "baseline_offset": {
          "orientation_xyzw": [
            -0.7087738044392737,
            -0.008892150472314335,
            0.017758836133112917,
            0.7051561866280782
          ],
          "position_m": {
            "x": -0.008446422573802681,
            "y": 0.0032902486306193805,
            "z": 0.056291417538677224
          }
        },
        "buffered": false,
        "current_offset": {
          "orientation_xyzw": [
            -0.7083416976378876,
            -0.007400778809739253,
            0.015716129015836265,
            0.705655915548961
          ],
          "position_m": {
            "x": -0.008478398524457304,
            "y": 0.0030433953970704,
            "z": 0.05605514822920987
          }
        },
        "drift": {
          "rotation_rad": 0.005228112843547426,
          "translation_m": 0.00034319406598895855
        },
        "eligible": true,
        "expires_at_ns": 426292300693403,
        "frame": {
          "controllable_component_id": "robotiq_2f_140",
          "frame_role_id": "tcp",
          "kind": "component",
          "model_instance_id": "robotiq_2f_140.model"
        },
        "fresh": true,
        "holder": {
          "command_target_id": "gripper",
          "controllable_component_id": "robotiq_2f_140",
          "gripper_link": "robotiq_base_link"
        },
        "measured_against": {
          "entity": "/World/ur10e_robotiq2f_140_ROS/ur10e_robotiq2f_140/ee_link/robotiq_arg2f_base_link",
          "link": "robotiq_base_link"
        },
        "object_id": "isaac_cheese_box",
        "observed_at_ns": 426290300693403,
        "payload": {
          "api_version": "manyforge.ros_pose_observation.v2",
          "kind": "simulation_entity_pose_with_references.v1",
          "pose": {
            "frame_id": "universe",
            "frame_kind": "cell_frame",
            "orientation_xyzw": [
              0.4981848870482472,
              -0.5290352359042739,
              -0.48623883551013997,
              0.4852889163479602
            ],
            "position_m": [
              -0.49346286058425903,
              -0.02209845557808876,
              0.32439935207366943
            ],
            "source_stamp_ns": 0
          },
          "references": [
            {
              "coherent": true,
              "command_target_id": "gripper",
              "controllable_component_id": "robotiq_2f_140",
              "entity": "/World/ur10e_robotiq2f_140_ROS/ur10e_robotiq2f_140/ee_link/robotiq_arg2f_base_link",
              "gripper_link": "robotiq_base_link",
              "link": "robotiq_base_link",
              "pose_in_reference": {
                "orientation_xyzw": [
                  -0.7083416976378876,
                  -0.007400778809739255,
                  0.015716129015836265,
                  0.705655915548961
                ],
                "position_m": [
                  -0.008478398524457304,
                  0.0030433953970704,
                  0.2560551482292099
                ]
              },
              "source_header_frame_id": "robotiq_arg2f_base_link"
            }
          ],
          "source": {
            "entity": "/mac_n_cheese_centered"
          }
        },
        "payload_quality": "good",
        "policy_id": "scene",
        "source_epoch": "isaac_scene_objects.47975ae68167d879eb3bd676660bbd09.1",
        "source_epoch_order": 1,
        "source_header_frame_id": "robotiq_arg2f_base_link",
        "source_id": "isaac_scene_objects",
        "source_sequence": 6036
      },
      {
        "buffered": false,
        "eligible": true,
        "expires_at_ns": 426291576264783,
        "fresh": true,
        "object_id": "isaac_small_klt",
        "observed_at_ns": 426290576264783,
        "payload": {
          "api_version": "manyforge.ros_pose_observation.v2",
          "kind": "simulation_entity_pose_with_references.v1",
          "pose": {
            "frame_id": "universe",
            "frame_kind": "cell_frame",
            "orientation_xyzw": [
              -7.44024727477188e-07,
              0.0014111957826795053,
              5.799027822903958e-05,
              0.9999990025810211
            ],
            "position_m": [
              -0.5618385076522827,
              -0.36725178360939026,
              0.0734587162733078
            ],
            "source_stamp_ns": 0
          },
          "references": [
            {
              "coherent": true,
              "command_target_id": "gripper",
              "controllable_component_id": "robotiq_2f_140",
              "entity": "/World/ur10e_robotiq2f_140_ROS/ur10e_robotiq2f_140/ee_link/robotiq_arg2f_base_link",
              "gripper_link": "robotiq_base_link",
              "link": "robotiq_base_link",
              "pose_in_reference": {
                "orientation_xyzw": [
                  -0.7071296813600123,
                  0.7063393703937014,
                  -0.028678780045265768,
                  -0.015160314859273502
                ],
                "position_m": [
                  0.33176052864656974,
                  0.08676931827220581,
                  0.5090466380765155
                ]
              },
              "source_header_frame_id": "robotiq_arg2f_base_link"
            }
          ],
          "source": {
            "entity": "/World/small_KLT"
          }
        },
        "payload_quality": "good",
        "policy_id": "scene",
        "source_epoch": "isaac_scene_objects.47975ae68167d879eb3bd676660bbd09.1",
        "source_epoch_order": 1,
        "source_id": "isaac_scene_objects",
        "source_sequence": 6037
      }
    ],
    "reason": "durable_finalized_active_projection",
    "runtime_identity": {
      "deployment_generation": 4,
      "graph_revision": "sha256:5b83362c6824f7f99d345196cd2cbfe591a476049df87438629ecc95f25fce95",
      "program_generation": 0,
      "retained_bundle_id": "sha256:9a6355d1b1ee8f2b79f8176739e267634fccf770aecf4dc3a4c63f9552911c9d",
      "runtime_generation": 4
    },
    "started_at_ns": 426124871967049,
    "state": "open",
    "updated_at_ns": 426290590363442
  }
};

function section(payload: Record<string, unknown>): HeadlessSourceSection {
  return {
    available: true,
    freshness: 'fresh',
    knownGaps: [],
    payload,
    sourceGeneration: 466,
    sourceOwnerEpoch: 'owner-epoch-1',
    sourceOwnerId: 'owner-1',
    receiverAgeNs: null,
  };
}

function headlessRuntime(): HeadlessRuntimeSnapshot {
  return {
    sections: {
      logicalScene: section(LOGICAL_SCENE_PAYLOAD),
      sceneRuntime: section(SCENE_RUNTIME_PAYLOAD),
    },
  } as unknown as HeadlessRuntimeSnapshot;
}

function environment(): NodeEnvironmentResponse {
  return {
    active: { retainedBundleId: BUNDLE, graphRevision: GRAPH },
  } as unknown as NodeEnvironmentResponse;
}

function sourceModel(): SourceModelProjection {
  return {
    cell_id: CELL,
    graph_revision: GRAPH,
    retained_bundle_id: BUNDLE,
    components: [
      { component_id: 'isaac_ur10e_light_robot', mounted_on: null },
      {
        component_id: 'robotiq_2f_140',
        mounted_on: { component_id: 'isaac_ur10e_light_robot', interface_id: 'flange' },
      },
    ],
    occurrences: [
      {
        component_id: 'isaac_ur10e_light_robot',
        model_instance_id: 'isaac_ur10e_light_robot.model',
        model_revision: ARM_MODEL_REVISION,
        coordinates: [],
        links: [
          { source_link_name: 'base_link', viewer_link_name: BASE_LINK },
          { source_link_name: 'tool0', viewer_link_name: TOOL0_LINK },
        ],
      },
      {
        component_id: 'robotiq_2f_140',
        model_instance_id: 'robotiq_2f_140.model',
        model_revision: TOOL_MODEL_REVISION,
        coordinates: [],
        links: [{ source_link_name: 'grasp_frame', viewer_link_name: GRASP_LINK }],
      },
    ],
    targets: [
      {
        component_id: 'isaac_ur10e_light_robot',
        target_id: 'arm',
        frames: [
          {
            frame_kind: 'component_model_frame',
            frame_role_id: 'base',
            model_instance_id: 'isaac_ur10e_light_robot.model',
            source_link_name: 'base_link',
          },
          {
            frame_kind: 'component_model_frame',
            frame_role_id: 'flange',
            model_instance_id: 'isaac_ur10e_light_robot.model',
            source_link_name: 'tool0',
          },
        ],
        model_groups: [],
      },
      {
        component_id: 'robotiq_2f_140',
        target_id: 'gripper',
        frames: [{
          frame_kind: 'component_model_frame',
          frame_role_id: 'tcp',
          model_instance_id: 'robotiq_2f_140.model',
          source_link_name: 'grasp_frame',
        }],
        model_groups: [],
      },
    ],
  } as unknown as SourceModelProjection;
}

function manifest(): SceneUrdfManifest {
  return {
    cell_id: CELL,
    graph_revision: GRAPH,
    retained_bundle_id: BUNDLE,
    root_link: BASE_LINK,
    links: [BASE_LINK, TOOL0_LINK, GRASP_LINK].map((name) => ({ name })),
    joints: [],
  } as unknown as SceneUrdfManifest;
}

describe('carried object on a mounted tool (Isaac UR10e light scene)', () => {
  it('keeps the scene owner facts and every fixed object while the tool carries the box', () => {
    const result = sceneOwnerFacts(headlessRuntime(), sourceModel(), environment());
    if (!result.available) throw new Error(result.reason);
    expect(result.facts.logicalScene.snapshot?.objects.map((object) => (
      [object.scene_resource_id, object.placement.kind]
    ))).toEqual([
      ['isaac_cheese_box', 'carried'],
      ['isaac_small_klt', 'cell_fixed'],
    ]);
    expect(result.facts.sceneRuntime.state.pose_sources[0]).toMatchObject({
      object_id: 'isaac_cheese_box',
      policy_id: 'scene',
      holder: { controllable_component_id: 'robotiq_2f_140', command_target_id: 'gripper' },
    });
  });

  it('renders the carried box on the mounted tool TCP link with its measurement', () => {
    const result = sceneOwnerFacts(headlessRuntime(), sourceModel(), environment());
    if (!result.available) throw new Error(result.reason);
    const presentation = runtimeScenePresentation(
      result.facts.sceneRuntime,
      result.facts.logicalScene,
      sourceModel(),
      manifest(),
    );
    expect(presentation.attachmentDiagnostics).toEqual([]);
    expect(presentation.attachedObjects).toHaveLength(1);
    expect(presentation.attachedObjects[0]).toMatchObject({
      carrierComponentId: 'robotiq_2f_140',
      commandTargetId: 'gripper',
      carrierModelInstanceId: 'robotiq_2f_140.model',
      carrierFrameRoleId: 'tcp',
      carrierCompiledFrameName: GRASP_LINK,
      poseMeasurement: {
        holder: { controllable_component_id: 'robotiq_2f_140', command_target_id: 'gripper' },
        frame: { controllable_component_id: 'robotiq_2f_140', frame_role_id: 'tcp' },
        fresh: true,
      },
    });
  });

  // G4 (C-P5): the join needs no Robot-control descriptor, so the box is
  // drawn at the gripper whether the arm, the gripper, another robot or
  // nothing is selected (before: carrier_descriptor_unavailable or
  // carrier_target_mismatch unless the arm was selected).
  it('draws the carried box at its holder frame without any selection', () => {
    const result = sceneOwnerFacts(headlessRuntime(), sourceModel(), environment());
    if (!result.available) throw new Error(result.reason);
    const joined = attachedScenePresentation(result.facts.logicalScene, sourceModel(), manifest());
    expect(joined.diagnostics).toEqual([]);
    expect(joined.objects.map((item) => [item.carrierComponentId, item.carrierCompiledFrameName]))
      .toEqual([['robotiq_2f_140', GRASP_LINK]]);
  });

  it('reports a holder frame its holder target does not list', () => {
    const result = sceneOwnerFacts(headlessRuntime(), sourceModel(), environment());
    if (!result.available) throw new Error(result.reason);
    const model = sourceModel();
    model.targets[1].frames[0].frame_role_id = 'cup_center';
    const joined = attachedScenePresentation(result.facts.logicalScene, model, manifest());
    expect(joined.objects).toEqual([]);
    expect(joined.diagnostics.map((item) => item.code)).toEqual(['carrier_frame_missing']);
  });

  it('reports a holder frame on a model its holder does not own', () => {
    const result = sceneOwnerFacts(headlessRuntime(), sourceModel(), environment());
    if (!result.available) throw new Error(result.reason);
    const model = sourceModel();
    model.occurrences[1].component_id = 'isaac_ur10e_light_robot';
    const joined = attachedScenePresentation(result.facts.logicalScene, model, manifest());
    expect(joined.objects).toEqual([]);
    expect(joined.diagnostics.map((item) => item.code)).toEqual(['carrier_model_mismatch']);
  });
});
