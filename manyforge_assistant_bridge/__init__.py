"""Bounded assistant bridge plus its owner-neutral read client.

``bridge`` is the assistant-provider mediator. ``headless``
exposes the closed read client and ``protocol`` exports the strict current
wire.
"""

__all__ = ["bridge", "headless", "protocol"]
