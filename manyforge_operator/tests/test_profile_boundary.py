from manyforge_operator import (
    LaunchInstanceState,
    ReadinessState,
)
from manyforge_operator.profile import (
    ProfileAdapter,
    ProfileGracefulStopRequest,
    ProfileGracefulStopResult,
    ProfileObserveRequest,
    ProfileObserveResult,
    ProfileStartRequest,
    ProfileStartResult,
)
from .conftest import make_readiness


def test_profile_requests_carry_the_complete_configuration(configuration_factory):
    configuration = configuration_factory()
    request = ProfileStartRequest(
        launch_instance_id="launch-1",
        configuration=configuration,
    )
    assert request.configuration == configuration
    assert request.configuration.configuration_digest == configuration.configuration_digest
    assert not hasattr(request, "configuration_digest")
    assert not hasattr(request, "profile")


def test_profile_start_observe_and_graceful_stop_dtos_are_typed(
    configuration_factory, status_factory
):
    configuration = configuration_factory()
    status = status_factory(configuration, with_resources=True)
    start = ProfileStartResult(
        launch_instance_id="launch-1",
        state=LaunchInstanceState.STARTING,
        resource_observations=status.resource_observations,
        readiness_observations=[],
    )
    observe_request = ProfileObserveRequest(
        launch_instance_id="launch-1",
        owned_resources=status.owned_resources,
    )
    observe = ProfileObserveResult(
        launch_instance_id=observe_request.launch_instance_id,
        state=LaunchInstanceState.READY,
        resource_observations=status.resource_observations,
        readiness_observations=[make_readiness()],
    )
    stop_request = ProfileGracefulStopRequest(
        launch_instance_id="launch-1",
        owned_resources=observe_request.owned_resources,
    )
    stop = ProfileGracefulStopResult(
        launch_instance_id=stop_request.launch_instance_id,
        state=LaunchInstanceState.STOPPED,
    )
    assert start.state is LaunchInstanceState.STARTING
    assert observe.readiness_observations[0].state is ReadinessState.READY
    assert stop.state is LaunchInstanceState.STOPPED
    assert ProfileAdapter.start
    assert ProfileAdapter.observe
    assert ProfileAdapter.request_graceful_stop


def test_profile_module_has_no_effectful_runtime_imports():
    import manyforge_operator.profile as profile

    forbidden = {"subprocess", "socket", "requests", "fastapi", "docker"}
    assert forbidden.isdisjoint(vars(profile))
    assert getattr(ProfileAdapter, "_is_protocol", False)
