"""OD-47: classify only the innermost non-runtime access/allocation frames."""

import importlib.util
import contextlib
import io
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "ros_tsan_classifier", ROOT / "scripts/ci/classify-ros-backed-tsan.py"
)
classifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(classifier)


def report(access: str, previous: str = "#0 sdk::read() /opt/sdk/read.cpp:5",
           location: str = "Location is heap block of size 8 allocated by thread T1:\n"
                           "    #0 malloc /src/libsanitizer/interceptor.cpp:1\n"
                           "    #1 sdk::allocate() /opt/sdk/allocator.cpp:5") -> str:
    return (
        "WARNING: ThreadSanitizer: data race\n"
        f"  Write of size 4 at 0x10 by thread T1:\n    {access}\n\n"
        f"  Previous read of size 4 at 0x10 by thread T2:\n    {previous}\n\n"
        f"  {location}\n\nSUMMARY: ThreadSanitizer: data race\n"
    )


class RosTsanClassifierTest(unittest.TestCase):
    def test_recorded_fastdds_allocation_with_deeper_manyforge_caller_is_sdk(self):
        fixture = ROOT / "tests/fixtures/ros_tsan_fastdds_sdk.txt"
        self.assertFalse(classifier.report_is_project(fixture.read_text()))

    def test_first_write_access_in_manyforge_fails(self):
        self.assertTrue(classifier.report_is_project(report(
            "#0 manyforge::core::Worker::update() /source/manyforge_core/src/worker.cpp:17"
        )))

    def test_second_access_in_manyforge_fails(self):
        self.assertTrue(classifier.report_is_project(report(
            "#0 sdk::write() /opt/sdk/write.cpp:1",
            "#0 manyforge::core::Worker::read() /source/manyforge_core/src/worker.cpp:9",
        )))

    def test_manyforge_allocation_after_runtime_frames_fails(self):
        location = (
            "Location is heap block of size 8 allocated by thread T1:\n"
            "    #0 operator new(unsigned long) /src/libsanitizer/tsan.cpp:2\n"
            "    #1 std::allocator<int>::allocate(unsigned long) /usr/include/c++/13/bits/allocator.h:9\n"
            "    #2 manyforge::core::Worker::make() /source/manyforge_core/src/worker.cpp:8"
        )
        self.assertTrue(classifier.report_is_project(report(
            "#0 sdk::write() /opt/sdk/write.cpp:1", location=location
        )))

    def test_manyforge_global_fails(self):
        self.assertTrue(classifier.report_is_project(report(
            "#0 sdk::write() /opt/sdk/write.cpp:1",
            location="Location is global 'manyforge::core::registry' of size 8 at 0x10",
        )))

    def test_sdk_access_with_deeper_manyforge_caller_is_not_project(self):
        access = (
            "#0 memcpy /src/libsanitizer/interceptor.cpp:1\n"
            "    #1 sdk::write() /opt/sdk/write.cpp:1\n"
            "    #2 manyforge::core::Worker::call() /source/manyforge_core/src/worker.cpp:7"
        )
        self.assertFalse(classifier.report_is_project(report(access)))

    def test_sdk_template_with_manyforge_type_is_not_own_frame(self):
        self.assertFalse(classifier.report_is_project(report(
            "#0 sdk::Client<manyforge::core::Message>::send() /opt/sdk/client.hpp:8"
        )))

    def test_sdk_function_argument_with_manyforge_type_is_not_own_frame(self):
        self.assertFalse(classifier.report_is_project(report(
            "#0 sdk::send(manyforge::core::Message*) /opt/sdk/client.hpp:8"
        )))

    def test_unrelated_standard_library_frame_is_a_library_boundary(self):
        access = (
            "#0 std::thread::detach() /usr/include/c++/13/bits/std_thread.h:100\n"
            "    #1 manyforge::core::Worker::call() /source/manyforge_core/src/worker.cpp:7"
        )
        self.assertFalse(classifier.report_is_project(report(access)))

    def test_truncated_report_is_visible_nonfailing_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tsan.1"
            path.write_text(
                "WARNING: ThreadSanitizer: data race\n"
                "  Write of size 4 at 0x10 by thread T1:\n"
            )
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertFalse(classifier.classify(path))
            self.assertIn("SDK race, to classify", output.getvalue())

    def test_runtime_fatal_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "console.log"
            path.write_text("FATAL: ThreadSanitizer: unexpected memory mapping\n")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertTrue(classifier.classify(path))

    def test_each_report_is_classified_independently(self):
        sdk = report("#0 sdk::write() /opt/sdk/write.cpp:1")
        owned = report("#0 manyforge::core::Worker::write() /source/manyforge_core/src/worker.cpp:3")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tsan.1"
            path.write_text(sdk + "\n" + owned)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertTrue(classifier.classify(path))
            self.assertIn("SDK race, to classify", output.getvalue())
            self.assertIn("ManyForge ThreadSanitizer report 2", output.getvalue())

    def test_lock_acquisition_by_manyforge_is_reported(self):
        self.assertTrue(classifier.report_is_project(
            "WARNING: ThreadSanitizer: lock-order-inversion\n"
            "  Mutex M2 acquired here while holding mutex M1:\n"
            "    #0 pthread_mutex_lock /src/libsanitizer/interceptor.cpp:1\n"
            "    #1 manyforge::core::Worker::lock() /source/manyforge_core/src/worker.cpp:7\n"
        ))

    def test_fastdds_internal_lock_order_cycles_do_not_fail(self):
        # Three real reports (Thor, 2026-11-11), frames compacted: every mutex
        # is taken in libfastdds; ManyForge appears only as a deeper caller.
        text = (ROOT / "tests/fixtures/ros_tsan_fastdds_lock_order.txt").read_text()
        reports = [r for r in text.split("WARNING: ThreadSanitizer:")[1:]]
        self.assertEqual(len(reports), 3)
        for body in reports:
            self.assertFalse(classifier.report_fails("WARNING: ThreadSanitizer:" + body))

    def test_manyforge_leaf_acquisition_in_a_cycle_fails(self):
        # The OD-111 shape: the leaf is taken through libstdc++ wrappers in a
        # ManyForge module while rclcpp holds its goal-request lock.
        inversion = (
            "WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)\n"
            "  Cycle in lock order graph: M0 => M1 => M0\n\n"
            "  Mutex M1 acquired here while holding mutex M0 in thread T1:\n"
            "    #0 pthread_mutex_lock /src/libsanitizer/interceptor.cpp:1 (libtsan.so.2+0x1)\n"
            "    #1 __gthread_mutex_lock /usr/include/c++/13/gthr-default.h:749"
            " (libmanyforge_ros2_control_execution.so+0x2)\n"
            "    #2 std::timed_mutex::lock() /usr/include/c++/13/mutex:250"
            " (libmanyforge_ros2_control_execution.so+0x3)\n"
            "    #3 std::unique_lock::lock() /usr/include/c++/13/bits/unique_lock.h:141"
            " (libmanyforge_ros2_control_execution.so+0x4)\n"
            "    #4 manyforge::ros2_control_execution::Ros2ControlExecutionBackend::SendAutomaticCancel()"
            " /source/manyforge_ros2_control_execution/src/ros2_control_execution_backend.cpp:1347"
            " (libmanyforge_ros2_control_execution.so+0x5)\n\n"
            "  Mutex M0 previously acquired by the same thread here:\n"
            "    #0 pthread_mutex_lock /src/libsanitizer/interceptor.cpp:1 (libtsan.so.2+0x1)\n"
            "    #1 rclcpp_action::ClientBase::handle_goal_response() <null> (librclcpp_action.so+0x6)\n\n"
            "  Mutex M0 acquired here while holding mutex M1 in thread T2:\n"
            "    #0 pthread_mutex_lock /src/libsanitizer/interceptor.cpp:1 (libtsan.so.2+0x1)\n"
            "    #1 rclcpp_action::ClientBase::send_goal_request() <null> (librclcpp_action.so+0x7)\n\n"
            "SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock)\n"
        )
        self.assertTrue(classifier.report_fails(inversion))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tsan.1"
            path.write_text(inversion)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertTrue(classifier.classify(path))
            self.assertIn("ManyForge ThreadSanitizer report 1", output.getvalue())

    def test_sdk_only_lock_order_inversion_stays_to_classify(self):
        inversion = (
            "WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)\n"
            "  Mutex M1 acquired here while holding mutex M0 in thread T1:\n"
            "    #0 pthread_mutex_lock /src/libsanitizer/interceptor.cpp:1\n"
            "    #1 sdk::lock() /opt/sdk/lock.cpp:3\n"
        )
        self.assertFalse(classifier.report_fails(inversion))

if __name__ == "__main__":
    unittest.main()
