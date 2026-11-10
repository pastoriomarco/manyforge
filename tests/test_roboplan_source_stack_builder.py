"""Contract tests for the ManyForge-owned RoboPlan source-stack builder."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


_REPO_ROOT = Path(__file__).resolve().parents[1]
_SCRIPT = _REPO_ROOT / "scripts/ci/build-roboplan-source-stack.sh"
_CLOSURE_SCRIPT = _REPO_ROOT / "scripts/ci/build-roboplan-source-closure.sh"
_VERIFIER_SCRIPT = _REPO_ROOT / "scripts/ci/verify-external-source-closure.py"
_DOCKERFILES = (_REPO_ROOT / "Dockerfile", _REPO_ROOT / "Dockerfile.ros-lyrical-full")
_PROJECTS = (
    "roboplan_common",
    "roboplan_example_models",
    "roboplan_core",
    "roboplan_simple_ik",
    "roboplan_oink",
    "roboplan_toppra",
    "roboplan_rrt",
    "roboplan_cartesian_planning",
    "roboplan_examples",
)


def test_native_and_ros_images_use_the_checked_manyforge_builder() -> None:
    for dockerfile in _DOCKERFILES:
        source = dockerfile.read_text(encoding="utf-8")
        assert "ARG ROBOPLAN_SOURCE_MODE=qualified" in source
        assert 'if [ "$ROBOPLAN_SOURCE_MODE" = qualified ]' in source
        assert "development_override" in source
        assert "COPY scripts/ci/build-roboplan-source-stack.sh" in source
        assert "COPY scripts/ci/build-roboplan-source-closure.sh" in source
        assert "COPY scripts/ci/verify-external-source-closure.py" in source
        assert "COPY .docker/proxsuite/ /tmp/proxsuite/" in source
        assert "COPY .docker/jrl-cmakemodules/ /tmp/jrl-cmakemodules/" in source
        assert "COPY .docker/toppra/ /tmp/toppra/" in source
        assert "COPY .docker/nanoflann/ /tmp/nanoflann/" in source
        assert "-DFETCHCONTENT_SOURCE_DIR_NANOFLANN=/tmp/nanoflann" in source
        assert (
            "COPY third_party/external_delta_ledger.v1.json"
            in source
        )
        assert "bash /tmp/build-roboplan-source-closure.sh" in source
        assert (
            "/tmp/proxsuite /tmp/jrl-cmakemodules /tmp/toppra /tmp/nanoflann /tmp/roboplan"
            in source
        )
        assert "ROBOPLAN_SOURCE_ID=roboplan_0_8_0_foundation" in source
        assert "ROBOPLAN_ALLOW_DEVELOPMENT_SOURCE" in source
        assert "-DFETCHCONTENT_FULLY_DISCONNECTED=ON" in source
        assert "-DBUILD_PYTHON_BINDINGS=OFF" in source
        assert "manyforge-roboplan.conf" in source
        assert "ROBOPLAN_SITE_PACKAGES" in source
        assert "umask 0002" in source
        assert "umask 0022" in source
        assert "ROBOPLAN_CMAKE_ARGS=" not in source
        assert "./scripts/build_cmake.bash" not in source

    compose = (_REPO_ROOT / "docker-compose.yaml").read_text(encoding="utf-8")
    assert (
        compose.count("ROBOPLAN_SOURCE_MODE: ${ROBOPLAN_SOURCE_MODE:-qualified}") == 4
    )

    setup = (_REPO_ROOT / "scripts/setup-docker.sh").read_text(encoding="utf-8")
    assert "2a6b447e648b1a3337f10cbab995cda0c0a71d55" in setup
    assert "6ae71b84730bf30453d70764073d186f5fe7ad69" in setup
    assert "984234828e97557b82d30262ffa3a851b6be2383" in setup
    assert "8121e9af72f5aa589b515aa15e0dc408344c3220" in setup
    assert "18f4edf753e813644a7a12711a86139777a8ef00" in setup
    assert "submodule update" not in setup
    assert "roboplan_0_8_0_foundation" in setup


def _fake_cmake(path: Path) -> None:
    path.write_text(
        """#!/usr/bin/env python3
import json
import os
import sys

with open(os.environ["ROBOPLAN_FAKE_CMAKE_LOG"], "a", encoding="utf-8") as stream:
    stream.write(json.dumps({
        "argv": sys.argv[1:],
        "cmake_prefix_path": os.environ.get("CMAKE_PREFIX_PATH", ""),
        "ld_library_path": os.environ.get("LD_LIBRARY_PATH", ""),
    }, sort_keys=True) + "\\n")
""",
        encoding="utf-8",
    )
    path.chmod(0o755)


def _fake_closure_cmake(path: Path) -> None:
    path.write_text(
        """#!/usr/bin/env python3
import json
import os
import pathlib
import sys

args = sys.argv[1:]
row = {
    "argv": args,
    "cmake_prefix_path": os.environ.get("CMAKE_PREFIX_PATH", ""),
    "ld_library_path": os.environ.get("LD_LIBRARY_PATH", ""),
}
with open(os.environ["ROBOPLAN_FAKE_CMAKE_LOG"], "a", encoding="utf-8") as stream:
    stream.write(json.dumps(row, sort_keys=True) + "\\n")

if "-S" in args and "-B" in args:
    source = pathlib.Path(args[args.index("-S") + 1])
    build = pathlib.Path(args[args.index("-B") + 1])
    build.mkdir(parents=True, exist_ok=True)
    install_arg = next(
        (item for item in reversed(args) if item.startswith("-DCMAKE_INSTALL_PREFIX=")),
        "-DCMAKE_INSTALL_PREFIX=",
    )
    install = install_arg.split("=", 1)[1]
    (build / "fake-configure.json").write_text(
        json.dumps({"source": str(source), "install": install}),
        encoding="utf-8",
    )
    prefixes = os.environ.get("CMAKE_PREFIX_PATH", "").split(":")
    if source.name == "roboplan_oink":
        exact = next(item for item in prefixes if item.endswith("/proxsuite"))
        (build / "CMakeCache.txt").write_text(
            f"proxsuite_DIR:PATH={exact}/lib/cmake/proxsuite\\n",
            encoding="utf-8",
        )
    elif source.name == "roboplan_toppra":
        exact = next(item for item in prefixes if item.endswith("/toppra"))
        (build / "CMakeCache.txt").write_text(
            f"toppra_DIR:PATH={exact}/lib/cmake/toppra\\n",
            encoding="utf-8",
        )
elif args[:1] == ["--install"]:
    build = pathlib.Path(args[1])
    configured = json.loads((build / "fake-configure.json").read_text(encoding="utf-8"))
    if pathlib.Path(configured["source"]).name.startswith("proxsuite"):
        header = pathlib.Path(configured["install"]) / "include/proxsuite/proxqp/dense/dense.hpp"
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text("// exact fake header\\n", encoding="utf-8")
""",
        encoding="utf-8",
    )
    path.chmod(0o755)


def _fake_source_verifier(path: Path) -> None:
    path.write_text(
        """#!/usr/bin/env python3
import json
import os
import sys

with open(os.environ["ROBOPLAN_FAKE_VERIFIER_LOG"], "a", encoding="utf-8") as stream:
    stream.write(json.dumps(sys.argv[1:]) + "\\n")
""",
        encoding="utf-8",
    )
    path.chmod(0o755)


def _verification_environment(tmp_path: Path) -> dict[str, str]:
    verifier = tmp_path / "verify-source.py"
    _fake_source_verifier(verifier)
    ledger = tmp_path / "ledger.json"
    ledger.write_text("{}\n", encoding="utf-8")
    return {
        "ROBOPLAN_SOURCE_LEDGER": str(ledger),
        "ROBOPLAN_SOURCE_VERIFIER": str(verifier),
        "ROBOPLAN_SOURCE_ID": "fixture_roboplan",
        "PROXSUITE_SOURCE_ID": "fixture_proxsuite",
        "JRL_CMAKEMODULES_SOURCE_ID": "fixture_jrl_cmakemodules",
        "TOPPRA_SOURCE_ID": "fixture_toppra",
        "NANOFLANN_SOURCE_ID": "fixture_nanoflann",
        "ROBOPLAN_FAKE_VERIFIER_LOG": str(tmp_path / "verifier.jsonl"),
    }


def test_builder_owns_arguments_and_prefixes_each_fresh_install(
    tmp_path: Path,
) -> None:
    source = tmp_path / "source"
    for project in _PROJECTS:
        project_root = source / project
        project_root.mkdir(parents=True)
        (project_root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.14)\n",
            encoding="utf-8",
        )

    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    _fake_cmake(fake_bin / "cmake")
    log = tmp_path / "cmake.jsonl"
    build = tmp_path / "build"
    install = tmp_path / "install"
    environment = {
        **os.environ,
        **_verification_environment(tmp_path),
        "PATH": f"{fake_bin}:{os.environ['PATH']}",
        "ROBOPLAN_FAKE_CMAKE_LOG": str(log),
        "ROBOPLAN_BUILD_ROOT": str(build),
        "ROBOPLAN_INSTALL_ROOT": str(install),
        "CMAKE_BUILD_PARALLEL_LEVEL": "2",
        "CMAKE_PREFIX_PATH": "/ambient/prefix",
        "LD_LIBRARY_PATH": "/ambient/lib",
    }

    subprocess.run(
        [
            str(_SCRIPT),
            str(source),
            "-DBUILD_TESTING=OFF",
            "-DSOURCE_STACK_SENTINEL=ON",
            "-DCMAKE_INSTALL_PREFIX=/caller-must-not-win",
        ],
        check=True,
        capture_output=True,
        text=True,
        env=environment,
    )

    rows = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
    verifier_rows = [
        json.loads(line)
        for line in Path(environment["ROBOPLAN_FAKE_VERIFIER_LOG"])
        .read_text(encoding="utf-8")
        .splitlines()
    ]
    assert verifier_rows == [
        [
            "--ledger",
            environment["ROBOPLAN_SOURCE_LEDGER"],
            "--source-id",
            "fixture_roboplan",
            "--root",
            str(source),
            "--content-kind",
            "build",
        ]
    ]
    assert len(rows) == len(_PROJECTS) * 3
    for index, project in enumerate(_PROJECTS):
        configure, build_row, install_row = rows[index * 3 : index * 3 + 3]
        project_install = str(install / project)
        prior_installs = [str(install / prior) for prior in reversed(_PROJECTS[:index])]
        expected_prefix = ":".join([*prior_installs, "/ambient/prefix"])
        expected_libraries = ":".join(
            [*(f"{path}/lib" for path in prior_installs), "/ambient/lib"]
        )

        assert configure["argv"] == [
            "-DBUILD_TESTING=OFF",
            "-DSOURCE_STACK_SENTINEL=ON",
            "-DCMAKE_INSTALL_PREFIX=/caller-must-not-win",
            "-S",
            str(source / project),
            "-B",
            str(build / project),
            f"-DCMAKE_INSTALL_PREFIX={project_install}",
            f"-DPYTHON_SITELIB=lib/python{sys.version_info.major}.{sys.version_info.minor}/site-packages",
        ]
        assert configure["cmake_prefix_path"] == expected_prefix
        assert configure["ld_library_path"] == expected_libraries
        assert build_row["argv"] == [
            "--build",
            str(build / project),
            "--parallel",
            "2",
        ]
        assert build_row["ld_library_path"] == ":".join(
            [
                str(build / project),
                str(build / project / "lib"),
                expected_libraries,
            ]
        )
        assert install_row["argv"] == ["--install", str(build / project)]
        assert install_row["ld_library_path"] == expected_libraries


def test_closure_builder_materializes_exact_dependencies_without_fetch_fallback(
    tmp_path: Path,
) -> None:
    proxsuite = tmp_path / "proxsuite-v0.8.2"
    jrl_cmakemodules = tmp_path / "jrl-cmakemodules-v2.3.0"
    toppra = tmp_path / "toppra-v0.6.10"
    nanoflann = tmp_path / "nanoflann-2.0.1"
    roboplan = tmp_path / "roboplan-v0.8.0"
    proxsuite.mkdir()
    (proxsuite / "CMakeLists.txt").write_text("project(proxsuite)\n", encoding="utf-8")
    jrl_cmakemodules.mkdir()
    (toppra / "cpp").mkdir(parents=True)
    (toppra / "cpp/CMakeLists.txt").write_text("project(toppra)\n", encoding="utf-8")
    (nanoflann / "include").mkdir(parents=True)
    (nanoflann / "include/nanoflann.hpp").write_text("// fixture\n", encoding="utf-8")
    sources = [str(proxsuite), str(jrl_cmakemodules), str(toppra), str(nanoflann), str(roboplan)]
    for project in _PROJECTS:
        project_root = roboplan / project
        project_root.mkdir(parents=True)
        (project_root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.14)\n",
            encoding="utf-8",
        )

    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    _fake_closure_cmake(fake_bin / "cmake")
    log = tmp_path / "cmake.jsonl"
    build = tmp_path / "build"
    install = tmp_path / "install"
    environment = {
        **os.environ,
        **_verification_environment(tmp_path),
        "PATH": f"{fake_bin}:{os.environ['PATH']}",
        "ROBOPLAN_FAKE_CMAKE_LOG": str(log),
        "ROBOPLAN_CLOSURE_BUILD_ROOT": str(build),
        "ROBOPLAN_CLOSURE_INSTALL_ROOT": str(install),
        "ROBOPLAN_DEPENDENCY_BUILD_TESTING": "OFF",
        "CMAKE_BUILD_PARALLEL_LEVEL": "2",
        "CMAKE_PREFIX_PATH": "/ambient/prefix",
        "LD_LIBRARY_PATH": "/ambient/lib",
    }

    subprocess.run(
        [
            "bash",
            str(_CLOSURE_SCRIPT),
            *sources,
            "-DBUILD_TESTING=OFF",
            "-DSOURCE_STACK_SENTINEL=ON",
        ],
        check=True,
        capture_output=True,
        text=True,
        env=environment,
    )

    rows = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
    verifier_rows = [
        json.loads(line)
        for line in Path(environment["ROBOPLAN_FAKE_VERIFIER_LOG"])
        .read_text(encoding="utf-8")
        .splitlines()
    ]
    assert verifier_rows == [
        [
            "--ledger",
            environment["ROBOPLAN_SOURCE_LEDGER"],
            "--source-id",
            source_id,
            "--root",
            str(root),
            "--content-kind",
            "build",
        ]
        for source_id, root in (
            ("fixture_proxsuite", proxsuite),
            ("fixture_jrl_cmakemodules", jrl_cmakemodules),
            ("fixture_toppra", toppra),
            ("fixture_nanoflann", nanoflann),
            ("fixture_roboplan", roboplan),
            ("fixture_roboplan", roboplan),
        )
    ]
    assert rows[0]["argv"] == [
        "-S",
        str(proxsuite),
        "-B",
        str(build / "proxsuite"),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={install / 'proxsuite'}",
        f"-DJRL_CMAKEMODULES_SOURCE_DIR={jrl_cmakemodules}",
        "-DBUILD_TESTING=OFF",
        "-DBUILD_PYTHON_INTERFACE=OFF",
        "-DBUILD_WITH_VECTORIZATION_SUPPORT=OFF",
        "-DPROXSUITE_BUILD_WITH_SERIALIZATION=OFF",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
        "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON",
    ]
    assert rows[3]["argv"] == [
        "-S",
        str(toppra / "cpp"),
        "-B",
        str(build / "toppra"),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={install / 'toppra'}",
        "-DBUILD_TESTING=OFF",
        "-DPYTHON_BINDINGS=OFF",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
        "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON",
    ]
    roboplan_configures = [
        row
        for row in rows
        if "-S" in row["argv"]
        and any(argument.startswith(f"{roboplan}/") for argument in row["argv"])
    ]
    assert len(roboplan_configures) == len(_PROJECTS)
    for row in roboplan_configures:
        assert "-DFETCHCONTENT_FULLY_DISCONNECTED=ON" in row["argv"]
        assert "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON" in row["argv"]
        assert f"-DFETCHCONTENT_SOURCE_DIR_NANOFLANN={nanoflann}" in row["argv"]
        assert "-DCMAKE_DISABLE_FIND_PACKAGE_nanoflann=ON" in row["argv"]
        assert "-DCMAKE_DISABLE_FIND_PACKAGE_tl-expected=ON" in row["argv"]
        prefixes = row["cmake_prefix_path"].split(":")
        assert str(install / "toppra") in prefixes
        assert str(install / "proxsuite") in prefixes
        assert prefixes.index(str(install / "toppra")) < prefixes.index(
            "/ambient/prefix"
        )
        assert prefixes.index(str(install / "proxsuite")) < prefixes.index(
            "/ambient/prefix"
        )

    verifier_log = Path(environment["ROBOPLAN_FAKE_VERIFIER_LOG"])
    verifier_log.unlink()
    development_environment = {
        **environment,
        "ROBOPLAN_ALLOW_DEVELOPMENT_SOURCE": "1",
    }
    completed = subprocess.run(
        [
            "bash",
            str(_CLOSURE_SCRIPT),
            *sources,
            "-DBUILD_TESTING=OFF",
        ],
        check=True,
        capture_output=True,
        text=True,
        env=development_environment,
    )
    development_verifier_rows = [
        json.loads(line)
        for line in verifier_log.read_text(encoding="utf-8").splitlines()
    ]
    assert development_verifier_rows == [
        [
            "--ledger",
            environment["ROBOPLAN_SOURCE_LEDGER"],
            "--source-id",
            source_id,
            "--root",
            str(root),
            "--content-kind",
            "build",
        ]
        for source_id, root in (
            ("fixture_proxsuite", proxsuite),
            ("fixture_jrl_cmakemodules", jrl_cmakemodules),
            ("fixture_toppra", toppra),
            ("fixture_nanoflann", nanoflann),
        )
    ]
    assert "development source" in completed.stderr

    in_source_build = roboplan / "generated-build"
    rejected_environment = {
        **environment,
        "ROBOPLAN_CLOSURE_BUILD_ROOT": str(in_source_build),
        "ROBOPLAN_CLOSURE_INSTALL_ROOT": str(tmp_path / "rejected-install"),
    }
    rejected = subprocess.run(
        [
            "bash",
            str(_CLOSURE_SCRIPT),
            *sources,
            "-DBUILD_TESTING=OFF",
        ],
        check=False,
        capture_output=True,
        text=True,
        env=rejected_environment,
    )
    assert rejected.returncode == 2
    assert "outside every source root" in rejected.stderr
    assert not in_source_build.exists()


def _git(root: Path, *arguments: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def test_source_verifier_rejects_wrong_content_before_build(tmp_path: Path) -> None:
    source = tmp_path / "source"
    source.mkdir()
    _git(source, "init", "-q")
    _git(source, "config", "user.email", "e1@example.invalid")
    _git(source, "config", "user.name", "Source Stack Test")
    (source / "LICENSE").write_text("fixture license\n", encoding="utf-8")
    (source / "CMakeLists.txt").write_text("project(fixture)\n", encoding="utf-8")
    _git(source, "add", "LICENSE", "CMakeLists.txt")
    _git(source, "commit", "-q", "-m", "fixture")
    content_sha256 = subprocess.run(
        [str(_VERIFIER_SCRIPT), "--print-content-digest", str(source)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    archive_sha256 = subprocess.run(
        ["git", "-C", str(source), "archive", "--format=tar", "HEAD"],
        check=True,
        capture_output=True,
    ).stdout
    ledger = tmp_path / "ledger.json"
    ledger.write_text(
        json.dumps(
            {
                "schema": "manyforge.composable_cell.e1.external_delta_ledger.v1",
                "frozen_sources": [
                    {
                        "id": "fixture",
                        "identity": {
                            "commit": _git(source, "rev-parse", "HEAD"),
                            "tree": _git(source, "rev-parse", "HEAD^{tree}"),
                            "git_archive_sha256": hashlib.sha256(
                                archive_sha256
                            ).hexdigest(),
                            "source_content_sha256": content_sha256,
                            "build_content_sha256": content_sha256,
                        },
                        "license": {
                            "path": "LICENSE",
                            "spdx": "MIT",
                            "sha256": hashlib.sha256(
                                (source / "LICENSE").read_bytes()
                            ).hexdigest(),
                        },
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    command = [
        str(_VERIFIER_SCRIPT),
        "--ledger",
        str(ledger),
        "--source-id",
        "fixture",
        "--root",
        str(source),
        "--content-kind",
        "build",
    ]
    subprocess.run(command, check=True, capture_output=True, text=True)

    (source / "CMakeLists.txt").write_text("project(wrong)\n", encoding="utf-8")
    rejected = subprocess.run(command, check=False, capture_output=True, text=True)

    assert rejected.returncode == 1
    assert "canonical build content digest does not match" in rejected.stderr


def test_source_verifier_rejects_wrong_license_or_git_identity(tmp_path: Path) -> None:
    source = tmp_path / "source"
    source.mkdir()
    _git(source, "init", "-q")
    _git(source, "config", "user.email", "e1@example.invalid")
    _git(source, "config", "user.name", "Source Stack Test")
    (source / "LICENSE").write_text("fixture license\n", encoding="utf-8")
    _git(source, "add", "LICENSE")
    _git(source, "commit", "-q", "-m", "fixture")
    content_sha256 = subprocess.run(
        [str(_VERIFIER_SCRIPT), "--print-content-digest", str(source)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    archive = subprocess.run(
        ["git", "-C", str(source), "archive", "--format=tar", "HEAD"],
        check=True,
        capture_output=True,
    ).stdout
    payload = {
        "schema": "manyforge.composable_cell.e1.external_delta_ledger.v1",
        "frozen_sources": [
            {
                "id": "fixture",
                "identity": {
                    "commit": _git(source, "rev-parse", "HEAD"),
                    "tree": _git(source, "rev-parse", "HEAD^{tree}"),
                    "git_archive_sha256": hashlib.sha256(archive).hexdigest(),
                    "source_content_sha256": content_sha256,
                    "build_content_sha256": content_sha256,
                },
                "license": {
                    "path": "LICENSE",
                    "spdx": "MIT",
                    "sha256": hashlib.sha256(
                        (source / "LICENSE").read_bytes()
                    ).hexdigest(),
                },
            }
        ],
    }
    ledger = tmp_path / "ledger.json"
    ledger.write_text(json.dumps(payload), encoding="utf-8")
    command = [
        str(_VERIFIER_SCRIPT),
        "--ledger",
        str(ledger),
        "--source-id",
        "fixture",
        "--root",
        str(source),
        "--content-kind",
        "source",
    ]

    payload["frozen_sources"][0]["license"]["sha256"] = "0" * 64
    ledger.write_text(json.dumps(payload), encoding="utf-8")
    wrong_license = subprocess.run(command, check=False, capture_output=True, text=True)
    assert wrong_license.returncode == 1
    assert "license digest does not match" in wrong_license.stderr

    payload["frozen_sources"][0]["license"]["sha256"] = hashlib.sha256(
        (source / "LICENSE").read_bytes()
    ).hexdigest()
    payload["frozen_sources"][0]["identity"]["commit"] = "0" * 40
    ledger.write_text(json.dumps(payload), encoding="utf-8")
    wrong_commit = subprocess.run(command, check=False, capture_output=True, text=True)
    assert wrong_commit.returncode == 1
    assert "HEAD does not match" in wrong_commit.stderr
