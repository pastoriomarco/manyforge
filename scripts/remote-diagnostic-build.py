#!/usr/bin/env python3
"""Run an immutable laptop snapshot through the existing remote diagnostic runner.

Example (image must already exist on the remote host):
  python3 scripts/remote-diagnostic-build.py --host thor --source /path/worktree \
    --image sha256:... --variant core-release --output /path/evidence \
    --artifact build/core/Testing -- bash -c 'cmake ...; ctest ...'

The default remote root is ~/manyforge-remote. Busy/priority returns 75 by
default. --priority is only for a caller-coordinated reservation; the caller
owns the flag lifecycle. --wait-seconds bounds lock acquisition without
owning the shared slot while waiting. .git and generated outputs
are not transferred. Git-dependent commands require a separately approved real
metadata arrangement; no substitute Git repository is created. Ignored SOURCE
inputs require explicit --include-ignored relative paths. No images are built.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import inspect
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import time


class SnapshotError(RuntimeError):
    pass


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def relative(value: str) -> str:
    path = PurePosixPath(value)
    if not value or path.is_absolute() or any(p in {"", ".", "..", ".git"} for p in value.split("/")):
        raise SnapshotError(f"Unsafe relative path: {value!r}")
    return str(path)


def generated(path: str) -> bool:
    return any(p in {"build", "install", "log", "__pycache__", ".pytest_cache", "node_modules", "dist"}
               or p.startswith(("build_", "cmake-build-")) for p in path.split("/")[:-1])


def facts(path: Path, root: Path) -> dict:
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode):
        target = os.readlink(path)
        if os.path.isabs(target) or not (path.parent / target).resolve().is_relative_to(root.resolve()):
            raise SnapshotError(f"Symlink escapes source: {path}")
        return {"kind": "symlink", "target": target, "mode": stat.S_IMODE(info.st_mode)}
    if not stat.S_ISREG(info.st_mode):
        raise SnapshotError(f"Unsupported source entry: {path}")
    data = path.read_bytes()
    after = path.lstat()
    if (info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns) != (
            after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
        raise SnapshotError(f"Source changed while reading: {path}")
    return {"kind": "file", "sha256": digest(data), "mode": stat.S_IMODE(info.st_mode)}


def git(source: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(source), *args], env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"})


def identity(source: Path) -> dict:
    helper = Path(__file__).parent / "qualification/source_identity.py"
    result = subprocess.run([sys.executable, str(helper), "--repo-root", str(source)],
                            capture_output=True, text=True)
    if result.returncode:
        raise SnapshotError(f"Laptop Git identity failed (exit {result.returncode}): {result.stderr.strip()}")
    return json.loads(result.stdout)


def inventory(source: Path, includes: list[str]) -> tuple[dict, list[str]]:
    tracked = {os.fsdecode(p) for p in git(source, "ls-files", "-z").split(b"\0") if p}
    untracked = {os.fsdecode(p) for p in git(source, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0") if p}
    paths = tracked | untracked
    for item in includes:
        path = source / relative(item)
        if path.is_dir() and not path.is_symlink():
            paths.update(str(p.relative_to(source)) for p in path.rglob("*") if not p.is_dir() or p.is_symlink())
        elif path.exists() or path.is_symlink():
            paths.add(item)
        else:
            raise SnapshotError(f"Explicit source input is missing: {item}")
    entries, deleted = {}, []
    for name in sorted(paths):
        # Never copy nested vendor Git control files either.
        if ".git" in PurePosixPath(name).parts:
            continue
        name = relative(name)
        path = source / name
        if not path.exists() and not path.is_symlink():
            if name in tracked:
                deleted.append(name)
                continue
            raise SnapshotError(f"Source disappeared: {name}")
        if generated(name):
            if name in tracked:
                raise SnapshotError(f"Tracked path matches generated-output exclusion: {name}")
            continue
        entries[name] = {**facts(path, source), "origin": "tracked" if name in tracked else "untracked" if name in untracked else "ignored"}
    return entries, deleted


def tree(root: Path) -> dict:
    return {str(p.relative_to(root)): facts(p, root) for p in sorted(root.rglob("*"))
            if not p.is_dir() or p.is_symlink()}


def content(entries: dict) -> dict:
    return {name: {k: v for k, v in value.items() if k != "origin"} for name, value in entries.items()}


def copy_entry(source: Path, destination: Path, entry: dict) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        destination.unlink()
    if entry["kind"] == "symlink":
        destination.symlink_to(entry["target"])
    else:
        shutil.copyfile(source, destination)
        destination.chmod(entry["mode"])


def snapshot(source: Path, output: Path, includes: list[str]) -> dict:
    before = identity(source)
    entries, deleted = inventory(source, includes)
    staged = output / "source"
    staged.mkdir()
    for name, entry in entries.items():
        copy_entry(source / name, staged / name, entry)
    if tree(staged) != content(entries):
        raise SnapshotError("Snapshot bytes/modes differ from the captured source manifest")
    after_entries, after_deleted = inventory(source, includes)
    if identity(source) != before or after_entries != entries or after_deleted != deleted:
        raise SnapshotError("Laptop source changed during snapshot; no remote run was started")
    result = {"laptop_source": str(source), "git_identity": before, "files": entries,
              "tracked_deletions": deleted, "included_ignored_paths": includes}
    write_json(output / "source-manifest.json", result)
    with tarfile.open(output / "source.tar", "w") as archive:
        for name in entries:
            archive.add(staged / name, arcname=name, recursive=False)
    return result


def unpack(archive_path: Path, target: Path, expected: dict) -> None:
    target.mkdir()
    with tarfile.open(archive_path) as archive:
        members = archive.getmembers()
        if len(members) != len(expected) or {m.name for m in members} != set(expected):
            raise SnapshotError("Archive inventory differs from source manifest")
        # Files first: never write through an archive-created symlink directory.
        for member in sorted(members, key=lambda m: m.issym()):
            name = relative(member.name)
            entry = expected[name]
            destination = target / name
            if any(parent.is_symlink() for parent in destination.parents if parent != target.parent):
                raise SnapshotError("Archive parent is a symlink")
            destination.parent.mkdir(parents=True, exist_ok=True)
            if member.issym() and entry["kind"] == "symlink" and member.linkname == entry["target"]:
                destination.symlink_to(member.linkname)
            elif member.isfile() and entry["kind"] == "file":
                stream = archive.extractfile(member)
                assert stream is not None
                destination.write_bytes(stream.read())
                destination.chmod(entry["mode"])
            else:
                raise SnapshotError(f"Archive entry type differs: {name}")
    if tree(target) != content(expected):
        raise SnapshotError("Received source bytes/modes differ from manifest")


def managed_directory(path: Path, root: Path, *, create: bool = False) -> None:
    """Validate container directories, separately from preserved source links."""
    root, path = root.absolute(), path.absolute()
    if root.resolve() != root or not path.is_relative_to(root):
        raise SnapshotError(f"Managed directory escapes configured root: {path}")
    directories = [root]
    for part in path.relative_to(root).parts:
        directories.append(directories[-1] / part)
    for directory in directories:
        if directory.is_symlink() or (directory.exists() and not directory.is_dir()):
            raise SnapshotError(f"Managed directory is a symlink/non-directory: {directory}")
        if create:
            directory.mkdir(parents=True, exist_ok=True)
        if directory.resolve() != directory:
            raise SnapshotError(f"Managed directory resolves outside its assigned path: {directory}")


def update_mirror(staged: Path, mirror: Path, expected: dict, *, root: Path | None = None) -> None:
    managed_directory(mirror, root if root is not None else mirror.parent, create=True)
    current = tree(mirror)
    previous_times = {name: (mirror / name).lstat().st_mtime_ns for name in current}
    desired = content(expected)
    # Remove obsolete entries and directories before creating changed paths.
    for name in current:
        if name not in expected or current[name] != desired.get(name):
            (mirror / name).unlink()
    for directory in sorted((p for p in mirror.rglob("*") if p.is_dir() and not p.is_symlink()),
                            key=lambda p: len(p.parts), reverse=True):
        if not any(directory.iterdir()):
            directory.rmdir()
    for name, entry in expected.items():
        if current.get(name) != desired[name]:
            copy_entry(staged / name, mirror / name, entry)
            if entry["kind"] == "file":
                changed_ns = max(time.time_ns(), previous_times.get(name, 0) + 1)
                os.utime(mirror / name, ns=(changed_ns, changed_ns))
            # Do not restore laptop timestamps: changed bytes must invalidate
            # timestamp-based Ninja products. Unchanged files are never touched.
    if tree(mirror) != content(expected):
        raise SnapshotError("Stable remote mirror differs from immutable snapshot")


def slot(root: Path, priority: bool = False, wait_seconds: int = 0):
    if not priority and (root / ".t2-priority").exists():
        raise BlockingIOError("Remote priority slot is reserved")
    handle = (root / ".docker-build.lock").open("a")
    deadline = time.monotonic() + wait_seconds
    try:
        while True:
            if not priority and (root / ".t2-priority").exists():
                raise BlockingIOError("Remote priority appeared while acquiring slot")
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise BlockingIOError("Remote shared lock acquisition timed out")
                time.sleep(min(0.1, remaining))
        if not priority and (root / ".t2-priority").exists():
            raise BlockingIOError("Remote priority appeared while acquiring slot")
    except BaseException:
        handle.close()
        raise
    return handle


def run_command(argv: list[str], log: Path, env: dict) -> int:
    child = None
    pending_signals = []
    previous = {}

    def forward(signum, _frame):
        if child is None:
            pending_signals.append(signum)
        else:
            try:
                os.killpg(child.pid, signum)
            except ProcessLookupError:
                pass

    # Install before Popen: a signal during creation must not strand a runner
    # after this process releases the global slot. Wait through runner cleanup.
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        previous[sig] = signal.signal(sig, forward)
    try:
        with log.open("w") as stream:
            child = subprocess.Popen(argv, stdout=stream, stderr=subprocess.STDOUT,
                                     env=env, start_new_session=True)
            for signum in pending_signals:
                forward(signum, None)
            return child.wait()
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def remote_run(incoming: Path) -> int:
    request = json.loads((incoming / "request.json").read_text())
    manifest = json.loads((incoming / "source-manifest.json").read_text())
    root = Path(request["remote_root"]).expanduser().absolute()
    expected = manifest["files"]
    result = {"command": request["command"], "variant": request["variant"],
              "priority": request["priority"], "wait_seconds": request["wait_seconds"],
              "source_manifest_sha256": digest((incoming / "source-manifest.json").read_bytes())}
    managed_directory(incoming, root)
    evidence = incoming / "result"
    evidence.mkdir()
    exit_code = 70
    try:
        if digest(Path(__file__).read_bytes()) != request["wrapper_sha256"]:
            raise SnapshotError("Transferred wrapper differs from laptop tooling")
        if digest((incoming / "source.tar").read_bytes()) != request["archive_sha256"]:
            raise SnapshotError("Transferred archive checksum differs")
        staged = incoming / "verified-source"
        unpack(incoming / "source.tar", staged, expected)
        with slot(root, request["priority"], request["wait_seconds"]):
            image_data = json.loads(subprocess.check_output(["docker", "image", "inspect", request["image"]]))[0]
            image_id, architecture = image_data["Id"], image_data["Architecture"]
            native = {"aarch64": "arm64", "x86_64": "amd64"}.get(os.uname().machine)
            if architecture != native:
                raise SnapshotError(f"Image architecture {architecture} differs from native host {native}")
            result.update(image_id=image_id, architecture=architecture, native_machine=os.uname().machine)
            work_root = root / request["worktree_id"]
            managed_directory(work_root, root, create=True)
            mirror = work_root / "source"
            update_mirror(staged, mirror, expected, root=root)
            result["remote_source"] = str(mirror)
            env = {**os.environ, "MANYFORGE_DIAGNOSTIC_SOURCE": str(mirror),
                   "MANYFORGE_DIAGNOSTIC_IMAGE": image_id,
                   "MANYFORGE_DIAGNOSTIC_CACHE_BASE": str(root / "diagnostic"),
                   "CMAKE_BUILD_PARALLEL_LEVEL": str(request["jobs"]), **request["compiler_flags"]}
            result["started_ns"] = time.time_ns()
            command_exit = run_command(["bash", str(mirror / "scripts/diagnostic-build.sh"), request["variant"], "--", *request["command"]], evidence / "command.log", env)
            result.update(command_exit=command_exit, finished_ns=time.time_ns())
            after = tree(mirror)
            if after != content(expected):
                write_json(evidence / "remote-source-after.json", after)
                changes = evidence / "remote-source-changes"
                for name, entry in after.items():
                    if content(expected).get(name) != entry:
                        copy_entry(mirror / name, changes / name, entry)
                raise SnapshotError("Remote source changed; returned changes need laptop review before any pass")
            log_text = (evidence / "command.log").read_text(errors="replace")
            roots = [line.removeprefix("Diagnostic tree: ").strip() for line in log_text.splitlines() if line.startswith("Diagnostic tree: ")]
            if len(roots) != 1:
                raise SnapshotError("Runner did not report exactly one diagnostic build tree")
            runner = Path(roots[0]).parent.resolve()
            if not runner.is_relative_to((root / "diagnostic").resolve()):
                raise SnapshotError("Runner reported a cache outside its selected root")
            result["diagnostic_build_tree"] = roots[0]
            for name in ["configuration.txt", *request["artifacts"]]:
                name = relative(name)
                artifact = runner / name
                if not artifact.exists():
                    raise SnapshotError(f"Requested evidence is missing: {name}")
                if artifact.is_symlink() or not artifact.resolve().is_relative_to(runner):
                    raise SnapshotError(f"Evidence escapes runner: {name}")
                target = evidence / "artifacts" / name
                if artifact.is_dir():
                    if any(p.is_symlink() for p in artifact.rglob("*")):
                        raise SnapshotError(f"Requested evidence contains symlinks: {name}")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copytree(artifact, target, symlinks=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(artifact, target)
            exit_code = command_exit if command_exit >= 0 else 128 - command_exit
    except BlockingIOError as error:
        result["error"] = str(error)
        exit_code = 75
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    result["exit_code"] = exit_code
    write_json(evidence / "receipt.json", result)
    # Evidence is a separate archive; source changes are never applied locally.
    write_json(evidence / "artifact-manifest.json", tree(evidence))
    with tarfile.open(incoming / "result.tar", "w") as archive:
        for path in sorted(evidence.rglob("*")):
            if not path.is_dir() or path.is_symlink():
                archive.add(path, arcname=str(path.relative_to(evidence)), recursive=False)
    print(json.dumps(result))
    return exit_code


def ssh(host: str, command: str, **kwargs):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10", host, command], check=True, **kwargs)


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "--remote-run":
        return remote_run(Path(sys.argv[2]).resolve())
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--image", required=True)
    parser.add_argument("--variant", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--remote-root", default="~/manyforge-remote")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--priority", action="store_true", help="Use an explicitly caller-coordinated reservation; never manages its flag")
    parser.add_argument("--wait-seconds", type=int, default=0, help="Bounded shared-lock acquisition wait; default fails fast")
    parser.add_argument("--include-ignored", action="append", default=[])
    parser.add_argument("--artifact", action="append", default=[])
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or args.jobs < 1 or not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9._-]*", args.variant):
        parser.error("A command, positive jobs and safe variant are required")
    if args.wait_seconds < 0:
        parser.error("Lock wait seconds must be nonnegative")
    if args.host.startswith("-"):
        parser.error("Host cannot be an SSH option")
    source = args.source.resolve(strict=True)
    output = args.output.resolve()
    if output.is_relative_to(source):
        parser.error("Evidence must live outside the source worktree")
    output.mkdir(parents=True, exist_ok=False)
    result = 70
    try:
        tooling = output / Path(__file__).name
        tooling.write_bytes(Path(__file__).read_bytes())
        wrapper_hash = digest(tooling.read_bytes())
        helper_hash = digest((Path(__file__).parent / "qualification/source_identity.py").read_bytes())
        manifest = snapshot(source, output, args.include_ignored)
        if digest(Path(__file__).read_bytes()) != wrapper_hash or digest((Path(__file__).parent / "qualification/source_identity.py").read_bytes()) != helper_hash:
            raise SnapshotError("Local tooling changed during snapshot")
        worktree_id = source.name + "-" + digest(os.fsencode(source))[:12]
        request = {"remote_root": args.remote_root, "worktree_id": worktree_id, "image": args.image,
                   "variant": args.variant, "command": command, "jobs": args.jobs,
                   "priority": args.priority, "wait_seconds": args.wait_seconds,
                   "artifacts": [relative(p) for p in args.artifact],
                   "wrapper_sha256": wrapper_hash,
                   "source_identity_helper_sha256": helper_hash,
                   "compiler_flags": {k: os.environ.get(k, "") for k in ("CFLAGS", "CXXFLAGS", "LDFLAGS")},
                   "archive_sha256": digest((output / "source.tar").read_bytes())}
        write_json(output / "request.json", request)
        # Expansion is performed by remote Python, never by interpolated shell.
        remote_root = ssh(args.host, "python3 -c " + shlex.quote("from pathlib import Path; print(Path(" + repr(args.remote_root) + ").expanduser().absolute())"), capture_output=True, text=True).stdout.strip()
        run_name = output.name + "-" + digest((output / "source-manifest.json").read_bytes())[:12]
        incoming = str(Path(remote_root) / worktree_id / "runs" / run_name)
        prepare = ("from pathlib import Path\nclass SnapshotError(RuntimeError): pass\n"
                   + inspect.getsource(managed_directory)
                   + "\nroot = Path(" + repr(remote_root) + ")\n"
                   + "managed_directory(Path(" + repr(str(Path(incoming).parent)) + "), root, create=True)\n"
                   + "Path(" + repr(incoming) + ").mkdir()\n")
        ssh(args.host, "python3 -c " + shlex.quote(prepare))
        for local in [output / "source.tar", output / "source-manifest.json", output / "request.json", tooling]:
            with local.open("rb") as stream:
                ssh(args.host, "cat > " + shlex.quote(str(Path(incoming) / local.name)), stdin=stream)
        invoke = "python3 " + shlex.quote(str(Path(incoming) / Path(__file__).name)) + " --remote-run " + shlex.quote(incoming)
        with (output / "ssh.log").open("w") as stream:
            run = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10", args.host, invoke], stdout=stream, stderr=subprocess.STDOUT)
        try:
            with (output / "result.tar").open("wb") as stream:
                ssh(args.host, "cat " + shlex.quote(str(Path(incoming) / "result.tar")), stdout=stream)
            returned = output / "returned"
            # Inspect and validate evidence without trusting arbitrary tar paths.
            with tarfile.open(output / "result.tar") as archive:
                entries = {}
                for member in archive.getmembers():
                    name = relative(member.name)
                    if member.isfile():
                        data = archive.extractfile(member).read()
                        entries[name] = {"kind": "file", "sha256": digest(data), "mode": member.mode}
                    elif member.issym():
                        entries[name] = {"kind": "symlink", "target": member.linkname, "mode": member.mode}
                    else:
                        raise SnapshotError("Unsupported returned evidence entry")
            unpack(output / "result.tar", returned, entries)
            recorded = json.loads((returned / "artifact-manifest.json").read_text())
            observed = tree(returned)
            observed.pop("artifact-manifest.json")
            if recorded != observed:
                raise SnapshotError("Returned evidence checksum mismatch")
            receipt = json.loads((returned / "receipt.json").read_text())
            if receipt["source_manifest_sha256"] != digest((output / "source-manifest.json").read_bytes()):
                raise SnapshotError("Returned source identity differs")
            if run.returncode != receipt["exit_code"]:
                raise SnapshotError("SSH exit and remote receipt disagree")
            result = receipt["exit_code"]
        except Exception as error:
            raise SnapshotError(f"Remote result/evidence is incomplete: {error}") from error
        print(json.dumps({"source_identity": manifest["git_identity"], "remote_run": incoming, "exit_code": result}))
    except Exception as error:
        write_json(output / "local-error.json", {"error": f"{type(error).__name__}: {error}"})
        print(str(error), file=sys.stderr)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
