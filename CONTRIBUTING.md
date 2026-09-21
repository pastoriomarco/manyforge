# Contributing to ManyForge

This is the public contribution entrypoint. Development policy and detailed
implementation procedures are reached through [AGENTS.md](AGENTS.md);
they are not duplicated in this deployment repository.

## Contributions and branches

- `dev` is the integration branch; `main` contains releases.
- Only the maintainer pushes to upstream `dev`. External contributors use a
  fork and submit a pull request targeting upstream `dev`.
- Keep existing work when updating a checkout. Do not reset away local changes.
- Use descriptive commit prefixes such as `fix:`, `feat:`, `docs:`,
  `refactor:` and `test:`.
- Include the behavior changed, relevant checks and their results, and any
  known limitations in the contribution.
- Maintainer development follows the authoritative instruction chain linked
  from AGENTS.md. Contributors without access can report issues and submit
  bounded fixes; contract or architecture changes need maintainer review.
- Public issues and PRs describe behavior without exposing private document
  contents or paths.

## Validation and review

Use the repository's checked-in CI and build/test scripts for affected checks.
Implementation builds and tests run in the matching development container;
host-only results are not qualification evidence. Detailed commands, lane
selection and checkpoint requirements belong in the development workflow
linked from AGENTS.md.

Behavior changes need focused regression coverage. Report checks that were
not run and why. Review substantial changes before submission, resolve findings
or document their disposition, and meet applicable CI requirements before merge.
Any temporary maintainer-only exception is defined in the development workflow,
not by an old public recipe.

## Agent authorization

Agents must read [AGENTS.md](AGENTS.md). A code-change request does not authorize
committing, pushing, creating a PR or releasing: each needs an explicit request.
Never push directly to `main`.

## Releases

Releases are maintainer-driven: update the root `VERSION` and changelog,
validate package-version consistency and applicable gates, squash-merge
`dev` into `main`, then tag. Agents must not perform release actions without
explicit authorization. `scripts/ci/verify-version-sync.sh` checks the
root version against package versions.

## Reporting issues

- Bugs: use the issue template and include the version, deployment/scenario,
  reproduction steps and relevant logs.
- Security: follow [SECURITY.md](SECURITY.md); do not disclose vulnerabilities
  in public issues.
- Broad feature or architecture proposals: discuss scope with the maintainer
  before implementation.

By contributing, you agree that contributions are licensed under the
[Apache License 2.0](LICENSE).
