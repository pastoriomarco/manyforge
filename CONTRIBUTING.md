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

Keep these public documents current with your change. `THIRD_PARTY_NOTICES.md`
(and `NOTICE`) must match what the published ROS image redistributes: when you
add, remove or re-pin anything `Dockerfile.ros-lyrical-full` installs, update
them, quoting each licence as its owner declares it.
`THIRD_PARTY_NOTICES_MANYFORGE_IMAGE.md` must match what the ManyForge image
adds to it: update it when `Dockerfile.manyforge` adds third-party software
other than the frontend's npm packages, or changes where their licence files
go. The README overview cards
must stay accurate: when a change affects what they state, edit
`docs/media/src/overview_cards.py`, run `docs/media/src/render.sh` (needs
Firefox) and commit the regenerated SVGs and PNGs.

Behavior changes need focused regression coverage. Report checks that were
not run and why. Review substantial changes before submission, resolve findings
or document their disposition, and meet applicable CI requirements before merge.
Any temporary maintainer-only exception is defined in the development workflow,
not by an old public recipe.

## CI tiers

GitHub Actions runs in two tiers. Superseded runs on the same ref are cancelled.

- **Fast tier**, on every pull request and push to `dev`/`main`, with path
  filters so unrelated changes skip jobs, about 10–12 min per job:
  `bash-syntax`, `version-sync`, `private-spec-boundary`, `generated-checks`
  (error catalog, `scripts/regen.py --check`), `frontend-build`
  (`tsc -b --force`, vitest, bundle), `python-tests` (one parallel job per
  package, `pytest -n auto` where tests are independent) and `cpp-unit`
  (ROS-free C++ packages, Release; `manyforge_core` is built once and its
  tests run in four shards). `python-tests` and `cpp-unit` also run nightly on
  `dev`, which refreshes their compiler cache for pushes and pull requests.
- **Heavy tier**, nightly on `dev`, on release tags `v*` and on demand:
  `cpp-asan`, `cpp-tsan`, `cpp-stability-gate` and `python-nightly` (core
  compiler bindings and the Python tests marked `@pytest.mark.slow`). Mark a
  test `slow` to move it from pull requests to the nightly run; every test
  still runs somewhere. Start a heavy workflow by hand with
  `gh workflow run <name>.yml --ref dev` or from the Actions tab.
- **Nightly runs only after a change:** every scheduled workflow first calls
  `nightly-dev-changed`, and skips its jobs unless `dev` was pushed after that
  workflow's previous scheduled run started. A failure is therefore reported
  once, not every night. Manual, tag, push and pull-request runs always run.
- **ROS tier**, after each release image and on demand: `ros-image` builds and
  publishes the reference ROS image, `ghcr.io/pastoriomarco/manyforge-ros`, for
  amd64 and arm64, on every `v*` tag push (also tagging it with the release
  version) and by hand (`gh workflow run ros-image.yml --ref <ref>`); a run
  whose image inputs are already published builds nothing. When it succeeds,
  `ros-integration` runs the ROS integration and ros2_control Mock-lane suites
  (`test-all.sh ros` and `ros-e2e`) in that image on the same commit; it never
  builds the image and does not run nightly on `dev`. Between releases, run the
  same suites locally (`local-ci`, below).
- **`local-ci`** runs every lane, including the ROS integration and e2e
  suites, in the development containers on a self-hosted runner labelled
  `manyforge-local`. It starts on release tags and on demand only, never on
  pull requests.

The ROS-backed TSan job in `local-ci` is staged with an explicit hold; a later
release removes it after one manual run on the local runner. It runs the ROS execution backend,
gripper backend, kernel-stop integration and retained-runtime-handle binaries
once each in separate processes, without suppressions. For each TSan report,
the first relevant frame in each conflicting access and allocation stack is
checked after sanitizer/interceptor, allocation, and standard-library
allocator/container frames are skipped. A ManyForge frame there, or a
ManyForge global location, fails the job. Other reports, including truncated
stacks, appear as **“SDK race, to
classify”** warnings; retain their complete logs and assess them for the
upstream findings. A deeper ManyForge caller alone does not establish that an
SDK access or allocation is ManyForge-owned. Test failures and TSan runtime
failures also fail the job. The staged job has not yet qualified the release.

## Agent authorization

Agents must read [AGENTS.md](AGENTS.md). A code-change request does not authorize
committing, pushing, creating a PR or releasing: each needs an explicit request.
Never push directly to `main`.

## Releases

Releases are maintainer-driven. Agents must not perform release actions
without explicit authorization.

1. Set the root `VERSION` and every package version;
   `scripts/ci/verify-version-sync.sh` must pass. Regenerate the committed
   outputs (`python3 scripts/regen.py --update`, then `--check`).
2. Give the release its section in `CHANGELOG.md` (version and date).
3. Check the companion pins: the xArm driver fork commit
   (`XARM_SOURCE_COMMIT` in `Dockerfile.ros-lyrical-full`, the same commit in
   `manyforge_xarm_control/vendor/driver.yaml`) is on the fork's
   `lyrical-no-gazebo` branch. For `v0.4.0`:
   `3654c9afef9c51c3bd6723d65ccecca166bc38b0`.
4. Run the applicable gates, squash-merge `dev` into `main` and tag
   `vX.Y.Z` (for this release `v0.4.0`; `v0.3.1` is `main` at `9e7f528f`,
   `v0.3.0` is `main` at `3fc751bc`,
   `v0.2.1` is `main` at `34bb5f4a`, `v0.2.0` is `main` at `3cc3e5d5`,
   `v0.1.0` is `main` at `8ae05265`).
5. Pushing the tag publishes the images; there is no manual image step. The
   `ros-image` workflow runs on the tag and publishes
   `ghcr.io/pastoriomarco/manyforge-ros:<VERSION>`, the image
   `./scripts/compose-dev.sh pull` fetches (when its inputs are unchanged it
   builds nothing and only adds the version tag). When it succeeds, the
   `manyforge-image` workflow builds ManyForge from the tagged commit on that
   ROS image (by digest) and publishes
   `ghcr.io/pastoriomarco/manyforge:<VERSION>`, the image `./manyforge run`
   starts. Check both runs. To run either again by hand:
   `gh workflow run ros-image.yml --ref vX.Y.Z` (its success starts
   `manyforge-image` again) or `gh workflow run manyforge-image.yml --ref vX.Y.Z`.
6. Tag the maintainer's private specification repository with the same `vX.Y.Z`
   at the commit that matches the release.

## Reporting issues

- Bugs: use the issue template and include the version, deployment/scenario,
  reproduction steps and relevant logs.
- Security: follow [SECURITY.md](SECURITY.md); do not disclose vulnerabilities
  in public issues.
- Broad feature or architecture proposals: discuss scope with the maintainer
  before implementation.

By contributing, you agree that contributions are licensed under the
[Apache License 2.0](LICENSE).
