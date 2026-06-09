// Build-around: derive a Hermes sandbox Dockerfile from the PRISTINE upstream
// NemoClaw one (agents/hermes/Dockerfile) by applying a few anchored transforms.
// The upstream file is read-only; we write a derived copy into our staging
// context. This tracks upstream (re-derives each run) instead of vendoring 330
// lines. Env-driven so setup-hermes.sh can parameterize per deployment.
//
//   HERMES_UPSTREAM_DOCKERFILE  pristine source (default ~/NemoClaw/agents/hermes/Dockerfile)
//   HERMES_DERIVED_DOCKERFILE   output path in the staging context (required)
//   NEMOCLAW_MODEL              model id to bake (direct `openshell create` skips
//                               nemoclaw's patchStagedDockerfile, so we bake it)
//   NEMOCLAW_INFERENCE_BASE_URL inference endpoint to bake
//   HERMES_MCP_VERSION          mcp SDK pin (default 1.26.0)
//   HERMES_TOOL_GATEWAY_PRESETS JSON array of preset names (default ["manyforge"])
import fs from "node:fs";

const SRC = process.env.HERMES_UPSTREAM_DOCKERFILE
  || `${process.env.HOME}/NemoClaw/agents/hermes/Dockerfile`;
const OUT = process.env.HERMES_DERIVED_DOCKERFILE;
if (!OUT) { console.error("HERMES_DERIVED_DOCKERFILE is required"); process.exit(2); }
const MODEL = process.env.NEMOCLAW_MODEL || "";
const BASE_URL = process.env.NEMOCLAW_INFERENCE_BASE_URL || "";
const MCP_VERSION = process.env.HERMES_MCP_VERSION || "1.26.0";
const presets = JSON.parse(process.env.HERMES_TOOL_GATEWAY_PRESETS || '["manyforge"]');
const presetsB64 = Buffer.from(JSON.stringify(presets)).toString("base64");

let d = fs.readFileSync(SRC, "utf8");
const fail = (m) => { console.error(`derive: ${m}`); process.exit(1); };

// 1+2. Enable the managed-tool-gateway broker + select our preset(s). The upstream
//      Dockerfile already declares these ARGs (defaults 0 / W10=); config-gen then
//      loads the matrix and merges entry.config (mcp_servers) into config.yaml.
if (!/^ARG NEMOCLAW_HERMES_TOOL_GATEWAY_BROKER=0$/m.test(d)) fail("broker ARG anchor missing");
d = d.replace(/^ARG NEMOCLAW_HERMES_TOOL_GATEWAY_BROKER=0$/m, "ARG NEMOCLAW_HERMES_TOOL_GATEWAY_BROKER=1");
d = d.replace(/^ARG NEMOCLAW_HERMES_TOOL_GATEWAY_PRESETS_B64=W10=$/m, `ARG NEMOCLAW_HERMES_TOOL_GATEWAY_PRESETS_B64=${presetsB64}`);

// 3. Bake model + endpoint when provided (direct `openshell create` does not run
//    nemoclaw's patchStagedDockerfile, which is what normally injects these).
if (MODEL) d = d.replace(/^ARG NEMOCLAW_MODEL=.*$/m, `ARG NEMOCLAW_MODEL=${MODEL}`);
if (BASE_URL) d = d.replace(/^ARG NEMOCLAW_INFERENCE_BASE_URL=.*$/m, `ARG NEMOCLAW_INFERENCE_BASE_URL=${BASE_URL}`);

// 4. Install the MCP client SDK the base image omits (HERMES_UV_EXTRAS excludes
//    "mcp"); fail the build if it can't import. Inserted after the cert symlink
//    RUN (still root, before USER sandbox).
const certAnchor = '    && ln -sf "$SSL_CERT_FILE" "$_hermes_certifi"\n';
if (!d.includes(certAnchor)) fail("cert-symlink anchor missing");
d = d.replace(certAnchor, certAnchor +
  "\n# [BUILD-AROUND] ManyForge: install the MCP client SDK the base image omits.\n" +
  `RUN uv pip install --python /opt/hermes/.venv/bin/python "mcp==${MCP_VERSION}" \\\n` +
  "    && /opt/hermes/.venv/bin/python -c \"import mcp, mcp.client.stdio; print('[build-around] mcp client SDK OK')\"\n");

// 5. Vendor the stdlib-only stdio bridge at the path the baked mcp_servers points to.
const pluginAnchor = "RUN chmod -R a+rX /opt/nemoclaw-hermes-plugin/\n";
if (!d.includes(pluginAnchor)) fail("plugin-chmod anchor missing");
d = d.replace(pluginAnchor, pluginAnchor +
  "\n# [BUILD-AROUND] ManyForge: vendored stdio bridge (stdlib-only).\n" +
  "RUN mkdir -p /sandbox/manyforge/scripts\n" +
  "COPY manyforge-mcp-bridge.py /sandbox/manyforge/scripts/manyforge-mcp-bridge.py\n" +
  "RUN chmod 755 /sandbox/manyforge/scripts/manyforge-mcp-bridge.py\n");

const checks = {
  brokerOn: /^ARG NEMOCLAW_HERMES_TOOL_GATEWAY_BROKER=1$/m.test(d),
  presets: d.includes(`PRESETS_B64=${presetsB64}`),
  model: !MODEL || d.includes(`ARG NEMOCLAW_MODEL=${MODEL}`),
  baseUrl: !BASE_URL || d.includes(`ARG NEMOCLAW_INFERENCE_BASE_URL=${BASE_URL}`),
  mcpInstall: d.includes(`"mcp==${MCP_VERSION}"`),
  bridgeCopy: d.includes("COPY manyforge-mcp-bridge.py /sandbox/manyforge/scripts/"),
};
if (!Object.values(checks).every(Boolean)) fail(`incomplete transforms ${JSON.stringify(checks)}`);
fs.writeFileSync(OUT, d);
console.log(`derived ${OUT} (${d.split("\n").length} lines) presets=${JSON.stringify(presets)} model=${MODEL||"(upstream default)"} ${JSON.stringify(checks)}`);
