#!/usr/bin/env python3
"""Offline CLI for the shared structural-model source preparer."""

from __future__ import annotations

import sys
from pathlib import Path


# CMake invokes this source-tree tool before Python packages are necessarily
# installed. Import the same package module used by Headless; this file owns no
# preparation semantics.
_PACKAGE_ROOT = Path(__file__).resolve().parents[2] / "manyforge_behavior"
sys.path.insert(0, str(_PACKAGE_ROOT))

from manyforge_behavior.model_source_preparation import (  # noqa: E402,F401
    CONFIGURATION_IDENTITY_DOMAIN,
    CONTENT_IDENTITY_PREFIX,
    GENERATOR_ID,
    GENERATOR_VERSION,
    PREPARATION_API_VERSION,
    PreparedResourceReference,
    PreparedStructuralModel,
    StructuralModelPreparationError,
    main,
    prepare_structural_model,
    qualify_captured_structural_model_preparation,
    qualify_structural_model_preparation,
)


if __name__ == "__main__":
    raise SystemExit(main())
