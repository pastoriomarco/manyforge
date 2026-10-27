# Fresh-checkout setup checklist

Use this checklist on a Linux host where it is safe to start the mock
workbench. It checks the documented source setup without a physical robot or
Isaac Sim. The [workbench guide](DEVELOPMENT_WORKBENCH.md) is the source for
prerequisites, commands and troubleshooting.

1. Start from a fresh clone. Record the checked-out commit, host architecture,
   Linux version and Docker/Compose versions with the test result.
2. Follow the guide's prerequisites and image-build instructions exactly.
   Record the command and whether the image build succeeds.
3. Follow the guide to start an empty workbench. Record the start result and
   verify that `./scripts/compose-dev.sh status` reports it ready.
4. Open `http://localhost:3000`. Verify that the page opens with an empty
   workspace and without an unresolved startup error.
5. In Composer, load `examples/lite6_pick_and_place.scenario` and verify the
   Program, Scene and Deployment appear. Check that the active configuration
   and component status are shown. Record any error rather than retrying an
   action with an unknown outcome.
6. Use the guide's stop command. Confirm that the workbench is stopped. Start
   it again, open Composer and confirm the workspace is usable after the page
   is refreshed. Stop the workbench when finished.
7. Record each step as pass, fail or not run, with the error and the command
   output needed to diagnose a failure. Do not claim Isaac or hardware
   qualification from this mock setup check.

The workbench normally keeps up to five previous sessions under
`.manyforge-dev/runtime/<container-name>/previous-sessions/`. If
`MANYFORGE_DEV_CACHE_BASE` is set, it replaces `.manyforge-dev` in that path.
`MANYFORGE_DEV_RUNTIME_STATE_ROOT`, when set, takes precedence over the cache
base and directly names the runtime root containing `previous-sessions/`.
Cleanup is deferred while a component incident is open or unknown. Unimported
records, including safety records, are not guaranteed to survive retention:
copy any sessions you want to keep elsewhere.

If session-count cleanup encounters more than 1,000 entries in
`previous-sessions/`, `session-reports/` or `session-reports/pending/`, it
warns and pauses. With the workbench stopped, inspect the backlog and copy
records you need. Preserve
the current session, pending reports, incomplete selections, and records
associated with an open or unknown incident.
