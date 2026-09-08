from manyforge_behavior.program_diagnostics import diagnostic_values


def test_small_values_are_copied_and_large_values_are_explicitly_omitted():
    original = {"pose": [1, 2, 3], "large": "x" * 100_000, "other": False}
    result = diagnostic_values(original.items(), total_count=len(original))
    original["pose"][0] = 9
    assert result == {"values": {"pose": [1, 2, 3], "other": False}, "omitted_count": 1}


def test_capture_budgets_bound_entries_depth_traversal_and_bytes():
    inspected = []

    def rows():
        for i in range(10000):
            inspected.append(i)
            yield str(i), i

    assert len(diagnostic_values(rows(), total_count=10000)["values"]) == 32
    assert len(inspected) == 32
    nested = []
    for _ in range(100):
        nested = [nested]
    result = diagnostic_values(
        [("deep", nested), ("wide", [1] * 10000), ("real", 7)], total_count=3
    )
    assert result == {"values": {"real": 7}, "omitted_count": 2}
    values = diagnostic_values(((str(i), "x" * 2000) for i in range(100)), total_count=100)
    assert len(values["values"]) == 8
    assert values["omitted_count"] == 92


def test_oversized_compounds_are_rejected_before_large_serialization(monkeypatch):
    import rfc8785

    encoded_sizes = []
    dumps = rfc8785.dumps

    def bounded_dumps(value):
        encoded = dumps(value)
        encoded_sizes.append(len(encoded))
        assert len(encoded) <= 16 * 1024
        return encoded

    monkeypatch.setattr(rfc8785, "dumps", bounded_dumps)
    rows = ((str(i), ["\x01" * 2048] * 511) for i in range(32))
    assert diagnostic_values(rows, total_count=32) == {
        "values": {},
        "omitted_count": 32,
    }
    assert len(encoded_sizes) <= 32 * 4


def test_one_nonportable_value_does_not_lose_the_other_captured_facts():
    values = {"text": "cafe\u0301", "too_large": float(2**53), "ready": True}
    assert diagnostic_values(values.items(), total_count=3) == {
        "values": {"ready": True},
        "omitted_count": 2,
    }


def test_near_limit_capture_validity_survives_canonical_key_ordering():
    import json
    from manyforge_behavior.canonical_artifact import canonical_json_bytes

    values = {
        "z": [0] * 100,
        **{f"a{i}": "x" * 2040 for i in range(7)},
        "a7": "x" * 1761,
    }
    capture = diagnostic_values(values.items(), total_count=len(values))
    assert capture["omitted_count"] == 0
    canonical = json.loads(canonical_json_bytes(capture["values"]))
    assert list(canonical) != list(values)
    assert diagnostic_values(canonical.items(), total_count=len(canonical)) == capture
