"""Composer imports the shared Behavior authoring-forest contract."""

from manyforge_behavior.authoring_forest import (
    AUTHORING_FOREST_VERSION, AuthoringForestError, adopt_active_catalog,
    derive_candidate, empty_authoring, formula_references, from_program,
    program_formula_references, validate_invariants,
)

__all__ = [
    "AUTHORING_FOREST_VERSION", "AuthoringForestError", "adopt_active_catalog",
    "derive_candidate", "empty_authoring", "formula_references", "from_program",
    "program_formula_references", "validate_invariants",
]
