"""ManyForge assistant-provider bridge service (v0.1 skeleton).

Implements the assistant-provider HTTP contract (per
`manyforge/docs/reference/ASSISTANT_BACKEND_CONTRACT.md` and
`manyforge_specs/docs/spec/480-assistant-modes-and-bounded-autonomy.md`)
against a local OpenAI-compatible LLM endpoint (typically vLLM on the
deployment host).

See `manyforge_specs/docs/spec/485-assistant-bridge-architecture.md`
for the durable architectural design.
"""

__all__ = ["bridge"]
