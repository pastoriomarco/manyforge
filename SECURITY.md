# Security policy

## Reporting a vulnerability

Please report security vulnerabilities **privately** by email:

📧 [pastoriomarco@gmail.com](mailto:pastoriomarco@gmail.com)

Include:

- A description of the vulnerability and its impact.
- Reproduction steps, ideally a minimal program or deployment YAML
  that triggers the issue.
- The version of ManyForge affected (check `VERSION` at the repo root
  or the `version` field of any sub-package's metadata).
- The lane / deployment context (`manyforge-only` / `isaac-sim` /
  `real-robot`) where the issue was observed, plus the assistant
  setting (`on` / `off`) if relevant.

I'll acknowledge receipt within 7 days and aim to provide a fix or
mitigation timeline within 30 days for confirmed issues. For severe
vulnerabilities affecting deployed robots — anything that would let an
unauthorized party command the arm, bypass the command gate, or
exfiltrate live sandbox state — flag this explicitly in your email so
the response is prioritized.

Please **do not** open public GitHub issues, draft pull requests, or
post in Discussions for security topics. A public report can give an
attacker a head start before a fix is available.

## Supported versions

Only the latest 0.x line receives security fixes at this stage:

| Version | Supported          |
| ------- | ------------------ |
| 0.1.x   | ✅ Yes             |
| < 0.1   | ❌ Pre-publication |

When 0.2 ships, 0.1.x will continue to receive critical security
fixes for one minor version cycle (≈ until 0.3 ships).

## Threat model

ManyForge runs as part of a robot/cell deployment. The following
assumptions are baked into the design:

- **The control network is trusted.** ROS 2 / DDS traffic is not
  authenticated by default; we assume the network the controller
  speaks on is isolated (operator-controlled subnet, no public
  exposure).
- **The host running Composer is trusted.** No ManyForge component
  is designed to be exposed to untrusted internet traffic; the
  Composer HTTP API has no authentication layer by default.
- **The sandbox + OpenShell gateway are isolation primitives, not
  certified safety systems.** They reduce blast radius for the
  AI-assisted authoring lane; they do not certify the resulting
  robot motions are safe.
- **Operator review is the safety chain.** The `composer-assistant`
  mode operates under `review: required` authority — AI-proposed
  edits do not reach a running robot until an operator approves them.

If your deployment violates any of these assumptions (Composer exposed
to the open internet, untrusted operators with assistant access, no
review gate), that's a configuration concern that's outside the scope
of upstream security fixes — though I'm happy to discuss hardening
guidance.

## What's NOT a security issue

The following are functional concerns, not security ones, and belong
in regular GitHub issues:

- Assistant calls timing out or returning poor-quality outputs.
- vLLM / OpenClaw connection failures.
- Composer UI bugs that don't affect access control.
- Test failures or build breakage.

## Hardening checklist for production deployments

If you're deploying ManyForge against real hardware:

- ✅ Run Composer on an isolated network — do not expose ports
  9000, 8081, 8100, 8200, 8000, or 8050 to the public internet.
- ✅ Use the `openclaw` assistant lane (sandboxed gateway) for
  any AI authoring — not the direct `nemoclaw` lane.
- ✅ Configure `assistant_modes.composer-assistant.authority.review`
  to `required` — keep AI edits behind operator approval.
- ✅ Wire `/api/infra/heartbeat` to an external watchdog independent
  of ManyForge.
- ✅ Connect the robot's e-stop and safety chain *outside* the
  ManyForge command path. ManyForge `InterventionRequired` is a
  software stop, not a certified safety stop.
- ✅ Audit `model_endpoints` in your deployment YAML — make sure
  `base_url` points only to endpoints you control.
