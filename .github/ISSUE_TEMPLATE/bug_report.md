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
- **Lane**: manyforge-only / isaac-sim / real-robot
- **Scenario or deployment YAML**: <!-- e.g. `--scenario isaac-light` or path to a custom deployment -->
- **Assistant**: on / off
- **OS**: <!-- e.g. Ubuntu 24.04 -->
- **Docker version**: <!-- docker version | head -3 -->
- **Hardware** (if relevant): <!-- e.g. UR10e + Robotiq 2F-140, Isaac Sim 2025.1, JetPack 7.x -->

## Logs

<details>
<summary>Composer / kernel / supervisor logs</summary>

```text
<paste relevant logs here — `docker logs manyforge-e2e-composer`,
 `journalctl -u <unit>`, or `scripts/launch.sh status` output>
```

</details>

## Additional context

<!-- Screenshots, related issues, anything else. -->
