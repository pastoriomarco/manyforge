# Copyright 2026 ManyForge contributors
# SPDX-License-Identifier: Apache-2.0
uint32 index
uint8 POSED=0
uint8 FAILED=1
uint8 NOT_ATTEMPTED=2
uint8 status
uint8 ESTIMATED=0
uint8 REFINED=1
uint8 REUSED=2
uint8 method
string reason
# Model pose in the source capture optical frame. Header is real source time.
geometry_msgs/PoseStamped pose
float64 estimate_ms
