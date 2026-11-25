"""Structured JSON logging configuration for ManyForge Composer.

The formatter is shared with the Headless API and the Program runner
(:mod:`manyforge_headless_api.logging_setup`); Composer logs as ``composer``.
"""
from __future__ import annotations

from manyforge_headless_api.logging_setup import JSONFormatter
from manyforge_headless_api.logging_setup import setup_logging as _setup_logging


def setup_logging(level: str = "INFO") -> None:
    """Configure root logger with JSON formatter on stderr."""
    _setup_logging(level, process="composer")


__all__ = ["JSONFormatter", "setup_logging"]
