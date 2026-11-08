from __future__ import annotations

import asyncio
import json
import queue
import threading

from manyforge_composer.backend.execution_observation_stream import ExecutionObservationSource


class Upstream:
    def __init__(self):
        self.cursors = queue.Queue()
        self.responses = queue.Queue()

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        pass

    def next(self, cursor, cancelled):
        self.cursors.put(cursor)
        while not cancelled.is_set():
            try:
                return self.responses.get(timeout=0.01)
            except queue.Empty:
                continue
        raise RuntimeError("cancelled")


class Client:
    def __init__(self):
        self.execution = Upstream()
        self.outside = Upstream()

    def open_execution_observation_stream(self):
        return self.execution

    def open_viewer_pose_stream(self):
        return self.outside


def execution(sequence, position):
    return {"api_version": "manyforge.execution_command_response.v4", "command": "observation", "ok": True,
            "observation": {"schema_version": "manyforge.execution_observation.v4", "owner_epoch": "owner",
                "cell_id": "cell", "retained_bundle_id": "bundle", "graph_revision": "graph",
                "source_epoch": "execution", "source_sequence": sequence, "measured_targets": [],
                "targets": [{"controllable_component_id": "robot", "command_target_id": "arm",
                    "operation_capability_id": "execution.trajectory.execute", "source_epoch": "arm",
                    "source_sequence": sequence, "source_joint_names": ["joint"],
                    "compiled_joint_names": ["robot__joint"], "coordinate_kinds": ["angular"],
                    "positions_si": [position]}]}}


def pose(generation):
    return {"generation": generation,
            "identity": {"cell_id": "cell", "retained_bundle_id": "bundle", "graph_revision": "graph"},
            "sources": [{"source": {"source_sequence": generation}, "age_at_delivery_ns": 0,
                         "freshness_budget_ns": 1_000_000_000}]}


def cursor(frame):
    return (frame["owner_epoch"], frame["source_epoch"], frame["source_sequence"], frame["delivery_sequence"])


def test_outside_only_frames_cannot_starve_execution_owner_reads():
    async def exercise():
        client = Client()
        source = ExecutionObservationSource(client)
        browser = source.subscribe()
        try:
            source.grant_credit(browser, None)
            assert await asyncio.to_thread(client.execution.cursors.get, True, 1) is None
            assert await asyncio.to_thread(client.outside.cursors.get, True, 1) is None
            client.outside.responses.put(pose(1))
            assert await asyncio.to_thread(client.outside.cursors.get, True, 1) == 1
            client.execution.responses.put(execution(1, 0.1))
            await asyncio.wait_for(browser.wake.wait(), 1)
            first = json.loads(source.take_payload(browser))
            client.outside.responses.put(pose(2))
            assert await asyncio.to_thread(client.outside.cursors.get, True, 1) == 2
            source.grant_credit(browser, cursor(first))
            await asyncio.wait_for(browser.wake.wait(), 1)
            outside_only = json.loads(source.take_payload(browser))
            assert outside_only["source_sequence"] == 1
            assert outside_only["outside_pose_sources"][0]["source"]["source_sequence"] == 2
            # The outside frame consumed the browser credit, but the same
            # credit still establishes an independent execution-owner read.
            request = await asyncio.to_thread(client.execution.cursors.get, True, 1)
            assert request["source_sequence"] == 1
            client.execution.responses.put(execution(2, 0.2))
            # Wait until the execution worker has consumed the response.
            for _ in range(100):
                with source._condition:
                    if source._last_identity == ("owner", "execution", 2):
                        break
                await asyncio.sleep(0.001)
            else:
                raise AssertionError("execution update was not admitted")
            source.grant_credit(browser, cursor(outside_only))
            await asyncio.wait_for(browser.wake.wait(), 1)
            latest = json.loads(source.take_payload(browser))
            assert latest["source_sequence"] == 2
            assert latest["targets"][0]["positions_si"] == [0.2]
        finally:
            source.unsubscribe(browser)
            source.close()

    asyncio.run(exercise())


def test_outside_update_consumes_rearmed_credit_and_keeps_latest_execution():
    async def exercise():
        client = Client()
        source = ExecutionObservationSource(client)
        browser = source.subscribe()
        admitted = threading.Event()
        pending_after_admission = []
        admit_response = source._admit_response

        def capture_admission(response, cancel):
            # Observe the response before the worker can start another read.
            with source._condition:
                admit_response(response, cancel)
                if response["observation"]["source_sequence"] == 3:
                    pending_after_admission.append(source._execution_read_pending)
                    admitted.set()

        source._admit_response = capture_admission
        try:
            source.grant_credit(browser, None)
            assert await asyncio.to_thread(
                client.execution.cursors.get, True, 1
            ) is None
            assert await asyncio.to_thread(
                client.outside.cursors.get, True, 1
            ) is None
            client.outside.responses.put(pose(1))
            assert await asyncio.to_thread(client.outside.cursors.get, True, 1) == 1
            client.execution.responses.put(execution(1, 0.1))
            await asyncio.wait_for(browser.wake.wait(), 1)
            first = json.loads(source.take_payload(browser))
            assert (
                first["outside_pose_sources"][0]["source"]["source_sequence"]
                == 1
            )

            source.grant_credit(browser, cursor(first))
            assert await asyncio.to_thread(
                client.execution.cursors.get, True, 1
            ) == {
                "owner_epoch": "owner",
                "source_epoch": "execution",
                "source_sequence": 1,
            }
            # A visual heartbeat re-arms the execution read without using the
            # outstanding credit.
            client.execution.responses.put(execution(2, 0.1))
            assert await asyncio.to_thread(
                client.execution.cursors.get, True, 1
            ) == {
                "owner_epoch": "owner",
                "source_epoch": "execution",
                "source_sequence": 2,
            }
            assert source.status()["fanout"]["coalesced_frames_total"] == 1
            assert not browser.wake.is_set()

            # This outside-only frame consumes the credit while the re-armed
            # execution read remains in flight.
            client.outside.responses.put(pose(2))
            assert await asyncio.to_thread(client.outside.cursors.get, True, 1) == 2
            await asyncio.wait_for(browser.wake.wait(), 1)
            outside_only = json.loads(source.take_payload(browser))
            assert outside_only["source_sequence"] == 2
            assert (
                outside_only["outside_pose_sources"][0]["source"]["source_sequence"]
                == 2
            )

            client.execution.responses.put(execution(3, 0.1))
            assert await asyncio.to_thread(admitted.wait, 1.0)
            assert pending_after_admission == [False]
            assert not browser.wake.is_set()
            assert source.status()["fanout"]["coalesced_frames_total"] == 2

            source.grant_credit(browser, cursor(outside_only))
            assert await asyncio.to_thread(client.execution.cursors.get, True, 1) == {
                "owner_epoch": "owner",
                "source_epoch": "execution",
                "source_sequence": 3,
            }
            client.execution.responses.put(execution(4, 0.2))
            await asyncio.wait_for(browser.wake.wait(), 1)
            latest = json.loads(source.take_payload(browser))
            assert latest["source_sequence"] == 4
            assert latest["targets"][0]["positions_si"] == [0.2]
        finally:
            source.unsubscribe(browser)
            source.close()

    asyncio.run(exercise())
