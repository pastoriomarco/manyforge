"""Canonical v1 source-envelope test data, verified by the real native reader.

This helper seals fixture bytes only; it is not a runtime publisher or owner.
Payload-specific qualification remains with each producer's contract tests.
"""

from hashlib import sha256

import rfc8785


def source_snapshot_text(value):
    source = dict(value)
    source.setdefault("api_version", "manyforge.source_snapshot.v1")
    source.setdefault("source_generation", 1)
    source.setdefault("observed_at_ns", 1)

    def digest(domain, payload):
        return (
            "sha256:"
            + sha256(
                b"\0".join(
                    (
                        b"manyforge.content-identity.v1",
                        domain.encode("ascii"),
                        source["api_version"].encode("ascii"),
                        rfc8785.dumps(payload),
                    )
                )
            ).hexdigest()
        )

    source["semantic_content_digest"] = digest(
        "manyforge-source-snapshot-semantic",
        {
            name: source[name]
            for name in (
                "payload",
                "source_owner_epoch",
                "source_owner_id",
                "source_owner_kind",
            )
        },
    )
    source.pop("record_digest", None)
    source["record_digest"] = digest("manyforge-source-snapshot-record", source)
    return rfc8785.dumps(source).decode("utf-8")
