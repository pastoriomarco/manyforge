const digest = `sha256:${'a'.repeat(64)}`;
export function managerApplyRecord(phase = 'fence_held', terminal: string | null = null) {
  return {
    apply_generation: 3, apply_id: 'apply-3', candidate_graph_revision: 'graph-2',
    candidate_retained_bundle_id: 'bundle-2', committed: terminal === 'succeeded',
    expected_active_projection_revision: {
      retained_bundle_id: 'bundle-1', graph_revision: 'graph-1',
      scene_revision: { scene_id: 'scene-1', revision_id: 1, content_hash: digest },
    },
    fence: {
      acceptance_digest: digest, configuration_fence_generation: 2,
      configuration_fence_id: 'fence-2', fence_attempt_digest: digest,
      scope: { configurations: ['configuration-1'], resources: ['resource-1'] },
      state: terminal === null ? 'held' : 'released',
    },
    incomplete: terminal === null, intent_digest: digest, phase,
    scene_initialization: 'preserve',
    prepared_activation: {
      schema_version: 'manyforge.prepared_activation_input.v7',
      prepared_activation_id: digest, origin: { kind: 'scenario', scenario_id: 'fixture',
        source_document_digest: digest, project_path: '/projects/fixture.scenario' }, captured_input_digest: digest,
      deployment_artifact_id: 'deployment.fixture', deployment_revision: digest,
      retained_bundle_id: digest, graph_revision: digest,
      program_id: 'fixture-program', program_revision: digest,
      program_outcome: { kind: 'bound', program_binding_id: digest },
      node_environment_digest: digest, assistant_mode: 'forbidden',
    },
    request: { correlation_id: null, request_id: 'request-3',
      request_owner_epoch: 'browser-epoch-3', source_label: 'composer_ui' },
    terminal_state: terminal,
  };
}
