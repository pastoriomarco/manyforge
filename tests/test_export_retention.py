from __future__ import annotations
import importlib.util
import io
import json
import os
from pathlib import Path
import sqlite3
import sys
import pytest

spec = importlib.util.spec_from_file_location('export_retention', Path(__file__).parents[1] / 'scripts/lib/export_retention.py')
assert spec and spec.loader
retention = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = retention
spec.loader.exec_module(retention)


def journal(root, acknowledged=1):
    path = root / 'execution-state/cclc-journal.sqlite3'
    path.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(path) as db:
        db.execute('CREATE TABLE cclc_component_incidents(acknowledged INTEGER)')
        db.execute('INSERT INTO cclc_component_incidents VALUES (?)', (acknowledged,))
    return path


def capture(root, index):
    if not (root/'execution-state/cclc-journal.sqlite3').exists():
        journal(root)
    folder = root / 'evidence' / f'20261013T0000{index:02}Z'
    folder.mkdir(parents=True)
    (folder / 'docker-inspect.json').write_text(json.dumps({'container_id':'a'*64,'image':'sha256:'+'b'*64,'mounts':[{'Type':'bind','Source':str(root),'Destination':'/tmp/manyforge-state'},{'Type':'bind','Source':'/workspace-owner','Destination':'/workspace'}],'manyforge_export_kind': 'console_tail', 'state': {'running': False}}))
    (folder / 'docker.log').write_bytes(b'console' * 1000)
    os.utime(folder, ns=(index + 1, index + 1))
    return folder


def epoch(root, index):
    folder = root / f'execution-run/controller-launch-artifacts/bundle/epoch{index}'
    component = folder / 'arm'
    component.mkdir(parents=True)
    expected = Path('/tmp/manyforge-state') / component.relative_to(root)
    (component / 'controller_launch_artifacts.json').write_text(json.dumps({
        'controllable_component_id': 'arm', 'robot_description_file': str(expected / 'robot_description.urdf'),
        'robot_description_sha256': 'a'*64, 'controllers_file': str(expected / 'controllers.yaml'), 'controllers_sha256': 'b'*64}))
    (component / 'robot_description.urdf').write_text('robot')
    (component / 'controllers.yaml').write_text('controllers')
    # Model the producer marker write chronology, independent of directory entries.
    marker = component / 'controller_launch_artifacts.json'
    written = 1791849600000000000 + index * 1000000000
    os.utime(marker, ns=(written, written))
    os.utime(folder, ns=(index+1, index+1))
    return folder


def pressure(root, **options):
    return retention.rotate_retained_logs(root, retention.SiteRetentionPolicy(10, 10), disk_facts=lambda _: retention.DiskFacts(1, 0), **options)


def test_tail_keeps_exact_last_bytes_of_arbitrarily_long_line(tmp_path):
    target = tmp_path / 'tail'
    retention.capture_tail(io.BytesIO(b'prefix'+b'x'*200000+b'end'), target, limit=100000)
    assert target.read_bytes() == b'x'*99997+b'end'
    with pytest.raises(FileExistsError):
        retention.capture_tail(io.BytesIO(b'replacement'), target)


def test_policy_defaults_and_invalid_values():
    assert retention.SiteRetentionPolicy.from_environment({}) == retention.SiteRetentionPolicy(10, 10_000_000_000)
    with pytest.raises(ValueError):
        retention.SiteRetentionPolicy.from_environment({'MANYFORGE_LOG_EVIDENCE_MAX_DISK_PERCENT': '0'})


def test_pressure_deletes_only_superseded_closed_units_oldest_first(tmp_path):
    root = tmp_path / 'runtime'
    durable = journal(root)
    before = durable.read_bytes()
    old, latest = capture(root,1), capture(root,2)
    old_epoch, latest_epoch = epoch(root,3), epoch(root,4)
    result = pressure(root)
    assert result.deleted == (str(old.relative_to(root)),str(old_epoch.relative_to(root)))
    assert latest.exists() and latest_epoch.exists() and durable.read_bytes() == before
    assert 'only protected material remains' in result.warning and 'latest controller-launch' in result.warning


@pytest.mark.parametrize('state',['open','missing','corrupt','wal'])
def test_open_or_unknown_incident_protects_all_epochs(tmp_path,state):
    root = tmp_path/'runtime'
    first,latest = epoch(root,1),epoch(root,2)
    if state != 'missing':
        path = journal(root,0 if state=='open' else 1)
        if state=='corrupt': path.write_text('not a journal')
        elif state=='wal': path.with_name(path.name+'-wal').write_bytes(b'unknown committed WAL')
    before={str(p):p.read_bytes() for p in root.rglob('*') if p.is_file()}
    result=pressure(root)
    assert not result.deleted and first.exists() and latest.exists() and 'incident' in result.warning
    assert before=={str(p):p.read_bytes() for p in root.rglob('*') if p.is_file()}


def test_partial_foreign_and_unproved_symlink_material_stays(tmp_path):
    root=tmp_path/'runtime'
    partial=capture(root,3)
    partial.rename(partial.with_name(partial.name+'.partial'))
    foreign=capture(root,4)
    (foreign/'docker-inspect.json').write_text('{}')
    outside=tmp_path/'other-application'
    outside.write_bytes(b'never remove')
    (foreign/'link').symlink_to(outside)
    latest=capture(root,5)
    result=pressure(root)
    assert not result.deleted and outside.read_bytes()==b'never remove'
    assert foreign.exists() and latest.exists()
    assert 'unknown' in result.warning and 'capture incomplete' in result.warning


def test_no_pressure_does_not_delete_eligible_material(tmp_path):
    root=tmp_path/'runtime'
    old,latest=capture(root,1),capture(root,2)
    result=retention.rotate_retained_logs(root,retention.SiteRetentionPolicy(10,10),disk_facts=lambda _:retention.DiskFacts(10**12,10**12))
    assert not result.deleted and result.warning is None and old.exists() and latest.exists()


def test_pass_deletes_at_most_eight_closed_units(tmp_path):
    root=tmp_path/'runtime'
    for index in range(12): capture(root,index)
    result=pressure(root)
    assert len(result.deleted)==8 and 'bounded deletion pass complete' in result.warning


def test_default_aggregate_counts_but_never_deletes_sibling_root(tmp_path):
    base=tmp_path/'runtime'
    selected,sibling=base/'selected',base/'active-other'
    selected.mkdir(parents=True)
    old,latest=capture(sibling,1),capture(sibling,2)
    result=pressure(selected,default_roots=base)
    assert not result.deleted and old.exists() and latest.exists()
    assert 'default-root aggregate' in result.warning and 'sibling root' in result.warning
    result=pressure(selected)
    assert 'configured-root' in result.warning and 'active-other' not in result.warning


@pytest.mark.parametrize('effects', ['closed', 'unresolved', 'unknown'])
def test_previous_session_journals_are_accounted_but_never_pruned_without_history_import(tmp_path, effects):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    archive = root / 'previous-sessions' / ('a' * 32)
    archive.mkdir(parents=True)
    (archive / 'session.json').write_text(json.dumps({
        'schema_version': 'manyforge.workbench_session.v1', 'session_id': 'a' * 32,
        'disposition': 'stopped', 'shutdown': 'clean', 'effects': effects,
    }))
    # Real allocated bytes, not sparse logical length, must count toward Q99.
    protected = archive / 'execution-state/operation-journal.sqlite3-wal'
    protected.parent.mkdir()
    protected.write_bytes(b'original safety evidence' * 4096)
    original = {str(p.relative_to(archive)): p.read_bytes() for p in archive.rglob('*') if p.is_file()}
    old, latest = capture(root, 1), capture(root, 2)
    result = pressure(root)
    assert result.deleted == (str(old.relative_to(root)),)
    assert latest.exists() and archive.exists()
    assert original == {str(p.relative_to(archive)): p.read_bytes() for p in archive.rglob('*') if p.is_file()}
    assert 'previous-session journals kept' in result.warning and 'not_imported or unknown' in result.warning
    measured = int(result.warning.split('measured owned=')[1].split(' bytes')[0])
    assert measured >= protected.stat().st_blocks * 512


def test_current_session_incident_guard_does_not_read_legacy_or_archived_owner(tmp_path):
    root = tmp_path / 'runtime'
    legacy = journal(root, 0)
    current = journal(root / 'current', 1)
    archived = journal(root / 'previous-sessions' / ('b' * 32), 0)
    originals = {p: p.read_bytes() for p in (legacy, current, archived)}
    old, latest = capture(root, 1), capture(root, 2)
    result = pressure(root)
    assert result.deleted == (str(old.relative_to(root)),)
    assert latest.exists() and archived.exists()
    assert all(p.read_bytes() == content for p, content in originals.items())


def test_previous_sessions_are_bounded_with_one_slot_reserved_for_current(tmp_path):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    sources = []
    for index in range(12):
        folder = root / 'previous-sessions' / f'{index:032x}'
        folder.mkdir(parents=True)
        source = folder / 'program-cycle.sqlite3'
        source.write_bytes(f'not imported safety event {index}'.encode())
        os.utime(folder, ns=(index + 1, index + 1))
        sources.append((source, source.read_bytes()))
    result = pressure(root)
    assert len(result.deleted) == 8
    assert all(not path.exists() for path, _ in sources[:8])
    assert all(path.read_bytes() == original for path, original in sources[8:])


def _archive(root, index, *, started=None, sealed=False):
    folder = root / 'previous-sessions' / f'{index:032x}'
    nested = folder / 'execution-state'
    nested.mkdir(parents=True)
    (nested / 'owner.sqlite3').write_text(f'journal {index}')
    if started is not None:
        (folder / 'session.json').write_text(json.dumps({
            'session_id': folder.name, 'started_at_wall': started,
        }))
    os.utime(folder, ns=(index + 1, index + 1))
    if sealed:
        (nested / 'owner.sqlite3').chmod(0o400)
        nested.chmod(0o500)
        folder.chmod(0o500)
    return folder


def _quiet_count_pass(root):
    return retention.rotate_retained_logs(
        root, retention.SiteRetentionPolicy(100, 0),
        disk_facts=lambda _: retention.DiskFacts(10**12, 10**12))


def test_count_pass_removes_oldest_sealed_archive_and_its_final_report_without_pressure(tmp_path):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    archives = [_archive(root, index, sealed=True) for index in range(5)]
    reports = root / 'session-reports'
    reports.mkdir()
    for folder in archives:
        (reports / (folder.name + '.json')).write_text('{}')
    result = _quiet_count_pass(root)
    assert result.deleted == (str(archives[0].relative_to(root)),
                              'session-reports/' + archives[0].name + '.json')
    assert not archives[0].exists()
    assert all(folder.exists() for folder in archives[1:])
    assert len(list(reports.glob('*.json'))) == 4
    for folder in archives[1:]:
        folder.chmod(0o700)
        (folder / 'execution-state').chmod(0o700)


def test_session_order_uses_start_time_and_legacy_directory_mtime(tmp_path):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    # Legacy archive's mtime makes it newer than the marked 2020 session.
    old = _archive(root, 0, started='2020-01-01T00:00:00+00:00')
    legacy = _archive(root, 1)
    os.utime(legacy, ns=(1_700_000_000_000_000_000,) * 2)
    recent = [_archive(root, i, started=f'202{i}-01-01T00:00:00+00:00')
              for i in range(2, 5)]
    _quiet_count_pass(root)
    assert not old.exists()
    assert legacy.exists() and all(folder.exists() for folder in recent)


@pytest.mark.parametrize('state', ['open', 'missing'])
def test_session_count_pruning_preserves_all_while_incident_is_open_or_unknown(tmp_path, state):
    root = tmp_path / 'runtime'
    if state == 'open':
        journal(root / 'current', 0)
    else:
        (root / 'current').mkdir(parents=True)
    archives = [_archive(root, index) for index in range(6)]
    result = _quiet_count_pass(root)
    assert not result.deleted
    assert all(folder.exists() for folder in archives)
    assert 'previous-session retention deferred' in result.warning


def test_session_pruning_preserves_incomplete_selection_and_pending_report(tmp_path):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    archives = [_archive(root, index) for index in range(6)]
    selection = root / '.session-selection.json'
    selection.write_text('{}')
    result = _quiet_count_pass(root)
    assert not result.deleted and all(folder.exists() for folder in archives)
    assert 'deferred during session selection' in result.warning
    selection.unlink()
    pending = root / 'session-reports' / 'pending'
    pending.mkdir(parents=True)
    (pending / (archives[0].name + '.json')).write_text('{}')
    result = _quiet_count_pass(root)
    assert archives[0].exists()
    assert not archives[1].exists() and not archives[2].exists()
    assert (pending / (archives[0].name + '.json')).exists()


def test_session_count_pass_deletes_at_most_eight_units_per_pass(tmp_path):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    archives = [_archive(root, index) for index in range(15)]
    first = _quiet_count_pass(root)
    assert len(first.deleted) == 8
    assert 'above its count limit' in first.warning
    second = _quiet_count_pass(root)
    assert len(second.deleted) == 3
    assert all(not folder.exists() for folder in archives[:11])
    assert all(folder.exists() for folder in archives[11:])


def test_finalized_orphan_reports_are_bounded_and_current_notice_is_preserved(tmp_path):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    reports = root / 'session-reports'
    reports.mkdir()
    identities = [f'{index:032x}' for index in range(7)]
    for index, identity in enumerate(identities):
        report = reports / (identity + '.json')
        report.write_text('{}')
        os.utime(report, ns=(index + 1, index + 1))
    (root / 'current' / 'session.json').write_text(json.dumps({
        'previous_session': {'session_id': identities[0]},
    }))
    _quiet_count_pass(root)
    assert (reports / (identities[0] + '.json')).exists()
    assert all(not (reports / (identity + '.json')).exists() for identity in identities[1:4])
    assert all((reports / (identity + '.json')).exists() for identity in identities[4:])


def test_report_pruning_prefers_orphans_over_reports_for_retained_archives(tmp_path):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    archives = [_archive(root, index) for index in range(3)]
    reports = root / 'session-reports'
    reports.mkdir()
    retained_reports = [reports / (folder.name + '.json') for folder in archives]
    orphan_reports = [reports / (f'{index:032x}.json') for index in range(10, 13)]
    for index, report in enumerate(retained_reports + orphan_reports):
        report.write_text('{}')
        os.utime(report, ns=(index + 1, index + 1))
    result = _quiet_count_pass(root)
    assert all(folder.exists() for folder in archives)
    assert all(report.exists() for report in retained_reports)
    assert all(not report.exists() for report in orphan_reports[:2])
    assert orphan_reports[2].exists()
    assert result.deleted == tuple(str(path.relative_to(root)) for path in orphan_reports[:2])


def test_mounted_session_report_container_is_never_pruned(tmp_path, monkeypatch):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    reports = root / 'session-reports'
    reports.mkdir()
    files = [reports / (f'{index:032x}.json') for index in range(6)]
    for report in files:
        report.write_text('external report')
    monkeypatch.setattr(retention, '_mounted_children', lambda _: {reports})
    result = _quiet_count_pass(root)
    assert not result.deleted
    assert all(report.read_text() == 'external report' for report in files)
    assert 'mounted session-reports' in result.warning


def test_previous_session_symlink_does_not_delete_outside_or_any_archive(tmp_path):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    archives = [_archive(root, index) for index in range(5)]
    outside = tmp_path / 'outside'
    outside.mkdir()
    (outside / 'keep').write_text('external data')
    (root / 'previous-sessions' / ('f' * 32)).symlink_to(outside, target_is_directory=True)
    result = _quiet_count_pass(root)
    assert not result.deleted
    assert all(folder.exists() for folder in archives)
    assert (outside / 'keep').read_text() == 'external data'
    assert 'symlink' in result.warning


def test_mounted_previous_sessions_container_is_never_pruned(tmp_path, monkeypatch):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    archives = [_archive(root, index) for index in range(5)]
    previous = root / 'previous-sessions'
    monkeypatch.setattr(retention, '_mounted_children', lambda _: {previous})
    result = _quiet_count_pass(root)
    assert not result.deleted
    assert all(folder.exists() for folder in archives)
    assert 'mounted previous-sessions' in result.warning


def test_previous_session_mounted_child_is_preserved(tmp_path, monkeypatch):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    archives = [_archive(root, index) for index in range(5)]
    mount = archives[0] / 'execution-state'
    monkeypatch.setattr(retention, '_mounted_children', lambda _: {mount})
    result = _quiet_count_pass(root)
    assert not result.deleted
    assert all(folder.exists() for folder in archives)
    assert 'mounted child' in result.warning


def test_failed_previous_session_deletion_reseals_surviving_archive(tmp_path, monkeypatch):
    root = tmp_path / 'runtime'
    journal(root / 'current')
    archives = [_archive(root, index, sealed=True) for index in range(5)]

    def fail_removal(*_args, **_kwargs):
        raise OSError('injected archive removal failure')

    fail_removal.avoids_symlink_attacks = True
    monkeypatch.setattr(retention.shutil, 'rmtree', fail_removal)
    result = _quiet_count_pass(root)
    assert not result.deleted
    assert all(folder.exists() for folder in archives)
    assert archives[0].stat().st_mode & 0o222 == 0
    assert (archives[0] / 'execution-state').stat().st_mode & 0o222 == 0
    assert 'injected archive removal failure' in result.warning
    for folder in archives:
        folder.chmod(0o700)
        (folder / 'execution-state').chmod(0o700)


def test_mounted_child_aborts_deletion_with_visible_warning(tmp_path,monkeypatch):
    root=tmp_path/'runtime'
    old,latest=capture(root,1),capture(root,2)
    mounted = old/'mounted'
    mounted.mkdir()
    assert mounted.stat().st_dev == old.stat().st_dev
    monkeypatch.setattr(retention,'_mounted_children',lambda _:{mounted})
    result=pressure(root)
    assert not result.deleted and 'accounting incomplete' in result.warning and old.exists() and latest.exists()


def test_receipts_qualify_only_exact_stopped_workspace_occurrences(tmp_path,capsys):
    workspace=tmp_path/'source'
    root=tmp_path/'runtime'
    old,current='a'*64,'b'*64
    mounts=[{'Type':'bind','Source':str(root),'Destination':'/tmp/manyforge-state'},{'Type':'bind','Source':str(workspace),'Destination':'/workspace'}]
    receipt=capture(root,1)
    receipt.rename(receipt.with_name(receipt.name+'.partial'))
    receipt=receipt.with_name(receipt.name+'.partial')
    (receipt/'docker-inspect.json').write_text(json.dumps({'container_id':old,'image':'image','mounts':mounts,'state':{'running':False}}))
    rows=[{'Id':identity,'Image':'image','Mounts':mounts,'State':{'Running':running}} for identity,running in [(old,False),(current,False),('c'*64,False),('d'*64,True)]]
    assert retention.retained_candidates(rows,workspace,current,tmp_path/'legacy')==[(old,True)]
    assert 'unidentified stopped container '+'c'*64 in capsys.readouterr().err
    rows[0]['Image']='changed'
    assert retention.retained_candidates(rows[:1],workspace,current,tmp_path/'legacy')==[]


def test_pressure_stops_when_floor_recovers_without_deleting_more(tmp_path):
    root=tmp_path/'runtime'
    first,second,latest=capture(root,1),capture(root,2),capture(root,3)
    result=retention.rotate_retained_logs(root,retention.SiteRetentionPolicy(100,10),
        disk_facts=lambda _:retention.DiskFacts(10**12,20 if not first.exists() else 0))
    assert result.deleted==(str(first.relative_to(root)),) and result.warning is None
    assert second.exists() and latest.exists()


def test_artifact_metadata_ownership_is_required_even_with_extra_files(tmp_path):
    root=tmp_path/'runtime'
    journal(root)
    old,latest=epoch(root,1),epoch(root,2)
    (old/'arm/operator-note.txt').write_text('extra user file')
    (old/'arm/controller_launch_artifacts.json').write_text('{}')
    result=pressure(root)
    assert not result.deleted and old.exists() and latest.exists()
    assert 'unknown controller-launch artifact ownership' in result.warning


@pytest.mark.parametrize('state',['open','unknown'])
def test_incident_protection_also_keeps_older_console_traces(tmp_path,state):
    root=tmp_path/'runtime'
    first,latest=capture(root,1),capture(root,2)
    path=root/'execution-state/cclc-journal.sqlite3'
    if state=='unknown': path.unlink()
    else:
        with sqlite3.connect(path) as db: db.execute('UPDATE cclc_component_incidents SET acknowledged=0')
    result=pressure(root)
    assert not result.deleted and first.exists() and latest.exists()
    assert 'incident' in result.warning


def test_root_replacement_during_inventory_never_deletes_replacement_files(tmp_path,monkeypatch):
    root=tmp_path/'runtime'
    original,latest=capture(root,1),capture(root,2)
    held=tmp_path/'original-owner'
    inventory=retention._units
    replaced=False
    def replace_before_inventory(owner, **options):
        nonlocal replaced
        if not replaced:
            replaced=True
            root.rename(held)
            capture(root,1)
            capture(root,2)
        return inventory(owner,**options)
    monkeypatch.setattr(retention,'_units',replace_before_inventory)
    result=pressure(root)
    assert not result.deleted and 'state root changed' in result.warning
    assert (held/original.relative_to(root)).exists() and (held/latest.relative_to(root)).exists()
    assert (root/original.relative_to(root)).exists() and (root/latest.relative_to(root)).exists()


def test_parent_symlink_swap_after_inventory_cannot_redirect_fd_deletion(tmp_path,monkeypatch):
    root=tmp_path/'runtime'
    old,latest=capture(root,1),capture(root,2)
    outside=tmp_path/'foreign'
    outside.mkdir()
    (outside/old.name).mkdir()
    foreign=outside/old.name/'important.txt'
    foreign.write_bytes(b'other application data')
    def swap_parent(_):
        (root/'evidence').rename(root/'saved-evidence')
        (root/'evidence').symlink_to(outside,target_is_directory=True)
        return retention.DiskFacts(1,0)
    result=retention.rotate_retained_logs(root,retention.SiteRetentionPolicy(10,10),disk_facts=swap_parent)
    assert not result.deleted and 'retention could not complete safely' in result.warning
    assert foreign.read_bytes()==b'other application data'
    assert (root/'saved-evidence'/old.name).exists() and (root/'saved-evidence'/latest.name).exists()


@pytest.mark.parametrize('swap',['journal','state-directory','wal'])
@pytest.mark.parametrize('initial_ack',[0,1])
def test_pinned_incident_inputs_detect_swap_before_inventory(tmp_path,monkeypatch,swap,initial_ack):
    root=tmp_path/'runtime'
    old,latest=capture(root,1),capture(root,2)
    path=root/'execution-state/cclc-journal.sqlite3'
    with sqlite3.connect(path) as db: db.execute('UPDATE cclc_component_incidents SET acknowledged=?',(initial_ack,))
    inventory=retention._units
    replaced=False
    def replace(owner,**options):
        nonlocal replaced
        if not replaced:
            replaced=True
            if swap=='journal':
                path.rename(root/'original-open.sqlite3')
                journal(root,1-initial_ack)
            elif swap=='state-directory':
                (root/'execution-state').rename(root/'original-execution-state')
                journal(root,1-initial_ack)
            else:
                path.with_name(path.name+'-wal').write_bytes(b'unknown committed changes')
        return inventory(owner,**options)
    monkeypatch.setattr(retention,'_units',replace)
    result=pressure(root)
    assert not result.deleted and old.exists() and latest.exists()
    assert result.warning is not None


def test_journal_opened_via_held_fd_and_all_resources_close(tmp_path,monkeypatch):
    root=tmp_path/'runtime'
    capture(root,1)
    initial=len(os.listdir('/proc/self/fd'))
    original=sqlite3.connect
    inputs=[]
    def connect(path,**options):
        inputs.append(path)
        return original(path,**options)
    monkeypatch.setattr(sqlite3,'connect',connect)
    for _ in range(4):
        pressure(root)
    assert all(path.startswith('file:/proc/self/fd/') and path.endswith('?mode=ro&immutable=1') for path in inputs)
    assert len(os.listdir('/proc/self/fd'))==initial


def test_failed_oldest_deletion_is_protected_and_later_eligible_unit_rotates(tmp_path,monkeypatch):
    root=tmp_path/'runtime'
    first,second,latest=capture(root,1),capture(root,2),capture(root,3)
    original=retention.shutil.rmtree
    calls=[]
    def fail_oldest(name,*,dir_fd):
        calls.append(name)
        if name==first.name: raise PermissionError('fixture oldest is unwritable')
        return original(name,dir_fd=dir_fd)
    fail_oldest.avoids_symlink_attacks=True
    monkeypatch.setattr(retention.shutil,'rmtree',fail_oldest)
    result=pressure(root)
    assert result.deleted==(str(second.relative_to(root)),)
    assert first.exists() and latest.exists() and calls.count(first.name)==1
    assert 'deletion failed; protected for this pass' in result.warning


def test_missing_journal_below_trigger_is_silent_and_preserves_material(tmp_path):
    root=tmp_path/'runtime'
    old,latest=capture(root,1),capture(root,2)
    (root/'execution-state/cclc-journal.sqlite3').unlink()
    result=retention.rotate_retained_logs(root,retention.SiteRetentionPolicy(),disk_facts=lambda _:retention.DiskFacts(10**12,10**12))
    assert not result.deleted and result.warning is None and old.exists() and latest.exists()


def test_symlinked_root_ancestor_is_resolved_and_descendant_link_is_protected(tmp_path):
    actual=tmp_path/'actual'
    actual.mkdir()
    alias=tmp_path/'ssd-alias'
    alias.symlink_to(actual,target_is_directory=True)
    old,latest=capture(actual/'runtime',1),capture(actual/'runtime',2)
    result=pressure(alias/'runtime')
    assert result.deleted==(str(old.relative_to(actual/'runtime')),)
    assert latest.exists()


def test_overflow_is_incomplete_protected_and_does_not_disable_known_rotation(tmp_path,monkeypatch):
    root=tmp_path/'runtime'
    old,latest=capture(root,1),capture(root,2)
    unseen=capture(root,3)
    for index in range(999):
        (root/'evidence'/f'unknown-{index:04}').mkdir()
    original=os.scandir
    class Entries:
        def __init__(self,path):
            with original(path) as rows: self.rows=list(rows)
            self.rows.sort(key=lambda row:(row.name==unseen.name,row.name))
            self.count=0
        def __enter__(self): return self
        def __exit__(self,*_): pass
        def __iter__(self): return self
        def __next__(self):
            if self.count==len(self.rows): raise StopIteration
            row=self.rows[self.count];self.count+=1
            assert self.count<=1001
            return row
    def bounded(path):
        return Entries(path) if isinstance(path,(str,Path)) and Path(path).name=='evidence' else original(path)
    monkeypatch.setattr(os,'scandir',bounded)
    result=pressure(root)
    assert result.deleted==(str(old.relative_to(root)),)
    assert latest.exists() and unseen.exists()
    assert '1000 observed, unseen 1+ protected' in result.warning and 'lower bound' in result.warning


def test_capacity_is_measured_using_the_held_root_descriptor(tmp_path,monkeypatch):
    root=tmp_path/'runtime'
    capture(root,1)
    observed=[]
    original=os.fstatvfs
    def measure(fd):
        observed.append(os.readlink(f'/proc/self/fd/{fd}'))
        return original(fd)
    monkeypatch.setattr(os,'fstatvfs',measure)
    retention.rotate_retained_logs(root,retention.SiteRetentionPolicy(100,0))
    assert observed==[str(root)]


def test_ancestor_swap_at_parent_open_discriminates_fd_anchor_from_by_name(tmp_path,monkeypatch):
    root=tmp_path/'runtime'
    journal(root)
    old,latest=epoch(root,1),epoch(root,2)
    foreign=tmp_path/'outside-owner'
    foreign.mkdir()
    moved=foreign/'execution-run'
    original_open=os.open
    attacked=False
    important=None
    def swap_at_open(path,flags,*arguments,**options):
        nonlocal attacked,important
        by_name=Path(path)==old.parent if isinstance(path,(str,Path)) else False
        by_fd=path=='execution-run' and options.get('dir_fd') is not None
        if not attacked and (by_name or by_fd) and flags & os.O_DIRECTORY:
            attacked=True
            (root/'execution-run').rename(moved)
            (root/'execution-run').symlink_to(moved,target_is_directory=True)
            important=moved/old.relative_to(root/'execution-run')/'foreign-data.txt'
            important.write_bytes(b'outside application evidence')
        return original_open(path,flags,*arguments,**options)
    monkeypatch.setattr(os,'open',swap_at_open)
    result=pressure(root)
    assert attacked and not result.deleted
    assert important is not None and important.read_bytes()==b'outside application evidence'
    assert (moved/latest.relative_to(root/'execution-run')).exists()


def test_binary_tail_handles_arbitrary_read_chunking(tmp_path):
    data=bytes(range(256))*1000
    class Chunked:
        offset=0
        def read(self,size):
            chunk=data[self.offset:self.offset+73]
            self.offset+=len(chunk)
            return chunk
    target=tmp_path/'tail'
    retention.capture_tail(Chunked(),target,limit=10000)
    assert target.read_bytes()==data[-10000:]


def test_legacy_partial_replacement_respects_unknown_receipts(tmp_path):
    evidence=tmp_path/'evidence'
    mounts=[{'Type':'bind','Source':'/workspace-owner','Destination':'/workspace'}]
    summary={'container_id':'a'*64,'image':'sha256:'+'b'*64,'mounts':mounts,'state':{'running':False}}
    partial=retention.prepare_capture(evidence,'20261013T000000Z',summary,legacy=True)
    (partial/'copied-journal').write_bytes(b'only evidence')
    (partial/'docker-inspect.json').write_text('{}')
    with pytest.raises(ValueError,match='unknown or changed'):
        retention.prepare_capture(evidence,'20261013T000002Z',summary,legacy=True)
    assert (partial/'copied-journal').read_bytes()==b'only evidence'


@pytest.mark.parametrize('active_flag',['Paused','Restarting'])
def test_active_occurrence_never_qualifies_for_failed_capture_retry(tmp_path,active_flag):
    workspace=tmp_path/'source'
    root=tmp_path/'runtime'
    mounts=[{'Type':'bind','Source':str(root),'Destination':'/tmp/manyforge-state'},
            {'Type':'bind','Source':str(workspace),'Destination':'/workspace'}]
    identity='a'*64
    partial=root/'evidence/20261013T000000Z.partial'
    partial.mkdir(parents=True)
    (partial/'docker-inspect.json').write_text(json.dumps({'container_id':identity,'image':'image','mounts':mounts,'state':{'running':False}}))
    rows=[{'Id':identity,'Image':'image','Mounts':mounts,'State':{'Running':False,active_flag:True}}]
    assert retention.retained_candidates(rows,workspace,'b'*64,tmp_path/'legacy')==[]


def test_oversized_encoded_summary_cannot_replace_previous_legacy_copy(tmp_path):
    evidence=tmp_path/'evidence'
    summary={'container_id':'a'*64,'image':'sha256:'+'b'*64,'mounts':[], 'state':{'running':False}}
    partial=retention.prepare_capture(evidence,'20261013T000000Z',summary,legacy=True)
    (partial/'copied-journal').write_bytes(b'only evidence')
    oversized={**summary,'detail':'é'*12000}
    with pytest.raises(ValueError,match='exceeds 64KB'):
        retention.prepare_capture(evidence,'20261013T000001Z',oversized,legacy=True)
    assert (partial/'copied-journal').read_bytes()==b'only evidence'
    assert list(evidence.iterdir())==[partial]


def test_epoch_component_overflow_is_named_incomplete_and_protected(tmp_path):
    root=tmp_path/'runtime'
    journal(root)
    old=epoch(root,1)
    latest=epoch(root,2)
    for index in range(1000):
        (old/f'unknown-component-{index}').mkdir()
    result=pressure(root)
    assert not result.deleted and old.exists() and latest.exists()
    assert str(old) in result.warning and '1000 observed, unseen 1+ protected' in result.warning


@pytest.mark.parametrize('linked_scope', ['capture', 'evidence', 'artifact-root', 'bundle', 'epoch'])
def test_descendant_symlink_protects_material_and_continues_independent_rotation(tmp_path, linked_scope):
    root, outside = tmp_path/'runtime', tmp_path/'outside'
    journal(root)
    if linked_scope in ('capture', 'evidence'):
        old, latest = epoch(root,1), epoch(root,2)
        foreign = capture(outside,1)
        link = root/'evidence'
        if linked_scope == 'capture':
            link.mkdir()
            link = link/foreign.name
        target = foreign if linked_scope == 'capture' else foreign.parent
    else:
        old, latest = capture(root,1), capture(root,2)
        foreign = epoch(outside,1)
        base = root/'execution-run/controller-launch-artifacts'
        link = base if linked_scope == 'artifact-root' else base/'bundle'
        target = outside/'execution-run/controller-launch-artifacts' if linked_scope == 'artifact-root' else foreign.parent
        if linked_scope == 'epoch':
            link.mkdir(parents=True)
            link = link/'epoch1'
            target = foreign
        else:
            link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(target, target_is_directory=True)
    before = {str(p):p.read_bytes() for p in outside.rglob('*') if p.is_file()}
    result = pressure(root)
    assert result.deleted == (str(old.relative_to(root)),)
    assert latest.exists() and link.is_symlink()
    assert before == {str(p):p.read_bytes() for p in outside.rglob('*') if p.is_file()}
    assert str(link) in result.warning and 'protected, accounting incomplete (lower bound)' in result.warning
    assert '/proc/self/fd/' not in result.warning


def mounted_summary(root):
    return {'container_id':'a'*64, 'image':'sha256:'+'b'*64,
        'mounts':[{'Type':'bind','Source':str(root),'Destination':'/tmp/manyforge-state'},
                  {'Type':'bind','Source':'/workspace-owner','Destination':'/workspace'}],
        'manyforge_export_kind':'console_tail', 'state':{'running':False}}


def test_mounted_partial_is_reused_and_preserves_prior_tail_and_receipt_identity(tmp_path):
    root = tmp_path/'runtime'
    root.mkdir()
    summary = mounted_summary(root)
    first = retention.prepare_capture(root/'evidence', '20261013T000000Z', summary, legacy=False, state_root=root)
    (first/'docker.log').write_bytes(b'prior good tail')
    (first/'.docker.log.next').write_bytes(b'failed next attempt')
    second = retention.prepare_capture(root/'evidence', '20261013T000001Z', summary, legacy=False, state_root=root)
    assert first == second and second.name == '.mounted-'+'a'*64+'.partial'
    assert (second/'docker.log').read_bytes() == b'prior good tail'
    assert not (second/'.docker.log.next').exists()
    assert list(second.parent.iterdir()) == [second]
    assert json.loads((second/'docker-inspect.json').read_text()) == summary
    rows = [{'Id':'a'*64, 'Image':summary['image'], 'Mounts':summary['mounts'], 'State':{'Running':False}}]
    assert retention.retained_candidates(rows, Path('/workspace-owner'), 'c'*64, tmp_path/'legacy') == [('a'*64,True)]
    assert 'capture incomplete' in pressure(root).warning


def test_single_historical_mounted_partial_reuses_owned_folder_without_losing_tail(tmp_path):
    root = tmp_path/'runtime'
    partial = capture(root,1)
    partial.rename(partial.with_name(partial.name+'.partial'))
    partial = partial.with_name(partial.name+'.partial')
    before = (partial/'docker.log').read_bytes()
    result = retention.prepare_capture(root/'evidence', '20261013T000002Z', mounted_summary(root), legacy=False, state_root=root)
    assert not partial.exists() and result.name == '.mounted-'+'a'*64+'.partial'
    assert (result/'docker.log').read_bytes() == before
    assert list(result.parent.iterdir()) == [result]


@pytest.mark.parametrize('exception', ['multiple', 'unknown-receipt', 'symlink'])
def test_mounted_partial_ambiguity_preserves_material_without_new_partial(tmp_path, exception):
    root = tmp_path/'runtime'
    root.mkdir()
    summary = mounted_summary(root)
    partial = retention.prepare_capture(root/'evidence', '20261013T000000Z', summary, legacy=False, state_root=root)
    (partial/'docker.log').write_bytes(b'investigation tail')
    if exception == 'multiple':
        historical = partial.parent/'20261013T000001Z.partial'
        historical.mkdir()
        (historical/'docker-inspect.json').write_text(json.dumps(summary))
        (historical/'docker.log').write_bytes(b'other historical tail')
    elif exception == 'unknown-receipt':
        (partial/'docker-inspect.json').write_text('{}')
        (partial/'operator-note').write_text('useful')
    else:
        outside = tmp_path/'outside'
        outside.write_bytes(b'foreign')
        outside.unlink()
        partial.rename(outside)
        partial.symlink_to(outside, target_is_directory=True)
    before = {str(p):p.read_bytes() for p in root.rglob('*') if p.is_file()}
    folders = set(partial.parent.iterdir())
    with pytest.raises(ValueError, match='multiple historical|unknown contents|unknown or changed'):
        retention.prepare_capture(root/'evidence', '20261013T000002Z', summary, legacy=False, state_root=root)
    assert folders == set(partial.parent.iterdir())
    assert before == {str(p):p.read_bytes() for p in root.rglob('*') if p.is_file()}



@pytest.mark.parametrize('kind', ['capture', 'epoch'])
def test_owned_unit_with_extra_user_file_and_directory_rotates(tmp_path, kind):
    root = tmp_path/'runtime'
    journal(root)
    create = capture if kind == 'capture' else epoch
    old, latest = create(root,1), create(root,2)
    (old/'user-file').write_bytes(b'user data')
    (old/'user-directory').mkdir()
    (old/'user-directory/note').write_bytes(b'user data')
    result = pressure(root)
    assert result.deleted == (str(old.relative_to(root)),)
    assert not old.exists() and latest.exists()


@pytest.mark.parametrize('kind', ['capture', 'epoch'])
def test_owned_unit_symlink_is_unlinked_without_touching_external_target(tmp_path, kind):
    root = tmp_path/'runtime'
    journal(root)
    create = capture if kind == 'capture' else epoch
    old, latest = create(root,1), create(root,2)
    outside = tmp_path/'foreign-application'
    outside.mkdir()
    (outside/'file').write_bytes(b'never delete or copy')
    link = old/'user-link'
    link.symlink_to(outside, target_is_directory=True)
    result = pressure(root)
    assert result.deleted == (str(old.relative_to(root)),)
    assert not link.is_symlink() and not old.exists() and latest.exists()
    assert (outside/'file').read_bytes() == b'never delete or copy'
    assert list(outside.iterdir()) == [outside/'file']


@pytest.mark.parametrize('acknowledged', [0, None])
def test_open_or_unknown_incident_owned_units_with_extra_files_remain_protected(tmp_path, acknowledged):
    root = tmp_path/'runtime'
    path = journal(root, acknowledged or 0)
    old, latest = capture(root,1), capture(root,2)
    old_epoch, latest_epoch = epoch(root,3), epoch(root,4)
    for folder in (old, old_epoch):
        (folder/'user-file').write_bytes(b'user data')
        (folder/'user-directory').mkdir()
        (folder/'user-directory/note').write_bytes(b'user data')
    if acknowledged is None:
        path.unlink()
    before = {str(p):p.read_bytes() for p in root.rglob('*') if p.is_file()}
    result = pressure(root)
    assert not result.deleted and 'incident' in result.warning
    assert before == {str(p):p.read_bytes() for p in root.rglob('*') if p.is_file()}
    assert all(p.exists() for p in (old, latest, old_epoch, latest_epoch))


def test_owned_capture_cleanup_refuses_same_filesystem_mount_before_any_mutation(tmp_path, monkeypatch):
    root = tmp_path/'runtime'
    root.mkdir()
    summary = mounted_summary(root)
    partial = retention.prepare_capture(root/'evidence', '20261013T000000Z', summary, legacy=False, state_root=root)
    mounted = partial/'user-directory'
    mounted.mkdir()
    (mounted/'file').write_bytes(b'mounted material')
    (partial/'user-file').write_bytes(b'ordinary extra')
    (partial/'docker.log').write_bytes(b'prior tail')
    assert mounted.stat().st_dev == partial.stat().st_dev
    before = {str(p):p.read_bytes() for p in partial.rglob('*') if p.is_file()}
    monkeypatch.setattr(retention, '_mounted_children', lambda _: {mounted})
    with pytest.raises(ValueError, match='mounted child'):
        retention.prepare_capture(root/'evidence', '20261013T000001Z', summary, legacy=False, state_root=root)
    assert before == {str(p):p.read_bytes() for p in partial.rglob('*') if p.is_file()}
    assert list(partial.parent.iterdir()) == [partial]


def test_same_filesystem_bind_mount_facts_are_read_from_existing_mountinfo(tmp_path, monkeypatch):
    root = tmp_path/'runtime'
    mounted = root/'evidence/capture/user directory'
    encoded = str(mounted).replace(' ', r'\040')
    data = f'50 30 8:1 /other/source {encoded} rw - ext4 /dev/same rw\n'
    original = Path.read_text
    monkeypatch.setattr(Path, 'read_text', lambda p,*a,**kw: data if p == Path('/proc/self/mountinfo') else original(p,*a,**kw))
    assert retention._mounted_children(root) == {mounted}


def test_mount_boundary_is_rechecked_after_accounting_before_owned_unit_deletion(tmp_path, monkeypatch):
    root = tmp_path/'runtime'
    old, latest = capture(root,1), capture(root,2)
    mounted = old/'user-directory'
    mounted.mkdir()
    (mounted/'file').write_bytes(b'same-filesystem bind material')
    assert mounted.stat().st_dev == old.stat().st_dev
    checked = []
    def mount_facts(scope):
        checked.append(scope)
        return {mounted} if scope == old else set()
    monkeypatch.setattr(retention, '_mounted_children', mount_facts)
    result = pressure(root)
    assert not result.deleted and old.exists() and latest.exists()
    assert root in checked and old in checked
    assert 'deletion boundary unavailable' in result.warning and 'mounted child' in result.warning
    assert (mounted/'file').read_bytes() == b'same-filesystem bind material'


@pytest.mark.parametrize('kind', ['capture', 'epoch'])
def test_owned_chronology_survives_directory_timestamp_changes(tmp_path, kind):
    root = tmp_path/'runtime'
    journal(root)
    create = capture if kind == 'capture' else epoch
    old, middle, latest = create(root,1), create(root,2), create(root,3)
    (old/'user-directory').mkdir()
    (old/'user-directory/note').write_bytes(b'extra file does not set chronology')
    os.utime(old, ns=(1999999999999999999,1999999999999999999))
    checks = 0
    def recover_after_one(_):
        nonlocal checks
        checks += 1
        return retention.DiskFacts(1, 0) if checks == 1 else retention.DiskFacts(10**15, 10**15)
    result = retention.rotate_retained_logs(root, retention.SiteRetentionPolicy(10,10), disk_facts=recover_after_one)
    assert result.deleted == (str(old.relative_to(root)),)
    assert not old.exists() and middle.exists() and latest.exists()


def test_equal_owned_marker_times_protect_both_latest_epochs(tmp_path):
    root = tmp_path/'runtime'
    journal(root)
    first, second = epoch(root,1), epoch(root,2)
    marker = first/'arm/controller_launch_artifacts.json'
    same = marker.stat().st_mtime_ns
    os.utime(second/'arm/controller_launch_artifacts.json', ns=(same,same))
    result = pressure(root)
    assert not result.deleted and first.exists() and second.exists()
    assert 'equal marker times protected' in result.warning


def ros_session(root, started, *, written=None):
    logs = root / 'execution-run/ros'
    sequence = 1 + sum(1 for entry in logs.iterdir()) if logs.exists() else 1
    folder = logs / f'{sequence}-{started}'
    folder.mkdir(parents=True)
    trace = folder / 'native-execution-host.log'
    trace.write_bytes(b'host trace' * 1000)
    os.utime(trace, ns=(written or started, written or started))
    return folder


def test_host_ros_rotation_keeps_current_session_and_shared_lock_home(tmp_path):
    root = tmp_path / 'runtime'
    journal(root)
    old, current = ros_session(root, 100), ros_session(root, 200)
    lock = root / 'execution-run/ros-home/spawner.lock'
    lock.parent.mkdir(parents=True)
    lock.write_bytes(b'coordination')
    result = pressure(root)
    assert result.deleted == (str(old.relative_to(root)),)
    assert not old.exists() and current.exists()
    assert lock.read_bytes() == b'coordination'
    assert 'current-session' in result.warning


def test_host_ros_old_directory_with_current_file_is_never_deleted(tmp_path):
    root = tmp_path / 'runtime'
    journal(root)
    old, current = ros_session(root, 100, written=200), ros_session(root, 200)
    result = pressure(root)
    assert not result.deleted and old.exists() and current.exists()
    assert 'current-session' in result.warning


@pytest.mark.parametrize('state', ['open', 'missing', 'corrupt', 'wal'])
def test_host_ros_open_or_unknown_incident_protects_every_session(tmp_path, state):
    root = tmp_path / 'runtime'
    old, current = ros_session(root, 100), ros_session(root, 200)
    if state != 'missing':
        path = journal(root, 0 if state == 'open' else 1)
        if state == 'corrupt':
            path.write_text('not a journal')
        elif state == 'wal':
            path.with_name(path.name + '-wal').write_bytes(b'unknown WAL')
    result = pressure(root)
    assert not result.deleted and old.exists() and current.exists()
    assert 'incident' in result.warning


def test_host_ros_unproved_names_and_latest_future_session_are_protected(tmp_path):
    root = tmp_path / 'runtime'
    journal(root)
    old = ros_session(root, 100)
    future = ros_session(root, retention.time.time_ns() + 10**12)
    unknown = root / 'execution-run/ros/unidentified'
    unknown.mkdir()
    (unknown / 'trace.log').write_bytes(b'unproved')
    result = pressure(root)
    assert result.deleted == (str(old.relative_to(root)),)
    assert future.exists() and unknown.exists()
    assert 'unproved host ROS' in result.warning


def test_host_ros_rotation_uses_existing_pressure_trigger(tmp_path):
    root = tmp_path / 'runtime'
    journal(root)
    old, current = ros_session(root, 100), ros_session(root, 200)
    result = retention.rotate_retained_logs(
        root, retention.SiteRetentionPolicy(100, 0),
        disk_facts=lambda _: retention.DiskFacts(10**12, 10**12))
    assert not result.deleted and old.exists() and current.exists()


def test_host_ros_latest_sequence_survives_backward_clock_and_catch_up(tmp_path, monkeypatch):
    root = tmp_path / 'runtime'
    journal(root)
    monkeypatch.setattr(retention.time, 'time_ns', lambda: 1000)
    first = retention.create_host_ros_log_session(root)
    first_log = first / 'host.log'
    first_log.write_text('older actual run')
    os.utime(first_log, ns=(1000, 1000))
    monkeypatch.setattr(retention.time, 'time_ns', lambda: 500)
    newest = retention.create_host_ros_log_session(root)
    newest_log = newest / 'host.log'
    newest_log.write_text('newest actual run')
    os.utime(newest_log, ns=(500, 500))
    assert first.name == '1-1000' and newest.name == '2-500'
    monkeypatch.setattr(retention.time, 'time_ns', lambda: 2000)
    result = pressure(root)
    assert not result.deleted and newest_log.read_text() == 'newest actual run'
    assert first.exists()  # Its file time is not proved before the new start.
    later = retention.create_host_ros_log_session(root)
    result = pressure(root)
    assert set(result.deleted) == {str(first.relative_to(root)), str(newest.relative_to(root))}
    assert later.name == '3-2000' and later.exists()


def test_host_ros_same_start_nanosecond_has_distinct_ordered_sessions(tmp_path, monkeypatch):
    root = tmp_path / 'runtime'
    root.mkdir()
    monkeypatch.setattr(retention.time, 'time_ns', lambda: 1000)
    first = retention.create_host_ros_log_session(root)
    second = retention.create_host_ros_log_session(root)
    assert first.name == '1-1000' and second.name == '2-1000'
    assert first.is_dir() and second.is_dir()


def test_host_ros_legacy_sole_copy_and_ambiguous_sequence_are_protected(tmp_path):
    root = tmp_path / 'runtime'
    journal(root)
    logs = root / 'execution-run/ros'
    for name in ['1000', '1-100', '1-200']:
        folder = logs / name
        folder.mkdir(parents=True)
        trace = folder / 'host.log'
        trace.write_text('keep original')
        os.utime(trace, ns=(100, 100))
    result = pressure(root)
    assert not result.deleted
    assert all((logs / name / 'host.log').read_text() == 'keep original' for name in ['1000', '1-100', '1-200'])


def test_host_ros_complete_names_keep_latest_beyond_deep_candidate_budget(tmp_path, monkeypatch):
    root = tmp_path / 'runtime'
    journal(root)
    logs = root / 'execution-run/ros'
    logs.mkdir(parents=True)
    for sequence in range(1, 1002):
        (logs / f'{sequence}-100').mkdir()
    result = pressure(root)
    assert len(result.deleted) == retention._MAX_DELETIONS
    assert (logs / '1001-100').exists()
    assert 'bounded deletion pass complete' in result.warning
    assert len(list(logs.iterdir())) == 1001 - retention._MAX_DELETIONS
    # Both creation and latest discovery scan complete top-level names, with
    # no recursive walk or separate counter/metadata record.
    monkeypatch.setattr(retention.time, 'time_ns', lambda: 50)
    actual = retention.create_host_ros_log_session(root)
    assert actual.name == '1002-50'
    assert all(p.is_dir() for p in logs.iterdir())


def test_host_ros_failed_name_listing_never_uses_partial_maximum(tmp_path, monkeypatch):
    root = tmp_path / 'runtime'
    journal(root)
    sessions = [ros_session(root, n) for n in [100, 200, 300]]
    original_scandir = retention.os.scandir
    class FailingNames:
        def __init__(self, path):
            self.path = path
        def __enter__(self):
            self.scan = original_scandir(self.path)
            return self
        def __exit__(self, *args):
            self.scan.close()
        def __iter__(self):
            for index, entry in enumerate(self.scan):
                if index == 2:
                    raise OSError('fixture interrupted top-level listing')
                yield entry
    def scandir(path):
        if isinstance(path, (str, Path)) and Path(path).name == 'ros':
            return FailingNames(path)
        return original_scandir(path)
    monkeypatch.setattr(retention.os, 'scandir', scandir)
    result = pressure(root)
    assert not result.deleted and all(folder.exists() for folder in sessions)
    assert 'session name listing failed' in result.warning


def test_host_ros_protected_legacy_prefix_does_not_starve_ordered_candidates(tmp_path):
    root = tmp_path / 'runtime'
    journal(root)
    logs = root / 'execution-run/ros'
    logs.mkdir(parents=True)
    for legacy in range(1, 1002):
        (logs / str(legacy)).mkdir()
    old, current = logs / '1-100', logs / '2-200'
    old.mkdir()
    current.mkdir()
    trace = old / 'host.log'
    trace.write_bytes(b'genuine old session')
    os.utime(trace, ns=(100, 100))
    result = pressure(root)
    assert result.deleted == (str(old.relative_to(root)),)
    assert current.exists() and all((logs / str(n)).exists() for n in range(1, 1002))
    assert 'accounting incomplete (lower bound)' in result.warning
