import pytest

from manyforge_behavior.cli import main


def test_migrate_pick_place_help_documents_composed_tcp_binding_refusal(capsys):
    with pytest.raises(SystemExit) as program_help_exit:
        main(["program", "--help"])

    assert program_help_exit.value.code == 0
    program_help = " ".join(capsys.readouterr().out.split())
    assert "migrate-pick-place" in program_help
    assert "holder lacks a composed tcp is refused at binding" in program_help
    assert "open it in Composer and choose the attach frame" in program_help

    with pytest.raises(SystemExit) as raised:
        main(["program", "migrate-pick-place", "--help"])

    assert raised.value.code == 0
    help_text = " ".join(capsys.readouterr().out.split())
    assert "catalog-2.2 composite Pick/Place nodes" in help_text
    assert "holder lacks a composed tcp is refused at binding" in help_text
    assert "Open the migrated Program in Composer and choose the attach frame" in help_text
    assert "never modifies the input or a Binding" in help_text
