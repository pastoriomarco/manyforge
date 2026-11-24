# Build a Scenario from scratch

A Scenario is a Program (the behavior tree), a Deployment (the components of
the cell and their settings) and a Scene. This page builds one in the
workbench, starting from an empty cell, and saves it as a project. Loading an
example from `examples/` and changing it is the other way to start.

## Steps

1. **New** (top bar). The cell is emptied and the built-in Untitled Scenario
   is open: no components, an empty Program, an empty Scene. Save is disabled
   for Untitled; you save it in step 5.
2. **Add the components** (Components, **Add component**). Pick the kind and
   the entry, check the component id, then **Add to Draft**. A machine (for
   example a Lite 6) adds its owner and every member. For each component,
   choose its **Lane** under **Placement and lane** (a member runs on its
   owner's lane). Components of one Scenario may use different lanes, for
   example a Mock robot next to a Real I/O bank.
   - A robot comes with its controlled Stop settings filled in from its model
     (**Stop settings** on its page; see
     [Controlled Stop profiles](../operations/STOP_PROFILES.md)). Check them
     before you apply.
   - An I/O bank on the Real lane needs its PLC side running
     ([I/O banks](IO_BANKS.md)).
3. **Write the Program** (Program). Drag a node from the **Nodes** palette onto
   the canvas. A Program has exactly one root: to run several steps, start
   with a **Sequence** node and add the steps under it. Until the Program has a root,
   Apply stays disabled; hovering over it shows the reason.
   - **Program settings**, **Portable compatibility**: a new Program leaves
     the assistant **Optional** and names no requirements. Change them only to
     restrict where the Program may be applied.
4. **Apply** (top bar). Apply shows the Preview (components added, Program
   impact) and asks once to confirm. The components come up **Prepared**. If
   Apply is refused, see
   [When an Apply or Load fails](../operations/DEVELOPMENT_WORKBENCH.md#when-an-apply-or-load-fails).
5. **Save Scenario as** (top bar). Pick a project folder (on the development
   workbench, under `examples/`) and a name. The Scenario now opens from that
   folder, and Save writes there. Save writes what is applied: Apply later
   edits before you save them.
6. **Run it.** **Execute** (Program panel) offers **Activate and execute**
   while components are not enabled: it prepares and enables them, then runs
   the Program once. Motion starts held; press **Run** in the Motion panel.

## Next

- Doing these steps over the API instead of the GUI:
  [`scripts/smoke-new-scenario-api.py`](../../scripts/smoke-new-scenario-api.py)
  does steps 1 to 5 with a Mock Lite 6 and a Mock I/O bank, then loads the
  saved Scenario back and checks that the components are Prepared. Each step is one
  function that names the route it calls. Run it from the repository folder
  while the workbench runs (it empties the cell first):
  `python3 scripts/smoke-new-scenario-api.py --confirm-replace-cell`
- Signals and interrupt rules: [Interrupts](INTERRUPTS.md)
- Speeds and tolerances: [Motion defaults](MOTION_DEFAULTS.md)
- Load, New, Apply and Revert in detail:
  [Development workbench](../operations/DEVELOPMENT_WORKBENCH.md#load-new-activate-execute-run)
