#!/usr/bin/env python3
"""Typed I9 gate for the historical live Isaac collider probe CLI.

The former wrapper is pinned at blob
``f1b78ac419838a403ae74753749a55bbb30b629a`` and its 2,793-line live probe at
blob ``380c7dc4ff873aa510290b5f6df5a080e9698ab0``.  The imported ``main`` now
rejects activation before parsing arguments, importing Isaac, opening a stage,
creating temporary prims, or writing reports/meshes.
"""

from __future__ import annotations

from isaac_collider_probe_lib import main


if __name__ == "__main__":
    raise SystemExit(main())
