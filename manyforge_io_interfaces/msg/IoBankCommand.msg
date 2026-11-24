# Copyright 2026 Marco Pastorio
# SPDX-License-Identifier: Apache-2.0
# One command of output writes, ROS to device. The device applies it when seq
# differs from its applied_seq: each value OFF or ON sets that output, KEEP
# (or any other value) leaves it unchanged; then it reports seq as
# applied_seq in IoBankState.
uint8 OUTPUT_OFF=0
uint8 OUTPUT_ON=1
uint8 OUTPUT_KEEP=2
uint32 seq
# Index = signal index: OFF/ON for the outputs this command sets, KEEP for all others.
uint8[64] values
