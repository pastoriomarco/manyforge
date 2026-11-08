"""Read-only observation of the actual dual-robot Load activation predicate."""

from copy import deepcopy
from types import SimpleNamespace

import pytest

from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.process_tests.test_dual_robot_grouped_execution import (
    observe_applied_activation_projection,
)


CANDIDATE = {"retained_bundle_id": "bundle", "graph_revision": "graph"}
APPLY = {
    "apply_id": "apply", "apply_generation": 2, "request_owner_epoch": "owner",
    "state": "succeeded", "ok": True, "candidate_projection_revision": CANDIDATE,
    "failure_acknowledgement": "unavailable",
    "prepared_activation": {"deployment_artifact_id": "deployment-id",
                            "deployment_revision": "deployment", "program_binding_id": "binding"},
}


def _sample():
    return {
        "apply": deepcopy(APPLY),
        "manager": {
            "source": {"source_owner_epoch": "owner", "available": True,
                       "freshness": "fresh", "known_gaps": []},
            "status": {"manager_ready": True, "runtime": {"converged": True},
                       "active": {"publication": {
                           "transaction": {"kind": "named_apply", "apply_id": "apply",
                                           "apply_generation": 2},
                           "projection_revision": deepcopy(CANDIDATE)}}},
        },
        "selected": {"scenarioId": "scenario", "deploymentRevision": "deployment",
                     "activationOrder": {"retainedBundleId": "bundle", "graphRevision": "graph"}},
        "program": {"origin": {"kind": "scenario", "scenario_id": "scenario"},
                    "deployment_artifact_id": "deployment-id", "program_binding_id": "binding",
                    "deployment_revision": "deployment", "retained_bundle_id": "bundle",
                    "graph_revision": "graph"},
    }


class Observations:
    def __init__(self, samples):
        self.samples = samples
        self.index = 0
        self.now = 0.0
        self.calls = []
        self.sleeps = []
        self.headless = SimpleNamespace(
            manager_apply_status=self.apply, manager_status=self.manager,
            program_applied_status=self.program,
        )
        self.composer = SimpleNamespace(request=self.workspace)

    @property
    def current(self):
        return self.samples[min(self.index, len(self.samples) - 1)]

    def apply(self, *identity):
        self.calls.append(("GET Apply", identity))
        assert identity == ("apply", 2)
        return self.current["apply"]

    def manager(self):
        self.calls.append(("GET Manager",))
        return self.current["manager"]

    def workspace(self, method, path):
        self.calls.append((method, path))
        assert (method, path) == ("GET", "/api/deployment/workspace")
        return {"managerApplied": self.current["selected"]}

    def program(self):
        self.calls.append(("GET Program",))
        value = self.current["program"]
        self.index += 1
        if isinstance(value, Exception):
            raise value
        return value

    def sleep(self, duration):
        self.sleeps.append(duration)
        self.now += duration

    def observe(self, **kwargs):
        return observe_applied_activation_projection(
            self.headless, self.composer, APPLY, "scenario",
            monotonic=lambda: self.now, sleep=self.sleep, **kwargs,
        )


@pytest.mark.parametrize("code", ["program_applied_selection_not_active",
                                 "program_applied_selection_not_found"])
def test_transient_program_selection_waits_for_exact_fresh_converged_tuple(code):
    pending, unconverged, final = _sample(), _sample(), _sample()
    pending["program"] = HeadlessClientError(code, "not selected yet")
    unconverged["manager"]["source"]["known_gaps"] = ["gap"]
    unconverged["manager"]["status"]["runtime"]["converged"] = False
    reads = Observations([pending, unconverged, final])
    trace = []
    result = reads.observe(on_observation=trace.append)
    assert result["candidate"] == CANDIDATE
    assert result["manager_source"]["known_gaps"] == []
    assert result["runtime_converged"] is True
    assert len(trace) == 3 and reads.sleeps == [0.02, 0.02]
    assert all(call[0].startswith("GET") for call in reads.calls)


@pytest.mark.parametrize("foreign", ["owner", "candidate", "composer", "program", "binding"])
def test_foreign_identity_refuses_without_effect_or_observation_retry(foreign):
    sample = _sample()
    if foreign == "owner":
        sample["manager"]["source"]["source_owner_epoch"] = "foreign"
    elif foreign == "candidate":
        sample["apply"]["candidate_projection_revision"]["graph_revision"] = "foreign"
    elif foreign == "composer":
        sample["selected"]["activationOrder"]["retainedBundleId"] = "foreign"
    elif foreign == "program":
        sample["program"]["graph_revision"] = "foreign"
    else:
        sample["program"]["program_binding_id"] = "foreign"
    reads = Observations([sample])
    with pytest.raises(AssertionError):
        reads.observe()
    assert reads.sleeps == []
    assert all(call[0].startswith("GET") for call in reads.calls)


def test_unavailable_program_selection_has_a_bounded_observation_timeout():
    sample = _sample()
    sample["program"] = HeadlessClientError("program_applied_selection_not_active", "pending")
    reads = Observations([sample])
    with pytest.raises(AssertionError, match="Applied activation projection did not settle"):
        reads.observe(timeout_seconds=0.03)
    assert reads.sleeps == [0.02, 0.02]
    assert reads.index == 3


def test_unnamed_program_error_is_not_polled():
    sample = _sample()
    sample["program"] = HeadlessClientError("foreign_error", "fail")
    reads = Observations([sample])
    with pytest.raises(HeadlessClientError, match="fail"):
        reads.observe()
    assert reads.index == 1 and reads.sleeps == []


def _predecessor(sample):
    program = None if isinstance(sample["program"], Exception) else deepcopy(sample["program"])
    publication = deepcopy(sample["manager"]["status"]["active"]["publication"])
    prior_apply = None
    if publication is not None and publication["transaction"]["kind"] == "named_apply":
        prior_apply = {**deepcopy(APPLY),
                       "apply_id": publication["transaction"]["apply_id"],
                       "apply_generation": publication["transaction"]["apply_generation"],
                       "candidate_projection_revision": deepcopy(publication["projection_revision"]),
                       "prepared_activation": {field: program[field] for field in (
                           "deployment_artifact_id", "deployment_revision", "program_binding_id")}}
    return {"source": deepcopy(sample["manager"]["source"]),
            "publication": publication, "apply_status": prior_apply,
            "managerApplied": deepcopy(sample["selected"]), "applied_status": program}


def test_exact_captured_predecessor_waits_for_the_new_named_apply():
    previous = _sample()
    previous["manager"]["status"]["active"]["publication"]["transaction"]["apply_id"] = "previous"
    # Repeating the same Scenario may keep its semantic selection tuple;
    # the new exact named transaction is still mandatory.
    reads = Observations([previous, _sample()])
    result = reads.observe(predecessor=_predecessor(previous))
    assert result["active_publication"]["transaction"]["apply_id"] == "apply"
    assert reads.sleeps == [0.02]


def test_changed_predecessor_candidate_is_not_treated_as_a_pending_projection():
    previous = _sample()
    previous["manager"]["status"]["active"]["publication"]["transaction"]["apply_id"] = "previous"
    changed = deepcopy(previous)
    changed["manager"]["status"]["active"]["publication"]["projection_revision"]["graph_revision"] = "foreign"
    reads = Observations([changed])
    with pytest.raises(AssertionError):
        reads.observe(predecessor=_predecessor(previous))
    assert reads.sleeps == []


def test_incoherent_predecessor_does_not_allow_stale_acceptance():
    previous = _sample()
    predecessor = _predecessor(previous)
    predecessor["applied_status"] = None
    reads = Observations([previous])
    with pytest.raises(AssertionError):
        reads.observe(predecessor=predecessor)
    assert reads.calls == [] and reads.sleeps == []


def test_distinct_captured_predecessor_channels_can_converge_independently():
    previous = _sample()
    publication = previous["manager"]["status"]["active"]["publication"]
    publication["transaction"]["apply_id"] = "previous"
    publication["projection_revision"] = {"retained_bundle_id": "previous-bundle",
                                          "graph_revision": "previous-graph"}
    previous["selected"] = {"scenarioId": "previous-scenario",
                            "deploymentRevision": "previous-deployment",
                            "activationOrder": {"retainedBundleId": "previous-bundle",
                                                "graphRevision": "previous-graph"}}
    previous["program"] = {"origin": {"kind": "scenario", "scenario_id": "previous-scenario"},
                           "deployment_artifact_id": "previous-deployment-id", "program_binding_id": "previous-binding",
                           "deployment_revision": "previous-deployment",
                           "retained_bundle_id": "previous-bundle", "graph_revision": "previous-graph"}
    partial = deepcopy(previous)
    partial["manager"] = _sample()["manager"]
    reads = Observations([previous, partial, _sample()])
    result = reads.observe(predecessor=_predecessor(previous))
    assert result["managerApplied"]["scenarioId"] == "scenario"
    assert reads.sleeps == [0.02, 0.02]


def test_fresh_bootstrap_with_no_program_selection_converges_to_scenario():
    bootstrap = _sample()
    bootstrap["manager"]["status"]["active"]["publication"] = {
        "publication_generation": 1,
        "transaction": {"kind": "bootstrap", "bootstrap_id": "bootstrap", "bootstrap_generation": 1},
        "projection_revision": {"retained_bundle_id": "empty-bundle", "graph_revision": "empty-graph"},
    }
    bootstrap["selected"] = {"scenarioId": None, "deploymentRevision": None,
                             "activationOrder": None}
    bootstrap["program"] = HeadlessClientError("program_applied_selection_not_found", "empty")
    predecessor = _predecessor(bootstrap)
    predecessor["applied_status"] = None
    reads = Observations([bootstrap, _sample()])
    result = reads.observe(predecessor=predecessor)
    assert result["applied_status"]["origin"]["scenario_id"] == "scenario"
    assert reads.sleeps == [0.02]


def test_historical_predecessor_request_owner_is_distinct_from_current_manager_source():
    previous = _sample()
    previous["manager"]["status"]["active"]["publication"]["transaction"]["apply_id"] = "previous"
    predecessor = _predecessor(previous)
    predecessor["apply_status"]["request_owner_epoch"] = "historical-owner"
    reads = Observations([previous, _sample()])
    result = reads.observe(predecessor=predecessor)
    assert result["manager_source"]["source_owner_epoch"] == APPLY["request_owner_epoch"]
    assert reads.sleeps == [0.02]


def test_same_deployment_predecessor_program_binding_waits_for_accepted_binding():
    previous = _sample()
    previous["manager"]["status"]["active"]["publication"]["transaction"]["apply_id"] = "previous"
    previous["program"]["program_binding_id"] = "previous-binding"
    partial = deepcopy(previous)
    partial["manager"] = _sample()["manager"]
    reads = Observations([partial, _sample()])
    result = reads.observe(predecessor=_predecessor(previous))
    assert result["applied_status"]["program_binding_id"] == APPLY["prepared_activation"]["program_binding_id"]
    assert reads.sleeps == [0.02]


@pytest.mark.parametrize("kind", ["foreign", "missing-transaction", None])
def test_captured_predecessor_requires_a_known_publication_transaction(kind):
    previous = _sample()
    predecessor = _predecessor(previous)
    if kind is None:
        predecessor["publication"] = None
    elif kind == "missing-transaction":
        del predecessor["publication"]["transaction"]
    else:
        predecessor["publication"]["transaction"]["kind"] = kind
    reads = Observations([previous])
    with pytest.raises(AssertionError):
        reads.observe(predecessor=predecessor)
    assert reads.calls == [] and reads.sleeps == []


@pytest.mark.parametrize("change", [{"scenarioId": "foreign"},
                                    {"deploymentRevision": "foreign"},
                                    {"scenarioId": None}])
def test_missing_activation_order_does_not_hide_foreign_or_incoherent_identity(change):
    sample = _sample()
    sample["selected"]["activationOrder"] = None
    sample["selected"].update(change)
    reads = Observations([sample])
    with pytest.raises(AssertionError):
        reads.observe()
    assert reads.sleeps == []


def test_accepted_partial_composer_tuple_can_publish_its_order_later():
    pending = _sample()
    pending["selected"]["activationOrder"] = None
    reads = Observations([pending, _sample()])
    result = reads.observe()
    assert result["managerApplied"]["activationOrder"] is not None
    assert reads.sleeps == [0.02]
