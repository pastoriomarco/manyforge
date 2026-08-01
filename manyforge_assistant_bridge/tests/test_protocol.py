"""Pure assistant protocol tests retained across the H0 outage."""

import pytest

from manyforge_assistant_bridge.protocol import compute_catalog_hash


def test_catalog_hash_is_order_independent_within_each_family() -> None:
    left = compute_catalog_hash(
        {
            "skills": ["recover", "inspect"],
            "tools": ["program_read", "status_read"],
            "nodes": ["sequence", "timer"],
        },
        "composer-assistant",
    )
    right = compute_catalog_hash(
        {
            "nodes": ["timer", "sequence"],
            "tools": ["status_read", "program_read"],
            "skills": ["inspect", "recover"],
        },
        "composer-assistant",
    )

    assert left == right
    assert len(left) == 64


def test_catalog_hash_binds_the_assistant_mode() -> None:
    catalog = {"skills": [], "tools": ["status_read"], "nodes": []}

    assert compute_catalog_hash(catalog, "monitor") != compute_catalog_hash(
        catalog, "composer-assistant"
    )


@pytest.mark.parametrize(
    "catalog",
    [
        {"tools": ["status_read", "status_read"]},
        {"tools": [True]},
        {"tools": "status_read"},
        {"tools": ["x" * 257]},
        {"unknown": []},
    ],
)
def test_catalog_hash_rejects_ambiguous_or_unbounded_catalogs(catalog) -> None:
    with pytest.raises((TypeError, ValueError)):
        compute_catalog_hash(catalog, "monitor")


def test_catalog_hash_bounds_each_catalog_family() -> None:
    with pytest.raises(ValueError, match="512-item bound"):
        compute_catalog_hash(
            {
                "tools": [
                    f"tool-{index}"
                    for index in range(513)
                ]
            },
            "monitor",
        )


def test_catalog_hash_rejects_keys_before_rendering_or_comparing_them() -> None:
    class PoisonKey:
        def __hash__(self) -> int:
            return 1

        def __eq__(self, _other: object) -> bool:
            raise AssertionError("catalog key was compared")

        def __str__(self) -> str:
            raise AssertionError("catalog key was rendered")

    with pytest.raises(TypeError, match="family names must be strings"):
        compute_catalog_hash({PoisonKey(): []}, "monitor")


def test_catalog_hash_rejects_container_subclasses() -> None:
    class Catalog(dict):
        pass

    class Family(list):
        pass

    with pytest.raises(TypeError, match="catalog must be an object"):
        compute_catalog_hash(Catalog(), "monitor")
    with pytest.raises(TypeError, match="must be a list"):
        compute_catalog_hash({"tools": Family()}, "monitor")
