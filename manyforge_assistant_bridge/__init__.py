"""ManyForge assistant-provider bridge service (v0.1 skeleton).

Implements the assistant-provider HTTP contract against a local
OpenAI-compatible LLM endpoint (typically vLLM on the deployment host).

Operator-facing wire contract: `docs/reference/ASSISTANT_PROVIDER_CONTRACT.md`.
"""

__all__ = ["bridge"]
