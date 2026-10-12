# Copyright 2026 ManyForge contributors
# SPDX-License-Identifier: Apache-2.0
uint32 index
float32 detection_score
# Source pixels, x/y/width/height; not the encoder's letterboxed box.
float64[4] bbox
bool selected
