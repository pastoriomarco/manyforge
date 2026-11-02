# Copyright 2026 Marco Pastorio
# SPDX-License-Identifier: Apache-2.0
uint8 NONE=0
uint8 IMAGE_BOX=1
uint8 SPACE_BOX=2
uint8 kind
# IMAGE_BOX: source pixels, x/y/width/height. Half-open extent.
float64[4] image_box
# SPACE_BOX: axis-aligned camera-frame bounds in metres.
string frame_id
geometry_msgs/Point minimum
geometry_msgs/Point maximum
