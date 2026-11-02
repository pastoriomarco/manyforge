"""Bounded assistant bridge plus owner-neutral client/reducer inputs.

``bridge`` is the current direct/``nemoclaw`` provider mediator. ``headless``
exposes the closed H0 read client, ``protocol`` exports the strict current wire,
and ``supervisor`` keeps only the pure reducer from the superseded fixed
launcher.
"""

__all__ = ["bridge", "headless", "protocol", "supervisor"]
