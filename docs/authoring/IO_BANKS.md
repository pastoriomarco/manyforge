# I/O banks: a PLC as an I/O device

An **I/O bank** is a block of up to 64 digital inputs and 64 digital outputs
of one device, such as a PLC, read and written over two ROS 2 topics. Each
bank is one component of the Deployment. A PLC with more signals has more
banks, one component each.

Programs, rules and the I/O panel use a bank's signals like any other:
`<component_id>/<signal_name>`, for example `plc_io/I3`. Outputs show the
value the device reports (read-back): `manyforge_write` after ManyForge's own
accepted write, `outside` when the device or its HMI changed it, with the
output's `changed_outside` signal set.

## Declare the bank's model

A bank is a model-form I/O device (`kind: io_device` with `lanes`, see
[Adding a component](ADDING_A_COMPONENT.md#add-an-io-device)). Every signal
has an `index`: its position in the bank's arrays, 0 to 63, unique per
direction. The real lane names the `ros_io_bank` adapter family and the
device's state rate.

ManyForge ships a generic model with terminal names,
`components/io_device/manyforge.io_bank_64x64` (inputs `I0`-`I63`, outputs
`Q0`-`Q63`). A site model gives the signals its own names:

```yaml
api_version: manyforge.component_package.v2
kind: io_device
id: acme.press_plc_bank
version: 1.0.0
title: Press PLC, bank 1
license: Apache-2.0
signals:
  - {signal_name: part_present, direction: input, value_type: bool, index: 0}
  - {signal_name: door_closed, direction: input, value_type: bool, index: 1}
  - {signal_name: clamp, direction: output, value_type: bool, index: 0}
  - {signal_name: blow_off, direction: output, value_type: bool, index: 5}
lanes:
  # rate_hz: the device's state rate. input_maximum_age_ms: ten periods.
  real: {adapter: ros_io_bank, rate_hz: 200, input_maximum_age_ms: 50}
```

Create it with `scripts/manyforge component new io_device acme.press_plc_bank`,
edit it, then `scripts/manyforge component check` and `register` it. Apply
refuses a bank whose signals lack an `index`, repeat one in a direction or use
one outside 0..63, and a `rate_hz` that isn't above 0 and at most 1000.

## Add it to the Deployment

Add the bank from **Add component** (the shipped model is "ROS I/O bank, 64
inputs and 64 outputs"), or as a row of its own (no owner):

```yaml
- component_id: plc_io
  component_profile_id: manyforge.io_bank_64x64
  lane: real
  configuration: {}
```

On the Mock and Isaac lanes the host's Mock I/O stands in for the device:
outputs read back what was written and inputs are set from the I/O panel. On
the real lane the `ros_io_bank` family talks to the device over the topics
below. The device's bridge runs outside ManyForge; ManyForge doesn't start it.

## Topics and messages

Package `manyforge_io_interfaces` (in the ManyForge repository; it builds
alone with `rosidl_default_generators`). Names are fixed under the component
id:

| Topic | Type | Direction | Rate |
| --- | --- | --- | --- |
| `/<component_id>/io_bank/state` | `manyforge_io_interfaces/msg/IoBankState` | device to ManyForge | the model's `rate_hz` (200 Hz in the shipped model) |
| `/<component_id>/io_bank/command` | `manyforge_io_interfaces/msg/IoBankCommand` | ManyForge to device | one message per command |

```text
# IoBankState
uint8 RESULT_UNSET=0
uint8 RESULT_ACCEPTED=1
uint8 RESULT_REFUSED=2
uint32 cycle            # +1 each time the device's program refreshes the state; wraps
uint32 applied_seq      # seq of the last command the device decided
uint8 applied_result    # ACCEPTED: no output refused; REFUSED: any refused
uint16 device_code      # the device's own reason for that result, 0 = none
bool[64] inputs         # index = signal index
bool[64] outputs        # the physical output values (read-back)
bool[64] refused        # the outputs of command applied_seq the device refused

# IoBankCommand
uint8 OUTPUT_OFF=0
uint8 OUTPUT_ON=1
uint8 OUTPUT_KEEP=2
uint32 seq
uint8[64] values        # OFF/ON for the outputs it sets, KEEP for all others
```

ManyForge subscribes to the state best-effort with a depth of 1 (a reliable
publisher matches) and publishes commands reliable with a depth of 1.

### What ManyForge expects of the device

- **State:** `cycle` grows by one per refresh. A sample with the same `cycle`
  as the previous one is not new; ten periods without a new sample make the
  stream stale, the component faults (`fault_stream_stale`, Recover) and
  motion stops. While samples keep arriving with a frozen `cycle` the fault
  carries `vendor_code: cycle_stopped`. The fault ends when new samples
  return. A `cycle` that goes back means the device restarted: values are
  read afresh and nothing is re-sent.
- **Writes:** one command in flight per bank. Writes that arrive while a
  command is in flight wait; the next command carries all of them, at most
  one per output (a second write to the same output waits for the command
  after, so a pulse on then off survives, in order). Only the bank's
  declared outputs are ever OFF or ON. ManyForge publishes each command
  once; the device applies a command whose `seq` differs from its
  `applied_seq`, then reports it in the state. At the first sample with
  `applied_seq == seq`:
  - with `RESULT_ACCEPTED` or `RESULT_REFUSED`, each write whose output is
    not in `refused` succeeded (that sample's `outputs` is the read-back);
    each refused one failed (`io_output_source_rejected`,
    `vendor_code: device_code=<n>`);
  - any other result: every write in the command is unknown
    (`io_output_acceptance_unknown`).
  - No `applied_seq == seq` within 40 device cycles, before the stream goes
    stale or before the device restarts: every write in the command is
    unknown. A command is never re-sent; its outputs stay unknown until the
    next sample reports them.
- **Order inside the device:** write the output values first, then
  `refused` and the result, then `applied_seq`; fill the state with `applied_seq` first and the
  output values last. Then a sample can show "old sequence, new value"
  (ManyForge waits) but never "new sequence, old value" (which would read as
  an outside change).
- **Refuse what you won't drive:** `outputs` is the physical value. An
  output the PLC program will not drive (an interlock) is refused (its
  `refused` bit and a `device_code`), not accepted and then held off.
- **Sequence numbers:** ManyForge starts each device epoch at the first
  sample's `applied_seq + 1` and counts up (it wraps), never sending a
  number equal to the device's last `applied_seq`.

**Latency measurement:** with the execution host's log level at debug,
every command logs one `io_bank command` line with its `seq`, the number of
outputs it set, `sent_at_ns`, `decided_at_ns`, `latency_ms` and outcome.

## Simulated PLC

`sim_plc` serves the device side for tests and for a real-lane Deployment
without hardware. Start it by hand next to ManyForge, with
`<config_dir>/plc_io.yaml` containing `mode: bank` (optional `rate_hz`, up to
1000, default 200; `mirror: true` loops each output back to the input of the
same index):

```bash
ros2 run manyforge_ros_bringup sim_plc --device plc_io --config-dir <config_dir>
```

It follows the device rules above, including the start-up guard. Test
switches are ROS parameters of `/manyforge/sim_plc_plc_io`:
`bank.response` (`accept`, `no_answer`), `bank.refused` (the output indexes
it refuses, e.g. `"2,5"`), `bank.device_code` (the code of a command with a
refusal),
`bank.apply_delay_cycles` (a late effect), `bank.publishing` (false: the
stream stops), `bank.frozen` (true: samples repeat with a frozen `cycle`) and
`bank.restart` (true: the PLC restarts).

## Example: a Siemens PLC through ROXSIE

ROXSIE (Siemens' ROS 2 PLC connector) generates the PLC blocks and a ROS 2
bridge node from a YAML file. Give it the `manyforge_io_interfaces` package
as its custom-interface workspace. One bank, `plc_io`, at 200 Hz (5 ms), both
topics in cyclic interrupt OBs (`priority: 1`):

```yaml
system:
  ros2: {package_name: roxsie_plc_io, node_name: plc_io_node}
  rib: {app_ipv4_address: "127.0.0.1", app_port: 27567, version: "v2.2.4"}
topics:
  - type: "manyforge_io_interfaces/msg/IoBankState.msg"
    name: "/plc_io/io_bank/state"
    provider: "plc"
    rate: 200.0
    priority: 1
  - type: "manyforge_io_interfaces/msg/IoBankCommand.msg"
    name: "/plc_io/io_bank/command"
    provider: "ros2"
    rate: 200.0
    priority: 1
```

The generator drops the leading `/` and turns each further `/` into `__`:
the state is filled in DB `ToRos2_plc_io__io_bank__state` (the user section
of its OB, before the copy) and the command arrives in DB
`FromRos2_plc_io__io_bank__command` (the user section of its OB, after the
copy). `bool[64]` becomes `Array[0..63] of Bool`, `uint8[64]`
`Array[0..63] of USInt` (`uint8` maps to `USInt`); the messages' constants
become members with start values.

**User code.** One global data block `IoBank`, not retain:
`q : Array[0..63] of Bool`, `refused : Array[0..63] of Bool`,
`applied_seq : UDInt`, `applied_result : USInt`, `device_code : UInt`,
`cycle : UDInt`, `first_command : Bool`.

```scl
// OB100 (start-up)
"IoBank".first_command := TRUE;

// Command topic's OB, user section after the copy
IF "IoBank".first_command THEN
    // After a start the command DB is refilled with the bridge's last
    // command: adopt its seq, never apply it again.
    "IoBank".applied_seq := "FromRos2_plc_io__io_bank__command".seq;
    "IoBank".first_command := FALSE;
ELSIF "FromRos2_plc_io__io_bank__command".seq <> "IoBank".applied_seq THEN
    #any_refused := FALSE;
    FOR #i := 0 TO 63 DO
        "IoBank".refused[#i] := FALSE;            // cleared per new command
        #v := "FromRos2_plc_io__io_bank__command".values[#i];
        IF #v = 0 OR #v = 1 THEN                           // 2 (KEEP), others: unchanged
            IF "WriteAllowed"(#i) THEN
                "IoBank".q[#i] := (#v = 1);       // 1st: the values
            ELSE
                "IoBank".refused[#i] := TRUE;     // won't drive it
                #any_refused := TRUE;
            END_IF;
        END_IF;
    END_FOR;
    IF #any_refused THEN                          // then the result
        "IoBank".applied_result := 2;                     // REFUSED
        "IoBank".device_code := 2;                        // site code: interlocked
    ELSE
        "IoBank".applied_result := 1;                     // ACCEPTED
        "IoBank".device_code := 0;
    END_IF;
    "IoBank".applied_seq := "FromRos2_plc_io__io_bank__command".seq;  // last
END_IF;

// State topic's OB, user section before the copy
"IoBank".cycle := "IoBank".cycle + 1;
"ToRos2_plc_io__io_bank__state".applied_seq := "IoBank".applied_seq;  // 1st
"ToRos2_plc_io__io_bank__state".applied_result := "IoBank".applied_result;
"ToRos2_plc_io__io_bank__state".device_code := "IoBank".device_code;
"ToRos2_plc_io__io_bank__state".cycle := "IoBank".cycle;
"ToRos2_plc_io__io_bank__state".refused := "IoBank".refused;
"ToRos2_plc_io__io_bank__state".inputs[0] := "PartPresent";   // one line per input
"Valve1" := "IoBank".q[0];                                    // one line per output
"ToRos2_plc_io__io_bank__state".outputs[0] := "Valve1";       // last: the real values
```

`#i : Int`, `#v : USInt` and `#any_refused : Bool` are temporaries of the
command OB. `"WriteAllowed"` stands for the
site's own interlock check; use `TRUE` if every output may be written.

**DDS on the PLC's host.** ManyForge runs Fast DDS over UDP only (its
launcher's default, `FASTDDS_BUILTIN_TRANSPORTS=UDPv4`). Set the same
variable in the bridge's compose file: the bridge runs as its own user with
the host's `/dev/shm`, and Fast DDS shared-memory files left by one user can
break same-host DDS for processes of another user without an error. RIB's own
shared memory, between the bridge and the PLC, is not affected.
