#!/usr/bin/env bash
# Exercise the fake RT/NRT ownership boundary under ThreadSanitizer.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_TSAN_BUILD_ROOT:-/tmp/manyforge-build/real-lite6-tsan}

MANYFORGE_REAL_LITE6_BUILD_ROOT=${BUILD_ROOT} \
MANYFORGE_REAL_LITE6_BUILD_PROFILE=tsan \
    "${WORKSPACE}/scripts/ros/build-real-lite6-runtime.sh"

set +u
source "${BUILD_ROOT}/install/setup.bash"
set -u

export TSAN_OPTIONS=halt_on_error=1

"${BUILD_ROOT}/build/manyforge_xarm_control/test_end_to_end_fake" \
    --gtest_filter='FakeBackendConcurrencyTest.*' \
    --gtest_brief=1

"${BUILD_ROOT}/build/manyforge_xarm_control/test_lite6_production_path" \
    --gtest_filter='ProductionPathTest.SlowPrimitiveDoesNotStarveRefreshStatusOrAuthorityRenewal:ProductionPathTest.SlowCommandGateLeaseDoesNotStarveLifecycleStatus:ProductionPathTest.CompletionAfterTransactionDeadlineIsAuditedButCannotMutateOutcome:ProductionPathTest.PortShutdownJoinsOneInFlightPrimitiveWithoutRedispatch' \
    --gtest_brief=1

echo "Fake xArm lifecycle ThreadSanitizer lane passed."
