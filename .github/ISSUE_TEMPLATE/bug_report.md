---
name: Bug report
about: Report something that's broken or behaves unexpectedly
title: "[bug] "
labels: bug
assignees: ''
---

<!--
Before filing: search existing issues. Security issues should NOT be
filed here — email pastoriomarco@gmail.com (see SECURITY.md).
-->

## What happened

<!-- What did you do, what did you expect, what did you get? -->

## Reproduction

1.
2.
3.

```text
<paste any relevant command output, error message, stack trace>
```

## Environment

- **ManyForge version**: <!-- cat VERSION -->
- **Lane**: Mock / Isaac / Real
- **Scenario project**: <!-- e.g. `--project examples/mock_dual_arm_workbench.scenario` or the Scenario loaded in Composer -->
- **AI assistant used**: yes / no
- **OS**: <!-- e.g. Ubuntu 24.04 -->
- **Docker version**: <!-- docker version | head -3 -->
- **Hardware** (if relevant): <!-- e.g. UR10e + Robotiq 2F-140, Isaac Sim 2025.1, JetPack 7.x -->

## Logs

<details>
<summary>Composer / kernel / supervisor logs</summary>

```text
<paste relevant logs here — `./scripts/compose-dev.sh status` output,
 `docker logs --tail 200 manyforge-development-workbench`, or `journalctl -u <unit>`>
```

</details>

## Additional context

<!-- Screenshots, related issues, anything else. -->
