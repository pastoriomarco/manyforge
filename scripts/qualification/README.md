# Explicit test PLC for R5 trials

The R5 trial Scenarios include an external ROS PLC dependency. Before activating
their `plc_io` component, open a separate terminal in the **same ROS container and
ROS environment** as the Workbench (including its dependency/application setup
scripts, ROS domain and discovery settings). Run from the repository root:

```bash
python3 scripts/qualification/r5_test_plc.py start \
  --response-delay-s 0.1 --log-dir /tmp/r5-test-plc-01
```

This explicit test fixture publishes Bool `true` at 10 Hz on `/plc_io/ready` and
serves SetBool `/plc_io/start`. Each response succeeds after the selected delay;
its message is the monotonic receive timestamp in nanoseconds. The publisher is
a separate process, so a delayed service response does not interrupt readiness
publication. For the authored 0.5 s acknowledgement window, a delay above that
window tests late acknowledgement; it does not change the Deployment timeout.

The helper refuses endpoints already discovered as occupied, prints both child
PIDs and log paths, and stays in the foreground. Discovery is a bounded preflight,
not an exclusive reservation: do not start another peer concurrently. Use a new
log directory each time. Observe actual component readiness before proceeding.
The helper sends no Apply, activation, Run, Reset or motion commands. A user
Activate+Execute already waiting for readiness can proceed when this peer starts.

**Cleanup:** after the test no longer needs PLC readiness, press Ctrl-C in that
terminal (or send SIGTERM to this helper). It terminates and waits for only its
two children, including when either child fails. Do not stop it during a Run
that still uses `plc_io`. It never starts automatically with the product and is
not a physical PLC implementation.
