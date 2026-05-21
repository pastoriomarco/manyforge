<!--
Thank you for opening a pull request to ManyForge!

This PR should target `dev` (never `main`). Releases happen via a
maintainer-driven squash-merge from `dev → main`.

If you're an LLM agent: confirm you have explicit human authorization
for the commit(s) on this branch. See AGENTS.md and CONTRIBUTING.md.
-->

## Summary

<!-- One or two sentences. What does this PR do and why? -->

## Type of change

- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality not to work as expected)
- [ ] Documentation only
- [ ] Refactor / internal cleanup (no user-visible change)
- [ ] CI / tooling

## Checklist

- [ ] PR targets `dev` (not `main`); topic branch name `feature/<x>`, `fix/<x>`, or `docs/<x>` if using one
- [ ] Tests added or updated for the behavior change
- [ ] `CHANGELOG.md` updated under the upcoming release section if user-visible
- [ ] `./scripts/ci/verify-version-sync.sh` passes locally
- [ ] Bash files I touched pass `bash -n`
- [ ] Frontend changes pass `cd manyforge_composer/frontend && npm run build`
- [ ] AGENTS.md / CONTRIBUTING.md / README updated if workflow changed
- [ ] **Launcher behavior change** (`scripts/launch.sh`, `scripts/lib/*.sh`, `scripts/lib/scenarios.sh`, or any `scripts/demo-*.sh` shim): `docs/operations/LAUNCHER_QUICKSTART.md` updated in this PR
- [ ] For non-trivial diffs: `/codex:review` run and findings addressed
- [ ] Spec-governed behavior checked against `manyforge_specs`, or the PR clearly states that maintainer spec audit is still required
- [ ] No links to private spec paths (`manyforge_specs/`) introduced

## Test plan

<!-- How you verified this works end-to-end. Reproduction steps,
     scenarios run, lanes exercised (manyforge-only / isaac-sim). -->

## Related issues / PRs

<!-- "Fixes #N", "Refs #N", or a link to a NemoClaw-Thor PR if this
     change crosses the repo boundary. -->
