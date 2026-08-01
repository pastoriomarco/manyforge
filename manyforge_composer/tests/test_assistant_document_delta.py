"""Pure assistant authoring-document reducer coverage."""

from manyforge_composer.backend._assistant_delta import compute_document_delta


def test_document_delta_is_explicit_stateless_and_transport_neutral() -> None:
    before = {"id": "sequence", "name": "root", "children": []}
    after = {
        "id": "sequence",
        "name": "root",
        "children": [{"id": "wait", "name": "pause", "params": {"s": 1}}],
    }

    result = compute_document_delta(
        before_tree=before,
        before_scene=[],
        after_tree=after,
        after_scene=[],
    )

    assert result["root_name"] == "root"
    assert result["tree"]["added"] == [
        {"name": "pause", "id": "wait", "parent": "root", "position": 0}
    ]
    assert result["unchanged"] is False
    assert "sinceCallStart" not in result
    assert "sinceConversationStart" not in result
