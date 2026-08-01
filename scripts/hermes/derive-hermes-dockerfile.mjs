#!/usr/bin/env node
/*
 * Typed boundary for the historical Hermes build-around derivation.
 *
 * Exact source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
 * blob 1a3a31cd9508243a294d28d6d5c8efb03682228b.
 *
 * Historical lines 16-24 parsed an upstream Dockerfile, output path, model,
 * endpoint, MCP SDK version, and managed-tool presets. Lines 26-58 rewrote
 * NemoClaw's image to enable the old managed gateway, install its MCP client,
 * and copy manyforge-mcp-bridge.py. Lines 60-70 validated and wrote that
 * contract-specific image. The future assistant deployment workflow must
 * recreate those responsibilities only after the public client and sandbox
 * transport are qualified. No old Dockerfile transform is kept dormant here.
 */

const unavailable = {
  schema_version: "manyforge.operator_unavailable.v1",
  reason_code: "composable_cell_runtime_unavailable",
  script: "hermes/derive-hermes-dockerfile.mjs",
  historical_source:
    "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1",
  exit_code: 78,
  assignments: [
    "bounded deployment configuration",
    "anchored upstream-image transformation",
    "qualified MCP client installation",
    "bridge artifact staging and transform validation",
  ],
};

console.error(JSON.stringify(unavailable));
process.exit(78);
