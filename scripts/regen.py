#!/usr/bin/env python3
"""Regenerate, or check, every committed generated output and identity pin.

    python3 scripts/regen.py --update [--build-dir BUILD]
    python3 scripts/regen.py --check  [--build-dir BUILD] [--require-native]

The steps run the existing generators, in dependency order. ``--update``
repeats the whole sequence until nothing changes. When a step moves a digest,
the old value is replaced by the new one in every source and test file that
holds a copy (docs, examples and generated outputs are left to their owners).
``--check`` writes nothing and exits 1 if any output or pin is stale.

Steps marked "native" need the built Deployment tools. Pass ``--build-dir``
(the directory that holds ``deployment/manyforge_deployment_revision``), the
two tool options, or ``MANYFORGE_DEPLOYMENT_REVISION_TOOL`` and
``MANYFORGE_PROGRAM_REVISION_TOOL``. Without them those steps are skipped and
reported; ``--require-native`` turns a skip into a failure.

Run it in the workbench or dev image (it needs the ManyForge Python
dependencies). See docs/reference/IDENTITY_AND_PINNING.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parents[1]
for _path in (ROOT / "manyforge_deployment/tools", ROOT, ROOT / "manyforge_behavior"):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

BEHAVIOR = "manyforge_behavior"
MAINTAINED = "manyforge_deployment/resources/maintained"
DESCRIPTION = f"{MAINTAINED}/maintained_deployment_sources.v1.json"
ASSISTANT_PROFILES = f"{MAINTAINED}/maintained_assistant_profiles.v1.json"
CATALOG_PY = f"{BEHAVIOR}/manyforge_behavior/catalog.py"
NODE_CATALOG = f"{BEHAVIOR}/resources/node_catalog.yaml"
SCHEMA_ROOT = "manyforge_deployment/resources/closed_schema"
ALARM_CATALOG = "manyforge_headless/resources/operator-alarm-catalog.v1.yaml"
MATERIALIZATION_CPP = "manyforge_deployment/src/activation_materialization_internal.cpp"
SOURCES_CHECKER = "manyforge_deployment/tools/check_maintained_deployment_sources.py"
PROJECT_SNAPSHOTS = (
    (f"{BEHAVIOR}/pyproject.toml",
     f"{BEHAVIOR}/manyforge_behavior/_retained_source/manyforge_behavior.pyproject.toml"),
    ("manyforge_example_nodes/pyproject.toml",
     "manyforge_example_nodes/_retained_source/pyproject.toml"),
)

HEX = re.compile(r"(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])")
DIGEST = re.compile(r"^sha256:([0-9a-f]{64})$")
# Pin copies are replaced only in these kinds of files ...
PIN_SUFFIXES = {".py", ".cpp", ".hpp", ".h", ".cc", ".ts", ".tsx", ".js",
                ".yaml", ".yml", ".json", ".cmake", ".in", ".txt"}
# ... and never under these directories or in sealed documents.
PIN_SKIP_DIRS = {".git", "node_modules", "build", ".manyforge-dev", "__pycache__",
                 "third_party", "docs", "examples", ".pytest_cache", "dist"}
PIN_SKIP_SUFFIXES = (".deployment.yaml", ".md")


class Stale(Exception):
    """A checked output or pin differs from what its generator produces."""


class Skipped(Exception):
    """A step cannot run here (missing native tool)."""


@dataclass
class Context:
    update: bool
    deployment_tool: Path | None
    program_tool: Path | None
    pairs: dict[str, str] = field(default_factory=dict)  # every move, old hex -> new hex
    pending: dict[str, str] = field(default_factory=dict)  # moves not yet propagated
    repinned: list[str] = field(default_factory=list)  # pin copies edited by this step
    cpp_repinned: bool = False
    changed: list[str] = field(default_factory=list)

    def tool(self, which: str) -> Path:
        path = self.deployment_tool if which == "deployment" else self.program_tool
        if path is None:
            raise Skipped(f"needs the built manyforge_{which}_revision tool")
        return path


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def env() -> dict[str, str]:
    value = dict(os.environ)
    value["PYTHONPATH"] = os.pathsep.join(
        [str(ROOT / BEHAVIOR), str(ROOT), str(ROOT / "manyforge_deployment/tools"),
         value.get("PYTHONPATH", "")]).rstrip(os.pathsep)
    value["PYTHONDONTWRITEBYTECODE"] = "1"
    return value


def run(*args: object, stale_on_failure: bool = True) -> str:
    """Run one generator; in check mode its non-zero exit means stale."""
    result = subprocess.run([str(a) for a in args], cwd=ROOT, env=env(), text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        message = (result.stderr.strip() or result.stdout.strip()).splitlines()[-3:]
        error = Stale if stale_on_failure else RuntimeError
        raise error(" | ".join(message) or f"exit {result.returncode}")
    return result.stdout


def python(script: str, *args: object, check: bool | None = None, ctx: Context) -> str:
    flag = [] if check is None else (["--check"] if check else ["--update"])
    return run(sys.executable, ROOT / script, *flag, *args,
               stale_on_failure=not ctx.update)


def sha256_file(path: str) -> str:
    return hashlib.sha256((ROOT / path).read_bytes()).hexdigest()


# --- pin propagation ---------------------------------------------------------

def generated_outputs() -> set[Path]:
    """Files owned by a generator: never edited as pin copies."""
    return {path for step in STEPS if step.generated for pattern in step.outputs
            for path in ROOT.glob(pattern) if path.is_file()}


def pin_files() -> list[Path]:
    generated = generated_outputs()
    found = []
    for directory, subdirs, files in os.walk(ROOT):
        subdirs[:] = [d for d in subdirs if d not in PIN_SKIP_DIRS]
        for name in files:
            path = Path(directory) / name
            if (path.suffix in PIN_SUFFIXES or name == "CMakeLists.txt") and \
                    not name.endswith(PIN_SKIP_SUFFIXES) and path not in generated:
                found.append(path)
    return found


def repin(ctx: Context, old: str, new: str) -> None:
    """Record that one pinned digest moved; check mode reports it as stale."""
    if old == new:
        return
    if not ctx.update:
        raise Stale(f"pin {old[:12]}… should be {new[:12]}…")
    ctx.pairs[old] = new
    ctx.pending[old] = new


def flush_pins(ctx: Context) -> None:
    """Replace every moved digest by its successor in all pin copies, in one pass."""
    if not ctx.pending:
        return
    pending, ctx.pending = ctx.pending, {}
    for path in pin_files():
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        hits = set(HEX.findall(text)) & pending.keys()
        if not hits:
            continue
        for old in hits:
            text = text.replace(old, pending[old])
        path.write_text(text, encoding="utf-8")
        ctx.repinned.append(rel(path))
        ctx.cpp_repinned |= path.suffix in {".cpp", ".hpp", ".h", ".cc", ".in"}


def digest_pairs(old_value: object, new_value: object) -> dict[str, str]:
    """Pair digests found at the same place in two versions of one document."""
    pairs: dict[str, str] = {}

    def key_of(row: object) -> object:
        if isinstance(row, dict):
            for key in sorted(row):
                if (key == "id" or key.endswith("_id")) and isinstance(row[key], str):
                    return (key, row[key])
        return None

    def walk(a: object, b: object) -> None:
        if isinstance(a, str) and isinstance(b, str):
            ma, mb = DIGEST.match(a), DIGEST.match(b)
            if ma and mb and a != b:
                pairs[ma.group(1)] = mb.group(1)
        elif isinstance(a, dict) and isinstance(b, dict):
            for key in a.keys() & b.keys():
                walk(a[key], b[key])
        elif isinstance(a, list) and isinstance(b, list):
            keyed_a = {key_of(row): row for row in a}
            keyed_b = {key_of(row): row for row in b}
            if None not in keyed_a and len(keyed_a) == len(a) and len(keyed_b) == len(b):
                for key in keyed_a.keys() & keyed_b.keys():
                    walk(keyed_a[key], keyed_b[key])
            elif len(a) == len(b):
                for x, y in zip(a, b):
                    walk(x, y)

    walk(old_value, new_value)
    return pairs


def load_data(raw: bytes) -> object:
    import yaml  # noqa: PLC0415

    try:
        return yaml.safe_load(raw)
    except yaml.YAMLError:
        return None


# --- steps -------------------------------------------------------------------

def step_project_snapshots(ctx: Context) -> None:
    for source, snapshot in PROJECT_SNAPSHOTS:
        if (ROOT / source).read_bytes() != (ROOT / snapshot).read_bytes():
            if not ctx.update:
                raise Stale(f"{snapshot} is not a byte copy of {source}")
            shutil.copyfile(ROOT / source, ROOT / snapshot)


def step_catalog_pin(ctx: Context) -> None:
    candidate = python(f"{BEHAVIOR}/scripts/generate_effective_node_catalog.py",
                       "--candidate-digest", ctx=ctx).strip()
    match = re.search(r'EXPECTED_BUILTIN_CATALOG_DIGEST = \(\s*"sha256:([0-9a-f]{64})"',
                      (ROOT / CATALOG_PY).read_text(encoding="utf-8"))
    if match is None or DIGEST.match(candidate) is None:
        raise RuntimeError("cannot read the catalog acceptance pin or candidate digest")
    repin(ctx, match.group(1), DIGEST.match(candidate).group(1))


def step_node_catalog(ctx: Context) -> None:
    python(f"{BEHAVIOR}/scripts/generate_effective_node_catalog.py",
           *(["--check"] if not ctx.update else []), ctx=ctx)


def step_closed_schemas(ctx: Context) -> None:
    # Generated schemas only. Their moved revisions reach the registry and the
    # C++/Python copies through the generic pin propagation after this step.
    # Hand-authored schema revisions are not recomputed here: the Python formula
    # does not reproduce every stored revision (contact_policy.v1).
    python("manyforge_deployment/tools/generate_io_provider_configuration_schema.py",
           "--workspace-root", ROOT, check=not ctx.update, ctx=ctx)


def step_alarm_catalog(ctx: Context) -> None:
    from manyforge_behavior.canonical_artifact import (  # noqa: PLC0415
        canonical_json_bytes, load_strict_json_yaml, raw_sha256_digest)

    value = load_strict_json_yaml(ROOT / ALARM_CATALOG)
    computed = raw_sha256_digest(canonical_json_bytes(
        {key: value[key] for key in ("catalog_version", "definitions", "schema_version")}))
    stored = DIGEST.match(value["descriptor_digest"]).group(1)
    repin(ctx, stored, computed[7:])  # the catalog YAML holds the first copy


def step_maintained_sources(ctx: Context) -> None:
    # Byte pins of the two maintained source descriptions (C++ qualification tool).
    text = (ROOT / MATERIALIZATION_CPP).read_text(encoding="utf-8")
    for source in (DESCRIPTION, ASSISTANT_PROFILES):
        name = re.escape(Path(source).name)
        match = re.search(name + r'"\);.*?"sha256:(?:"\s*")?([0-9a-f]{64})"', text, re.S)
        if match is None:
            raise RuntimeError(f"cannot find the byte pin of {source}")
        repin(ctx, match.group(1), sha256_file(source))
    # Runtime restoration profile digests, with the checker's own formula.
    import importlib  # noqa: PLC0415

    import check_maintained_deployment_sources as checker  # noqa: PLC0415

    checker = importlib.reload(checker)  # its pin table may have moved in an earlier pass

    description = json.loads((ROOT / DESCRIPTION).read_text(encoding="utf-8"))
    expected = checker._EXPECTED_RUNTIME_RESTORATION_PROFILE_DIGESTS
    for profile in description["runtime_restoration_profiles"]:
        old = expected.get(profile["runtime_restoration_profile_id"])
        new = hashlib.sha256(json.dumps(profile, ensure_ascii=False, separators=(",", ":"),
                                        sort_keys=True).encode("utf-8")).hexdigest()
        if old is not None:
            repin(ctx, old, new)
    flush_pins(ctx)  # the checker below reads the moved pins
    run(sys.executable, ROOT / SOURCES_CHECKER, "--check", "--input", ROOT / DESCRIPTION,
        "--workspace-root", ROOT)


def step_program_binding_transport(ctx: Context) -> None:
    python("manyforge_deployment/tools/generate_program_binding_transport.py",
           *(["--check"] if not ctx.update else []), ctx=ctx)


def step_assistant_registry(ctx: Context) -> None:
    python("manyforge_deployment/tools/generate_assistant_instrument_registry.py",
           *(["--check"] if not ctx.update else []),
           "--catalog", ROOT / NODE_CATALOG,
           "--assistant-profiles", ROOT / ASSISTANT_PROFILES,
           "--output", ROOT / "manyforge_deployment/resources/assistant/"
                              "assistant_instrument_registry_source.v1.json", ctx=ctx)


def step_node_environment(ctx: Context) -> None:
    python(f"{BEHAVIOR}/scripts/generate_node_environment_manifest.py",
           *(["--check"] if not ctx.update else []), ctx=ctx)


def step_program_revisions(ctx: Context) -> None:
    tool = ctx.tool("program")
    for path in sorted((ROOT / "examples").glob("*.program.yaml")):
        if load_data(path.read_bytes()).get("api_version") != "manyforge.program.v2":
            continue
        try:
            run(tool, "--check", "--input", path)
        except Stale:
            if not ctx.update:
                raise Stale(f"{rel(path)}: program_revision is stale") from None
            run(tool, "--update", "--input", path, stale_on_failure=False)


def step_maintained_occurrences(ctx: Context) -> None:
    tool = ctx.tool("deployment")
    python("manyforge_deployment/tools/generate_maintained_deployment_occurrences.py",
           "--description", ROOT / DESCRIPTION, "--assistant-profiles",
           ROOT / ASSISTANT_PROFILES, "--workspace-root", ROOT, "--revision-tool", tool,
           check=not ctx.update, ctx=ctx)


def step_ui6i_scenario(ctx: Context) -> None:
    deployment_tool, program_tool = ctx.tool("deployment"), ctx.tool("program")
    python("scripts/qualification/generate_ui6i_dual_robot_scenario.py",
           "--workspace-root", ROOT, "--deployment-revision-tool", deployment_tool,
           "--program-revision-tool", program_tool, check=not ctx.update, ctx=ctx)


def step_scenario_pins(ctx: Context) -> None:
    """Re-pin member digests, the checked-content digest and the source snapshot
    of every Scenario folder, with the Composer's own functions. Content is
    never changed; generator-owned Scenarios are already current here."""
    from manyforge_behavior.canonical_artifact import (  # noqa: PLC0415
        load_strict_json_yaml_bytes, raw_sha256_digest)
    from manyforge_composer.backend import scenario_artifacts as sa  # noqa: PLC0415

    stale = []
    for project in sorted((ROOT / "examples").glob("*.scenario")):
        manifest_path = project / sa.SCENARIO_MANIFEST
        manifest = load_strict_json_yaml_bytes(manifest_path.read_bytes(),
                                               source=rel(manifest_path))
        values, digests = {}, {}
        for member, filename in sa.SCENARIO_PROJECT_MEMBERS.items():
            raw = (project / filename).read_bytes()
            values[member] = sa._load_project_member(raw, source=rel(project / filename),
                                                     nullable=True)
            digests[filename] = raw_sha256_digest(raw)
        pinned = dict(manifest)
        pinned["members"] = dict(sorted(digests.items()))
        pinned["content"] = {
            "deployment_present": values["deployment_draft"] is not None,
            "program_present": values["program_draft"] is not None,
            "assignment_present": values["program_role_assignment"] is not None,
        }
        pinned["saved_status"] = {**manifest["saved_status"],
                                  "checked_content_digest": sa.scenario_content_digest(
                                      values["program_draft"], values["deployment_draft"],
                                      values["program_role_assignment"])}
        pinned["source_snapshot"], _ = sa._source_snapshot(values["deployment_draft"],
                                                           project_root=project)
        if pinned != manifest:
            stale.append(project.name)
            if ctx.update:
                manifest_path.write_bytes(sa._json_bytes(pinned, label=sa.SCENARIO_MANIFEST))
    if stale and not ctx.update:
        raise Stale("Scenario pins are stale: " + ", ".join(stale))


def step_rest_contracts(ctx: Context) -> None:
    for selector in ("structural-rest", "headless-rest", "mcp"):
        run(sys.executable, ROOT / "scripts/regen-public-contracts.py", selector,
            *(["--check"] if not ctx.update else []), stale_on_failure=not ctx.update)


@dataclass(frozen=True)
class Step:
    name: str
    run: Callable[[Context], None]
    outputs: tuple[str, ...]  # globs, relative to the root; used to detect changes
    generated: bool = True  # outputs are generator-owned, never edited as pin copies
    native: bool = False


STEPS = (
    Step("project-snapshots", step_project_snapshots,
         tuple(snapshot for _, snapshot in PROJECT_SNAPSHOTS)),
    Step("catalog-pin", step_catalog_pin, (CATALOG_PY,), generated=False),
    Step("node-catalog", step_node_catalog, (NODE_CATALOG,)),
    Step("closed-schemas", step_closed_schemas,
         (f"{SCHEMA_ROOT}/value/manyforge.value_schema.runtime.io_provider_execution.v1.yaml",
          f"{SCHEMA_ROOT}/value/manyforge.value_schema.runtime.pose_source.v2.yaml")),
    Step("alarm-catalog", step_alarm_catalog, (ALARM_CATALOG,), generated=False),
    Step("maintained-sources", step_maintained_sources, (MATERIALIZATION_CPP, SOURCES_CHECKER),
         generated=False),
    Step("program-binding-transport", step_program_binding_transport,
         ("manyforge_headless_api/program_binding_transport.py",
          "manyforge_composer/frontend/src/types/programBindingTransport.generated.ts",
          "manyforge_composer/frontend/src/api/programBindingTransport.generated.ts")),
    Step("assistant-registry", step_assistant_registry,
         ("manyforge_deployment/resources/assistant/*.json",)),
    Step("node-environment", step_node_environment,
         (f"{BEHAVIOR}/resources/node_environment_manifest.yaml",)),
    Step("program-revisions", step_program_revisions, ("examples/*.program.yaml",),
         native=True),
    Step("maintained-occurrences", step_maintained_occurrences,
         (f"{MAINTAINED}/catalog/*/*.json", f"{MAINTAINED}/site_overlays/*.json",
          f"{MAINTAINED}/maintained_historical_capability_closure.v1.json",
          "examples/*.deployment.yaml", "examples/*.scenario/*"), native=True),
    Step("ui6i-scenario", step_ui6i_scenario,
         ("examples/ur5e_lite6_wall*.yaml", "examples/ur5e_lite6_wall*.scenario/*"),
         native=True),
    Step("scenario-pins", step_scenario_pins, ("examples/*.scenario/scenario.yaml",)),
    Step("rest-contracts", step_rest_contracts,
         ("docs/reference/REST_API.md", "docs/reference/HEADLESS_REST_API.md",
          "docs/reference/MCP_RUNTIME_CONTRACT.md")),
)


def snapshot(step: Step) -> dict[Path, bytes]:
    files: dict[Path, bytes] = {}
    for pattern in step.outputs:
        for path in ROOT.glob(pattern):
            if path.is_file():
                files[path] = path.read_bytes()
    return files


def run_step(step: Step, ctx: Context) -> tuple[str, str]:
    before = snapshot(step) if ctx.update else {}
    ctx.repinned = []
    status, detail = "current", ""
    try:
        step.run(ctx)
    except Skipped as reason:
        return "skipped", str(reason)
    except Stale as reason:
        status, detail = "stale", str(reason)
    except Exception as error:  # noqa: BLE001 - report every step, then exit non-zero
        status, detail = "failed", f"{type(error).__name__}: {error}"
    if not ctx.update:
        return status, detail
    after = snapshot(step)
    moved = sorted(rel(p) for p in before.keys() | after.keys()
                   if before.get(p) != after.get(p))
    # A digest that a generator moved inside its own outputs is a pin move too,
    # unless the old value is still in use somewhere in those outputs.
    outputs = b"".join(after.values())
    for path in moved:
        old, new = before.get(ROOT / path), after.get(ROOT / path)
        if old is None or new is None or path.endswith((".md", ".py", ".ts")):
            continue
        for old_hex, new_hex in digest_pairs(load_data(old), load_data(new)).items():
            if old_hex.encode() not in outputs:
                repin(ctx, old_hex, new_hex)
    flush_pins(ctx)
    changes = moved + [f"{path} (re-pinned)" for path in ctx.repinned if path not in moved]
    ctx.repinned = []
    ctx.changed.extend(changes)
    if status != "current":
        return status, detail
    return ("updated", ", ".join(changes)) if changes else ("current", "")


def leftovers(ctx: Context) -> list[str]:
    """Old digests still present outside docs: places regen does not own."""
    if not ctx.pairs:
        return []
    found = []
    for directory, subdirs, files in os.walk(ROOT):
        subdirs[:] = [d for d in subdirs
                      if d not in PIN_SKIP_DIRS - {"examples"} | {"docs"}]
        for name in files:
            path = Path(directory) / name
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for old in sorted(set(HEX.findall(text)) & ctx.pairs.keys()):
                found.append(f"{rel(path)} still holds {old[:12]}…")
    return found


def tool_path(option: Path | None, variable: str, build: Path | None, name: str) -> Path | None:
    for candidate in (option, os.environ.get(variable),
                      build / "deployment" / name if build else None):
        if candidate:
            path = Path(candidate).resolve()
            if not path.is_file():
                raise SystemExit(f"regen: {name} not found at {path}")
            return path
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--update", action="store_true")
    mode.add_argument("--check", action="store_true")
    parser.add_argument("--build-dir", type=Path,
                        help="build tree holding deployment/manyforge_*_revision")
    parser.add_argument("--deployment-revision-tool", type=Path)
    parser.add_argument("--program-revision-tool", type=Path)
    parser.add_argument("--require-native", action="store_true",
                        help="fail instead of skipping the native steps")
    parser.add_argument("--only", help="comma-separated step names")
    parser.add_argument("--list", action="store_true", help="list the steps and exit")
    args = parser.parse_args()
    if args.list:
        for step in STEPS:
            print(f"{step.name}{' (native)' if step.native else ''}")
        return 0
    selected = [s for s in STEPS if not args.only or s.name in args.only.split(",")]
    ctx = Context(
        update=args.update,
        deployment_tool=tool_path(args.deployment_revision_tool,
                                  "MANYFORGE_DEPLOYMENT_REVISION_TOOL", args.build_dir,
                                  "manyforge_deployment_revision"),
        program_tool=tool_path(args.program_revision_tool, "MANYFORGE_PROGRAM_REVISION_TOOL",
                               args.build_dir, "manyforge_program_revision"),
    )
    bad = False
    for attempt in range(1, 4 if ctx.update else 2):
        if ctx.update:
            print(f"pass {attempt}")
        ctx.changed = []
        bad = False
        for step in selected:
            status, detail = run_step(step, ctx)
            print(f"  {step.name:<26} {status}{': ' + detail if detail else ''}")
            bad |= status in {"stale", "failed"} or (status == "skipped" and args.require_native)
        if bad or not ctx.update or not ctx.changed:
            break
    else:
        print("regen: outputs still change after 3 passes", file=sys.stderr)
        return 1
    if ctx.update:
        for line in leftovers(ctx):
            print(f"  note: {line}")
        if ctx.cpp_repinned:
            print("  C++ pins moved: rebuild the native tools, then run --update again "
                  "and --check with the tools.")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
