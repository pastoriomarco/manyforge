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
- The lane (Mock / Isaac / Real) where the issue was observed, and
  whether the AI assistant was in use.

I'll acknowledge receipt within 7 days and aim to provide a fix or
mitigation timeline within 30 days for confirmed issues. For severe
vulnerabilities affecting deployed robots — anything that would let an
unauthorized party command the arm, bypass the command gate, or
exfiltrate runtime state — flag this explicitly in your email so
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
- **Operator review gates AI edits.** The AI assistant edits only the
  Program Draft; nothing it proposes reaches the runtime until an
  operator Applies the Draft. This is not a certified safety function.

If your deployment violates any of these assumptions (Composer exposed
to the open internet, untrusted operators with assistant access, no
review gate), that's a configuration concern that's outside the scope
of upstream security fixes — though I'm happy to discuss hardening
guidance.

## What's NOT a security issue

The following are functional concerns, not security ones, and belong
in regular GitHub issues:

- Assistant calls timing out or returning poor-quality outputs.
- Model server connection failures.
- Composer UI bugs that don't affect access control.
- Test failures or build breakage.

## Hardening checklist for production deployments

If you're deploying ManyForge against real hardware:

- ✅ Run the workbench on an isolated network — do not expose its ports
  (3000, 9010, 9011, 9020, and the assistant bridge on 8100) or your
  model server to the public internet.
- ✅ Review AI assistant edits in the Draft before Applying them.
- ✅ Connect the robot's e-stop and safety chain *outside* the
  ManyForge command path. ManyForge Stops are software stops, not
  certified safety stops.
- ✅ Audit the `ai_model` components in your Deployment — make sure
  `base_url` points only to endpoints you control.
