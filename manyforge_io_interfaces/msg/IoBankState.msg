# Copyright 2026 Marco Pastorio
# SPDX-License-Identifier: Apache-2.0
# One I/O bank's state, device to ROS, published periodically (default 200 Hz).
uint8 RESULT_UNSET=0
uint8 RESULT_ACCEPTED=1
uint8 RESULT_REFUSED=2
# +1 each time the device's program refreshes this state; wraps.
uint32 cycle
# seq of the last command the device decided.
uint32 applied_seq
# RESULT_* for that command: ACCEPTED when no output was refused, REFUSED when any was.
uint8 applied_result
# The device's own reason for that result, 0 = none.
uint16 device_code
# Index = signal index.
bool[64] inputs
# The device's physical output values (read-back).
bool[64] outputs
# The outputs of the command applied_seq that the device refused (index = signal index).
bool[64] refused
