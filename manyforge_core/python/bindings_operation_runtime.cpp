#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/types/operation_runtime.hpp>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

template <typename Targets>
[[nodiscard]] StopIntentDTO make_stop_intent(PublicRequestContextDTO request_context,
                                             Targets targets, std::optional<std::string> reason,
                                             RequestAttributionDTO attribution) {
  return StopIntentDTO{std::move(request_context), StopSelectorDTO(std::move(targets)),
                       std::move(reason), std::move(attribution)};
}

}  // namespace

void register_operation_runtime_bindings(py::module_& module) {
  module.def(
      "verify_resolved_stop_base_plan_canonical_bytes",
      [](const py::bytes& value) {
        const auto decoded = DecodeResolvedStopBasePlan(value.cast<std::string>());
        if (!decoded.ok()) {
          const auto reason = decoded.error().details.find("reason");
          throw py::value_error(reason == decoded.error().details.end() ? decoded.error().message
                                                                        : reason->second);
        }
        return decoded.value().resolved_stop_base_plan_id;
      },
      py::arg("canonical_bytes"),
      "Verify one exact canonical ResolvedStopBasePlan wire value and return its sole ID.");

  py::enum_<StopResolutionDispositionDTO>(module, "StopResolutionDispositionDTO")
      .value("kExact", StopResolutionDispositionDTO::kExact)
      .value("kStaleButUnambiguouslyResolved",
             StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved)
      .value("kAlreadyTerminalNoEffect", StopResolutionDispositionDTO::kAlreadyTerminalNoEffect)
      .value("kInvalid", StopResolutionDispositionDTO::kInvalid);

  py::enum_<StopScopeResolutionReasonDTO>(module, "StopScopeResolutionReasonDTO")
      .value("kExactCurrentScope", StopScopeResolutionReasonDTO::kExactCurrentScope)
      .value("kStaleMetadataCurrentScope", StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope)
      .value("kKnownTerminalOrIdleNoEffect",
             StopScopeResolutionReasonDTO::kKnownTerminalOrIdleNoEffect)
      .value("kMalformedOrdinaryIntent", StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent)
      .value("kUnknownOperation", StopScopeResolutionReasonDTO::kUnknownOperation)
      .value("kUnknownComponent", StopScopeResolutionReasonDTO::kUnknownComponent)
      .value("kUnknownResource", StopScopeResolutionReasonDTO::kUnknownResource)
      .value("kUnknownProgramRun", StopScopeResolutionReasonDTO::kUnknownProgramRun);

  py::enum_<StopKind>(module, "StopKind")
      .value("kGroups", StopKind::kGroups)
      .value("kStopAll", StopKind::kStopAll)
      .value("kHalt", StopKind::kHalt)
      .value("kOperation", StopKind::kOperation)
      .value("kCellStop", StopKind::kCellStop)
      .value("kRuleFailNode", StopKind::kRuleFailNode)
      .value("kRuleHold", StopKind::kRuleHold)
      .value("kInterruptRule", StopKind::kInterruptRule)
      .value("kRunnerLoss", StopKind::kRunnerLoss)
      .value("kProgramEnd", StopKind::kProgramEnd)
      .value("kInternalFault", StopKind::kInternalFault)
      .value("kTakeover", StopKind::kTakeover)
      .value("kPartialDispatch", StopKind::kPartialDispatch)
      .value("kRuntimeCollision", StopKind::kRuntimeCollision)
      .value("kHoldTimeout", StopKind::kHoldTimeout)
      .value("kOperationReset", StopKind::kOperationReset)
      .value("kPreparedDispatchInvalid", StopKind::kPreparedDispatchInvalid)
      .value("kPlannedStop", StopKind::kPlannedStop)
      .value("kStopFailedHalt", StopKind::kStopFailedHalt);

  py::enum_<StopWidenReason>(module, "StopWidenReason")
      .value("kNone", StopWidenReason::kNone)
      .value("kUnknownTarget", StopWidenReason::kUnknownTarget)
      .value("kMalformed", StopWidenReason::kMalformed)
      .value("kKeyConflict", StopWidenReason::kKeyConflict)
      .value("kStaleStart", StopWidenReason::kStaleStart)
      .value("kUnreadableKind", StopWidenReason::kUnreadableKind);

  py::class_<StopStaleDiagnosticDTO>(module, "StopStaleDiagnosticDTO", py::is_final())
      .def(py::init<OperationNumber, OperationRevision, OperationRevision>(),
           py::arg("operation_id"), py::arg("expected_operation_revision"),
           py::arg("current_operation_revision"))
      .def_readonly("operation_id", &StopStaleDiagnosticDTO::operation_id)
      .def_readonly("expected_operation_revision",
                    &StopStaleDiagnosticDTO::expected_operation_revision)
      .def_readonly("current_operation_revision",
                    &StopStaleDiagnosticDTO::current_operation_revision)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopOperationTargetDTO>(module, "StopOperationTargetDTO", py::is_final())
      .def(py::init<OperationNumber, OperationRevision>(), py::arg("operation_id"),
           py::arg("expected_operation_revision"))
      .def_readonly("operation_id", &StopOperationTargetDTO::operation_id)
      .def_readonly("expected_operation_revision",
                    &StopOperationTargetDTO::expected_operation_revision)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopComponentTargetDTO>(module, "StopComponentTargetDTO", py::is_final())
      .def(py::init<ControllableComponentId, GraphRevision, ComponentLifecycleGeneration>(),
           py::arg("controllable_component_id"), py::arg("expected_graph_revision"),
           py::arg("expected_component_lifecycle_generation"))
      .def_readonly("controllable_component_id", &StopComponentTargetDTO::controllable_component_id)
      .def_readonly("expected_graph_revision", &StopComponentTargetDTO::expected_graph_revision)
      .def_readonly("expected_component_lifecycle_generation",
                    &StopComponentTargetDTO::expected_component_lifecycle_generation)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopResourceTargetDTO>(module, "StopResourceTargetDTO", py::is_final())
      .def(py::init<ResourceId, GraphRevision, std::uint64_t>(), py::arg("resource_id"),
           py::arg("expected_graph_revision"), py::arg("expected_operation_resource_generation"))
      .def_readonly("resource_id", &StopResourceTargetDTO::resource_id)
      .def_readonly("expected_graph_revision", &StopResourceTargetDTO::expected_graph_revision)
      .def_readonly("expected_operation_resource_generation",
                    &StopResourceTargetDTO::expected_operation_resource_generation)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopIntentDTO>(module, "StopIntentDTO", py::is_final())
      .def_static(
          "for_operations",
          [](PublicRequestContextDTO request_context,
             std::vector<StopOperationTargetDTO> operations, std::optional<std::string> reason,
             RequestAttributionDTO attribution) {
            return make_stop_intent(std::move(request_context), std::move(operations),
                                    std::move(reason), std::move(attribution));
          },
          py::arg("request_context"), py::arg("operations"), py::arg("reason") = std::nullopt,
          py::arg("attribution") = RequestAttributionDTO{})
      .def_static(
          "for_components",
          [](PublicRequestContextDTO request_context,
             std::vector<StopComponentTargetDTO> components, std::optional<std::string> reason,
             RequestAttributionDTO attribution) {
            return make_stop_intent(std::move(request_context), std::move(components),
                                    std::move(reason), std::move(attribution));
          },
          py::arg("request_context"), py::arg("components"), py::arg("reason") = std::nullopt,
          py::arg("attribution") = RequestAttributionDTO{})
      .def_static(
          "for_resources",
          [](PublicRequestContextDTO request_context, std::vector<StopResourceTargetDTO> resources,
             std::optional<std::string> reason, RequestAttributionDTO attribution) {
            return make_stop_intent(std::move(request_context), std::move(resources),
                                    std::move(reason), std::move(attribution));
          },
          py::arg("request_context"), py::arg("resources"), py::arg("reason") = std::nullopt,
          py::arg("attribution") = RequestAttributionDTO{})
      .def_readonly("request_context", &StopIntentDTO::request_context)
      .def_readonly("reason", &StopIntentDTO::reason)
      .def_readonly("attribution", &StopIntentDTO::attribution)
      .def_property_readonly(
          "selector_kind",
          [](const StopIntentDTO& value) {
            if (std::holds_alternative<std::vector<StopOperationTargetDTO>>(value.selector)) {
              return std::string("operations");
            }
            if (std::holds_alternative<std::vector<StopComponentTargetDTO>>(value.selector)) {
              return std::string("components");
            }
            return std::string("resources");
          })
      .def_property_readonly(
          "operations",
          [](const StopIntentDTO& value) -> py::object {
            const auto* rows = std::get_if<std::vector<StopOperationTargetDTO>>(&value.selector);
            return rows == nullptr ? py::none() : py::cast(*rows);
          })
      .def_property_readonly(
          "components",
          [](const StopIntentDTO& value) -> py::object {
            const auto* rows = std::get_if<std::vector<StopComponentTargetDTO>>(&value.selector);
            return rows == nullptr ? py::none() : py::cast(*rows);
          })
      .def_property_readonly("resources",
                             [](const StopIntentDTO& value) -> py::object {
                               const auto* rows =
                                   std::get_if<std::vector<StopResourceTargetDTO>>(&value.selector);
                               return rows == nullptr ? py::none() : py::cast(*rows);
                             })
      .def("field_error",
           [](const StopIntentDTO& value) {
             const auto error = stop_intent_field_error(value);
             return error.has_value() ? py::cast(std::string(*error)) : py::none();
           })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<FullStopIntentDTO>(module, "FullStopIntentDTO", py::is_final())
      .def(py::init<PublicRequestContextDTO, RequestAttributionDTO>(), py::arg("request_context"),
           py::arg("attribution") = RequestAttributionDTO{})
      .def_readonly("request_context", &FullStopIntentDTO::request_context)
      .def_readonly("attribution", &FullStopIntentDTO::attribution)
      .def("field_error",
           [](const FullStopIntentDTO& value) {
             const auto error = full_stop_intent_field_error(value);
             return error.has_value() ? py::cast(std::string(*error)) : py::none();
           })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ProgramRunEndIntentDTO>(module, "ProgramRunEndIntentDTO", py::is_final())
      .def(py::init<PublicRequestContextDTO, ProgramRunNumber, RequestAttributionDTO>(),
           py::arg("request_context"), py::arg("program_run_id"),
           py::arg("attribution") = RequestAttributionDTO{})
      .def_readonly("request_context", &ProgramRunEndIntentDTO::request_context)
      .def_readonly("program_run_id", &ProgramRunEndIntentDTO::program_run_id)
      .def_readonly("attribution", &ProgramRunEndIntentDTO::attribution)
      .def("field_error",
           [](const ProgramRunEndIntentDTO& value) {
             const auto error = program_run_end_intent_field_error(value);
             return error.has_value() ? py::cast(std::string(*error)) : py::none();
           })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopCoveredParticipantDTO>(module, "StopCoveredParticipantDTO", py::is_final())
      .def(py::init<OperationNumber, std::uint32_t, bool>(), py::arg("operation_id"),
           py::arg("participant_ordinal"), py::arg("started"))
      .def_readonly("operation_id", &StopCoveredParticipantDTO::operation_id)
      .def_readonly("participant_ordinal", &StopCoveredParticipantDTO::participant_ordinal)
      .def_readonly("started", &StopCoveredParticipantDTO::started)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopAcceptedDTO>(module, "StopAcceptedDTO", py::is_final())
      .def(py::init<RequestKey, RequestReplayStatusDTO, StopEventId, StopKind, StopKind,
                    StopWidenReason, std::optional<ProgramRunNumber>,
                    std::vector<StopStaleDiagnosticDTO>, std::uint32_t,
                    std::vector<StopCoveredParticipantDTO>, std::uint64_t, std::int64_t>(),
           py::arg("request_key"), py::arg("replay_status"), py::arg("event_id"),
           py::arg("requested"), py::arg("applied"), py::arg("widen_reason"),
           py::arg("program_run_id"), py::arg("stale_diagnostics"), py::arg("stale_total"),
           py::arg("covered"), py::arg("hold_revision"), py::arg("accepted_at_ns"))
      .def_readonly("request_key", &StopAcceptedDTO::request_key)
      .def_readonly("replay_status", &StopAcceptedDTO::replay_status)
      .def_readonly("event_id", &StopAcceptedDTO::event_id)
      .def_readonly("requested", &StopAcceptedDTO::requested)
      .def_readonly("applied", &StopAcceptedDTO::applied)
      .def_readonly("widen_reason", &StopAcceptedDTO::widen_reason)
      .def_readonly("program_run_id", &StopAcceptedDTO::program_run_id)
      .def_readonly("stale_diagnostics", &StopAcceptedDTO::stale_diagnostics)
      .def_readonly("stale_total", &StopAcceptedDTO::stale_total)
      .def_readonly("covered", &StopAcceptedDTO::covered)
      .def_readonly("hold_revision", &StopAcceptedDTO::hold_revision)
      .def_readonly("accepted_at_ns", &StopAcceptedDTO::accepted_at_ns)
      .def("field_error",
           [](const StopAcceptedDTO& value) {
             const auto error = stop_accepted_field_error(value);
             return error.has_value() ? py::cast(std::string(*error)) : py::none();
           })
      .def(py::self == py::self)
      .def(py::self != py::self);
}

}  // namespace manyforge::core::python
