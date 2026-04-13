#!/usr/bin/env python3
"""Generate an OpenClaw workspace plugin that exposes ManyForge MCP tools.

The generated plugin is intended for the embedded OpenClaw main agent. It
registers one OpenClaw agent tool per ManyForge MCP tool and forwards calls
to a configured ManyForge HTTP endpoint. The endpoint can be:

    - direct dedicated route: https://host.openshell.internal/api/mcp
    - fallback mux route:     https://inference.local/v1/completions

The tool definitions are generated from the live ManyForge MCP tool list so the
plugin stays aligned with the current backend surface.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import textwrap
import urllib.error
import urllib.request


DEFAULT_MCP_ENDPOINT = "http://localhost:9000/api/mcp"
DEFAULT_PLUGIN_ID = "manyforge-mcp"
DEFAULT_TRANSPORT_ENDPOINT = "https://host.openshell.internal/api/mcp"
DEFAULT_PRINCIPAL_ID = "llm:openclaw-agent"
DEFAULT_TIMEOUT_MS = 120_000
DEFAULT_PLUGIN_VERSION = "0.2.0"
DEFAULT_TERMINAL_ANSWER_DIR = "/sandbox/.manyforge/terminal-answers"


def fetch_manyforge_tools(endpoint: str, principal_id: str) -> list[dict]:
    payload = {
        "jsonrpc": "2.0",
        "method": "tools/list",
        "id": 1,
    }
    body = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        endpoint,
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "X-Principal-ID": principal_id,
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            data = json.loads(response.read())
    except urllib.error.HTTPError as exc:
        message = exc.read().decode("utf-8", errors="replace")
        raise SystemExit(f"ManyForge tools/list failed: HTTP {exc.code}: {message[:400]}") from exc
    except urllib.error.URLError as exc:
        raise SystemExit(f"ManyForge tools/list failed: {exc}") from exc

    tools = data.get("result", {}).get("tools")
    if not isinstance(tools, list) or not tools:
        raise SystemExit("ManyForge tools/list returned no tools")
    return tools


def build_plugin_manifest(plugin_id: str, plugin_version: str) -> dict:
    return {
        "id": plugin_id,
        "version": plugin_version,
        "name": "ManyForge MCP",
        "description": "Expose ManyForge MCP tools to the embedded OpenClaw agent via a configured HTTP transport.",
        "configSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "endpoint": {
                    "type": "string",
                    "description": "ManyForge transport endpoint inside the sandbox.",
                },
                "principalId": {
                    "type": "string",
                    "description": "Principal ID sent to ManyForge for audit attribution.",
                },
                "bearerToken": {
                    "type": "string",
                    "description": "Dedicated bearer token for the ManyForge route.",
                },
                "caCertPath": {
                    "type": "string",
                    "description": "Optional CA bundle used only for direct HTTPS routes.",
                },
                "timeoutMs": {
                    "type": "number",
                    "minimum": 1000,
                    "maximum": 600000,
                    "description": "HTTP timeout for forwarded ManyForge tool calls.",
                },
                "terminalAnswerDir": {
                    "type": "string",
                    "description": "Directory where reporting-only terminal-answer tool calls are persisted as structured JSON records.",
                },
            },
        },
    }


def build_plugin_index(plugin_id: str, tools: list[dict]) -> str:
    tool_defs = []
    for tool in tools:
        name = tool.get("name")
        description = tool.get("description")
        schema = tool.get("inputSchema")
        if not isinstance(name, str) or not isinstance(description, str) or not isinstance(schema, dict):
            raise SystemExit(f"Unexpected ManyForge tool shape: {tool!r}")
        tool_defs.append(
            {
                "name": name,
                "label": name.replace("_", " "),
                "description": description,
                "parameters": schema,
            }
        )

    tools_json = json.dumps(tool_defs, indent=2, sort_keys=True)
    return textwrap.dedent(
        f"""\
        import {{ appendFileSync, existsSync, mkdirSync, readFileSync, writeFileSync }} from "node:fs";
        import {{ request as httpRequest }} from "node:http";
        import {{ request as httpsRequest }} from "node:https";

        const MANYFORGE_PLUGIN_ID = {json.dumps(plugin_id)};
        const DEFAULT_ENDPOINT = {json.dumps(DEFAULT_TRANSPORT_ENDPOINT)};
        const DEFAULT_PRINCIPAL_ID = {json.dumps(DEFAULT_PRINCIPAL_ID)};
        const DEFAULT_TIMEOUT_MS = {DEFAULT_TIMEOUT_MS};
        const DEFAULT_TERMINAL_ANSWER_DIR = {json.dumps(DEFAULT_TERMINAL_ANSWER_DIR)};
        const TOOL_DEFS = {tools_json};
        const TERMINAL_LIFECYCLE_STATES = ["idle", "running", "stopped", "direct_control"];
        const TERMINAL_RECOVERY_OUTCOMES = ["resolved", "unresolved", "escalated"];
        const TERMINAL_PROGRAM_ACTIONS = ["load", "validate", "save", "run", "stop", "intervene"];
        const TERMINAL_PROGRAM_RESULTS = ["success", "failure"];

        function normalizeTimeoutMs(value) {{
          if (typeof value !== "number" || !Number.isFinite(value)) {{
            return DEFAULT_TIMEOUT_MS;
          }}
          return Math.max(1000, Math.min(600000, Math.floor(value)));
        }}

        function normalizeTerminalAnswerDir(value) {{
          if (typeof value !== "string" || !value.trim()) {{
            return DEFAULT_TERMINAL_ANSWER_DIR;
          }}
          return value;
        }}

        function stringifyFallback(value) {{
          try {{
            return JSON.stringify(value, null, 2);
          }} catch (_err) {{
            return String(value);
          }}
        }}

        function normalizeResult(result) {{
          const details = result?.structuredContent ?? result?.details ?? result;
          const content = Array.isArray(result?.content) && result.content.every(
            (item) => item && typeof item.type === "string",
          )
            ? result.content
            : [{{ type: "text", text: stringifyFallback(details) }}];
          return details === undefined ? {{ content }} : {{ content, details }};
        }}

        function persistTerminalAnswer(answerDir, toolName, payload) {{
          mkdirSync(answerDir, {{ recursive: true }});
          const timestamp = Date.now();
          const serialized = JSON.stringify(payload, null, 2);
          writeFileSync(`${{answerDir}}/latest-${{toolName}}.json`, `${{serialized}}\\n`, "utf8");
          writeFileSync(`${{answerDir}}/${{timestamp}}-${{toolName}}.json`, `${{serialized}}\\n`, "utf8");
          appendFileSync(`${{answerDir}}/history.jsonl`, `${{JSON.stringify(payload)}}\\n`, "utf8");
        }}

        function terminalAnswerResponse(payload) {{
          return {{
            content: [{{
              type: "text",
              text: JSON.stringify(payload),
            }}],
            details: payload,
          }};
        }}

        function terminalAnswerError(toolName, message) {{
          return {{
            content: [{{
              type: "text",
              text: `${{toolName}} rejected: ${{message}}`,
            }}],
            details: {{
              ok: false,
              tool: toolName,
              error: message,
            }},
          }};
        }}

        async function callManyForge({{
          endpoint,
          principalId,
          bearerToken,
          caCertPath,
          timeoutMs,
          toolName,
          args,
          signal,
        }}) {{
          const url = new URL(endpoint);
          const usesMuxEndpoint =
            url.hostname === "inference.local" || url.pathname.endsWith("/completions");
          const payloadText = JSON.stringify({{
            jsonrpc: "2.0",
            method: "tools/call",
            params: {{
              name: toolName,
              arguments: args ?? {{}},
            }},
            id: `manyforge-${{toolName}}-${{Date.now()}}`,
            ...(usesMuxEndpoint ? {{ x_manyforge: true }} : {{}}),
          }});
          const payloadBytes = Buffer.from(payloadText, "utf8");
          const headers = {{
            "Content-Type": "application/json",
            "Content-Length": String(payloadBytes.length),
            "X-Principal-ID": principalId,
          }};
          if (usesMuxEndpoint) {{
            headers["X-ManyForge-Target"] = "mcp";
          }}
          if (typeof bearerToken === "string" && bearerToken) {{
            headers.Authorization = `Bearer ${{bearerToken}}`;
          }}

          const requestImpl = url.protocol === "https:" ? httpsRequest : httpRequest;
          const tlsOptions =
            url.protocol === "https:" && typeof caCertPath === "string" && caCertPath && existsSync(caCertPath)
              ? {{ ca: readFileSync(caCertPath, "utf8") }}
              : {{}};

          return await new Promise((resolve) => {{
            const req = requestImpl(
              {{
                protocol: url.protocol,
                hostname: url.hostname,
                port: url.port || (url.protocol === "https:" ? 443 : 80),
                path: `${{url.pathname}}${{url.search}}`,
                method: "POST",
                headers,
                ...tlsOptions,
              }},
              (res) => {{
                const chunks = [];
                res.on("data", (chunk) => chunks.push(Buffer.from(chunk)));
                res.on("end", () => {{
                  const text = Buffer.concat(chunks).toString("utf8");
                  let parsed = null;
                  try {{
                    parsed = text ? JSON.parse(text) : null;
                  }} catch (_err) {{
                    parsed = null;
                  }}

                  if ((res.statusCode ?? 500) >= 400) {{
                    resolve({{
                      content: [{{
                        type: "text",
                        text: `ManyForge transport error ${{res.statusCode}}: ${{parsed ? stringifyFallback(parsed) : text}}`,
                      }}],
                      details: parsed ?? text,
                    }});
                    return;
                  }}

                  if (parsed?.error) {{
                    resolve({{
                      content: [{{
                        type: "text",
                        text: `ManyForge MCP error: ${{parsed.error.message ?? stringifyFallback(parsed.error)}}`,
                      }}],
                      details: parsed.error,
                    }});
                    return;
                  }}

                  resolve(normalizeResult(parsed?.result ?? {{}}));
                }});
              }},
            );

            let settled = false;
            const finish = (value) => {{
              if (!settled) {{
                settled = true;
                resolve(value);
              }}
            }};

            const timer = setTimeout(() => {{
              req.destroy(new Error(`timeout after ${{timeoutMs}}ms`));
            }}, timeoutMs);

            const abortHandler = () => {{
              req.destroy(new Error("aborted"));
            }};

            if (signal) {{
              signal.addEventListener("abort", abortHandler, {{ once: true }});
            }}

            req.on("error", (error) => {{
              clearTimeout(timer);
              if (signal) {{
                signal.removeEventListener("abort", abortHandler);
              }}
              finish({{
                content: [{{
                  type: "text",
                  text: `ManyForge bridge error: ${{error instanceof Error ? error.message : String(error)}}`,
                }}],
              }});
            }});

            req.on("response", () => {{
              clearTimeout(timer);
              if (signal) {{
                signal.removeEventListener("abort", abortHandler);
              }}
            }});

            req.write(payloadBytes);
            req.end();
          }});
        }}

        export default function registerManyForgePlugin(api) {{
          const pluginConfig = api.pluginConfig ?? {{}};
          const endpoint =
            typeof pluginConfig.endpoint === "string" && pluginConfig.endpoint
              ? pluginConfig.endpoint
              : DEFAULT_ENDPOINT;
          const principalId =
            typeof pluginConfig.principalId === "string" && pluginConfig.principalId
              ? pluginConfig.principalId
              : DEFAULT_PRINCIPAL_ID;
          const bearerToken =
            typeof pluginConfig.bearerToken === "string" ? pluginConfig.bearerToken : "";
          const caCertPath =
            typeof pluginConfig.caCertPath === "string" && pluginConfig.caCertPath
              ? pluginConfig.caCertPath
              : "";
          const timeoutMs = normalizeTimeoutMs(pluginConfig.timeoutMs);
          const terminalAnswerDir = normalizeTerminalAnswerDir(pluginConfig.terminalAnswerDir);

          for (const toolDef of TOOL_DEFS) {{
            api.registerTool(
              {{
                name: toolDef.name,
                label: toolDef.label,
                description: toolDef.description,
                parameters: toolDef.parameters,
                async execute(_toolCallId, params, signal) {{
                  return callManyForge({{
                    endpoint,
                    principalId,
                    bearerToken,
                    caCertPath,
                    timeoutMs,
                    toolName: toolDef.name,
                    args: params,
                    signal,
                  }});
                }},
              }},
              {{ optional: true }},
            );
          }}

          api.registerTool(
            {{
              name: "report_lifecycle_state",
              label: "report lifecycle state",
              description:
                "Reporting-only terminal-answer tool. Use it to emit the final ManyForge lifecycle state as a narrow typed result.",
              parameters: {{
                type: "object",
                additionalProperties: false,
                required: ["lifecycle_state"],
                properties: {{
                  lifecycle_state: {{
                    type: "string",
                    enum: TERMINAL_LIFECYCLE_STATES,
                  }},
                  current_node: {{
                    type: "string",
                  }},
                  run_id: {{
                    type: "string",
                  }},
                  summary_code: {{
                    type: "string",
                  }},
                }},
              }},
              async execute(_toolCallId, params) {{
                const lifecycleState = params?.lifecycle_state;
                if (!TERMINAL_LIFECYCLE_STATES.includes(lifecycleState)) {{
                  return terminalAnswerError(
                    "report_lifecycle_state",
                    `lifecycle_state must be one of ${{TERMINAL_LIFECYCLE_STATES.join(", ")}}`,
                  );
                }}
                const payload = {{
                  ok: true,
                  tool: "report_lifecycle_state",
                  lifecycle_state: lifecycleState,
                  current_node:
                    typeof params?.current_node === "string" ? params.current_node : null,
                  run_id: typeof params?.run_id === "string" ? params.run_id : null,
                  summary_code:
                    typeof params?.summary_code === "string" ? params.summary_code : null,
                  timestamp_ms: Date.now(),
                }};
                persistTerminalAnswer(terminalAnswerDir, "report_lifecycle_state", payload);
                return terminalAnswerResponse(payload);
              }},
            }},
            {{ optional: true }},
          );

          api.registerTool(
            {{
              name: "report_recovery_outcome",
              label: "report recovery outcome",
              description:
                "Reporting-only terminal-answer tool. Use it to emit the final delegated-recovery outcome as a narrow typed result.",
              parameters: {{
                type: "object",
                additionalProperties: false,
                required: ["outcome", "summary_code"],
                properties: {{
                  outcome: {{
                    type: "string",
                    enum: TERMINAL_RECOVERY_OUTCOMES,
                  }},
                  summary_code: {{
                    type: "string",
                  }},
                  run_id: {{
                    type: "string",
                  }},
                  recovery_request_id: {{
                    type: "string",
                  }},
                }},
              }},
              async execute(_toolCallId, params) {{
                const outcome = params?.outcome;
                const summaryCode = params?.summary_code;
                if (!TERMINAL_RECOVERY_OUTCOMES.includes(outcome)) {{
                  return terminalAnswerError(
                    "report_recovery_outcome",
                    `outcome must be one of ${{TERMINAL_RECOVERY_OUTCOMES.join(", ")}}`,
                  );
                }}
                if (typeof summaryCode !== "string" || !summaryCode.trim()) {{
                  return terminalAnswerError(
                    "report_recovery_outcome",
                    "summary_code must be a non-empty string",
                  );
                }}
                const payload = {{
                  ok: true,
                  tool: "report_recovery_outcome",
                  outcome,
                  summary_code: summaryCode,
                  run_id: typeof params?.run_id === "string" ? params.run_id : null,
                  recovery_request_id:
                    typeof params?.recovery_request_id === "string"
                      ? params.recovery_request_id
                      : null,
                  timestamp_ms: Date.now(),
                }};
                persistTerminalAnswer(terminalAnswerDir, "report_recovery_outcome", payload);
                return terminalAnswerResponse(payload);
              }},
            }},
            {{ optional: true }},
          );

          api.registerTool(
            {{
              name: "report_program_operation",
              label: "report program operation",
              description:
                "Reporting-only terminal-answer tool. Use it to emit the final result of a program-facing operation such as load, validate, save, run, stop, or intervene.",
              parameters: {{
                type: "object",
                additionalProperties: false,
                required: ["action", "result", "summary_code"],
                properties: {{
                  action: {{
                    type: "string",
                    enum: TERMINAL_PROGRAM_ACTIONS,
                  }},
                  result: {{
                    type: "string",
                    enum: TERMINAL_PROGRAM_RESULTS,
                  }},
                  summary_code: {{
                    type: "string",
                  }},
                  artifact_path: {{
                    type: "string",
                  }},
                  run_id: {{
                    type: "string",
                  }},
                }},
              }},
              async execute(_toolCallId, params) {{
                const action = params?.action;
                const result = params?.result;
                const summaryCode = params?.summary_code;
                if (!TERMINAL_PROGRAM_ACTIONS.includes(action)) {{
                  return terminalAnswerError(
                    "report_program_operation",
                    `action must be one of ${{TERMINAL_PROGRAM_ACTIONS.join(", ")}}`,
                  );
                }}
                if (!TERMINAL_PROGRAM_RESULTS.includes(result)) {{
                  return terminalAnswerError(
                    "report_program_operation",
                    `result must be one of ${{TERMINAL_PROGRAM_RESULTS.join(", ")}}`,
                  );
                }}
                if (typeof summaryCode !== "string" || !summaryCode.trim()) {{
                  return terminalAnswerError(
                    "report_program_operation",
                    "summary_code must be a non-empty string",
                  );
                }}
                const payload = {{
                  ok: true,
                  tool: "report_program_operation",
                  action,
                  result,
                  summary_code: summaryCode,
                  artifact_path:
                    typeof params?.artifact_path === "string" ? params.artifact_path : null,
                  run_id: typeof params?.run_id === "string" ? params.run_id : null,
                  timestamp_ms: Date.now(),
                }};
                persistTerminalAnswer(terminalAnswerDir, "report_program_operation", payload);
                return terminalAnswerResponse(payload);
              }},
            }},
            {{ optional: true }},
          );

          api.logger?.info?.(
            `[${{MANYFORGE_PLUGIN_ID}}] registered ${{TOOL_DEFS.length}} ManyForge tools and 3 terminal-answer tools via ${{endpoint}}`,
          );
        }}
        """
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate the ManyForge OpenClaw workspace plugin")
    parser.add_argument("--mcp-endpoint", default=DEFAULT_MCP_ENDPOINT, help="Host-side ManyForge MCP HTTP endpoint")
    parser.add_argument("--principal-id", default="system:manyforge-plugin-generator", help="Principal ID used for the host-side tools/list call")
    parser.add_argument("--output-dir", required=True, help="Destination plugin directory")
    parser.add_argument("--plugin-id", default=DEFAULT_PLUGIN_ID, help="Plugin identifier")
    parser.add_argument("--plugin-version", default=DEFAULT_PLUGIN_VERSION, help="Plugin version to embed in openclaw.plugin.json")
    args = parser.parse_args()

    output_dir = pathlib.Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    tools = fetch_manyforge_tools(args.mcp_endpoint, args.principal_id)
    (output_dir / "openclaw.plugin.json").write_text(
        json.dumps(build_plugin_manifest(args.plugin_id, args.plugin_version), indent=2) + "\n",
        encoding="utf-8",
    )
    dist_dir = output_dir / "dist"
    dist_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "package.json").write_text(
        json.dumps(
            {
                "name": args.plugin_id,
                "version": args.plugin_version,
                "private": True,
                "type": "module",
                "main": "dist/index.js",
                "openclaw": {
                    "extensions": ["./dist/index.js"],
                },
                "files": ["dist/", "openclaw.plugin.json", "README.md"],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    (dist_dir / "index.js").write_text(
        build_plugin_index(args.plugin_id, tools),
        encoding="utf-8",
    )
    (output_dir / "README.md").write_text(
        textwrap.dedent(
            f"""\
            # ManyForge OpenClaw Plugin

            Generated from the live ManyForge MCP tool list.

            - Plugin id: `{args.plugin_id}`
            - Plugin version: `{args.plugin_version}`
            - Host MCP endpoint: `{args.mcp_endpoint}`
            - Sandbox transport: `{DEFAULT_TRANSPORT_ENDPOINT}`
            - Terminal answer dir: `{DEFAULT_TERMINAL_ANSWER_DIR}`
            - Tool count: `{len(tools)}`
            """
        ),
        encoding="utf-8",
    )

    print(f"Generated plugin at {output_dir} with {len(tools)} tools")
    return 0


if __name__ == "__main__":
    sys.exit(main())
