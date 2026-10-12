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
      .value("kPromotedToFull", StopResolutionDispositionDTO::kPromotedToFull);

  py::enum_<StopScopeResolutionReasonDTO>(module, "StopScopeResolutionReasonDTO")
      .value("kExactCurrentScope", StopScopeResolutionReasonDTO::kExactCurrentScope)
      .value("kStaleMetadataCurrentScope", StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope)
      .value("kKnownTerminalOrIdleNoEffect",
             StopScopeResolutionReasonDTO::kKnownTerminalOrIdleNoEffect)
      .value("kMalformedOrdinaryIntent", StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent)
      .value("kUnknownOperation", StopScopeResolutionReasonDTO::kUnknownOperation)
      .value("kUnknownProgramRun", StopScopeResolutionReasonDTO::kUnknownProgramRun)
      .value("kUnknownComponent", StopScopeResolutionReasonDTO::kUnknownComponent)
      .value("kUnknownResource", StopScopeResolutionReasonDTO::kUnknownResource)
      .value("kRequestIdentityConflict", StopScopeResolutionReasonDTO::kRequestIdentityConflict)
      .value("kOverlappingStopTransactions",
             StopScopeResolutionReasonDTO::kOverlappingStopTransactions)
      .value("kEmergencyReserveCoalesced", StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced)
      .value("kExplicitFullStop", StopScopeResolutionReasonDTO::kExplicitFullStop);

  py::enum_<StopScopeStaleFieldDTO>(module, "StopScopeStaleFieldDTO")
      .value("kRequestOwnerEpoch", StopScopeStaleFieldDTO::kRequestOwnerEpoch)
      .value("kOperationRevision", StopScopeStaleFieldDTO::kOperationRevision)
      .value("kOperationStopGeneration", StopScopeStaleFieldDTO::kOperationStopGeneration)
      .value("kGraphRevision", StopScopeStaleFieldDTO::kGraphRevision)
      .value("kComponentLifecycleGeneration", StopScopeStaleFieldDTO::kComponentLifecycleGeneration)
      .value("kOperationResourceGeneration", StopScopeStaleFieldDTO::kOperationResourceGeneration)
      .value("kResourceStopGeneration", StopScopeStaleFieldDTO::kResourceStopGeneration)
      .value("kCellStopGeneration", StopScopeStaleFieldDTO::kCellStopGeneration);

  py::class_<StopScopeStaleDiagnosticDTO>(module, "StopScopeStaleDiagnosticDTO", py::is_final())
      .def(py::init<StopScopeStaleFieldDTO, StopScopeDiagnosticTargetDTO, StopScopeStaleValueDTO,
                    StopScopeStaleValueDTO>(),
           py::arg("field"), py::arg("target"), py::arg("expected"), py::arg("current"))
      .def_readonly("field", &StopScopeStaleDiagnosticDTO::field)
      .def_readonly("target", &StopScopeStaleDiagnosticDTO::target)
      .def_readonly("expected", &StopScopeStaleDiagnosticDTO::expected)
      .def_readonly("current", &StopScopeStaleDiagnosticDTO::current)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopOperationTargetDTO>(module, "StopOperationTargetDTO", py::is_final())
      .def(py::init<OperationId, OperationRevision, StopGeneration>(), py::arg("operation_id"),
           py::arg("expected_operation_revision"), py::arg("expected_stop_generation"))
      .def_readonly("operation_id", &StopOperationTargetDTO::operation_id)
      .def_readonly("expected_operation_revision",
                    &StopOperationTargetDTO::expected_operation_revision)
      .def_readonly("expected_stop_generation", &StopOperationTargetDTO::expected_stop_generation)
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
      .def(py::init<ResourceId, GraphRevision, std::uint64_t, StopGeneration>(),
           py::arg("resource_id"), py::arg("expected_graph_revision"),
           py::arg("expected_operation_resource_generation"), py::arg("expected_stop_generation"))
      .def_readonly("resource_id", &StopResourceTargetDTO::resource_id)
      .def_readonly("expected_graph_revision", &StopResourceTargetDTO::expected_graph_revision)
      .def_readonly("expected_operation_resource_generation",
                    &StopResourceTargetDTO::expected_operation_resource_generation)
      .def_readonly("expected_stop_generation", &StopResourceTargetDTO::expected_stop_generation)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopProgramRunTargetDTO>(module, "StopProgramRunTargetDTO", py::is_final())
      .def(py::init<ProgramRunId>(), py::arg("program_run_id"))
      .def_readonly("program_run_id", &StopProgramRunTargetDTO::program_run_id)
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
      .def_static(
          "for_program_runs",
          [](PublicRequestContextDTO request_context,
             std::vector<StopProgramRunTargetDTO> program_runs, std::optional<std::string> reason,
             RequestAttributionDTO attribution) {
            return make_stop_intent(std::move(request_context), std::move(program_runs),
                                    std::move(reason), std::move(attribution));
          },
          py::arg("request_context"), py::arg("program_runs"), py::arg("reason") = std::nullopt,
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
            if (std::holds_alternative<std::vector<StopResourceTargetDTO>>(value.selector)) {
              return std::string("resources");
            }
            return std::string("program_runs");
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
      .def_property_readonly(
          "program_runs",
          [](const StopIntentDTO& value) -> py::object {
            const auto* rows = std::get_if<std::vector<StopProgramRunTargetDTO>>(&value.selector);
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
      .def(py::init<PublicRequestContextDTO, StopGeneration, RequestAttributionDTO>(),
           py::arg("request_context"), py::arg("expected_stop_generation"),
           py::arg("attribution") = RequestAttributionDTO{})
      .def_readonly("request_context", &FullStopIntentDTO::request_context)
      .def_readonly("expected_stop_generation", &FullStopIntentDTO::expected_stop_generation)
      .def_readonly("attribution", &FullStopIntentDTO::attribution)
      .def("field_error",
           [](const FullStopIntentDTO& value) {
             const auto error = full_stop_intent_field_error(value);
             return error.has_value() ? py::cast(std::string(*error)) : py::none();
           })
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopAffectedOperationRevisionDTO>(module, "StopAffectedOperationRevisionDTO",
                                               py::is_final())
      .def(py::init<OperationId, OperationRevision>(), py::arg("operation_id"),
           py::arg("operation_revision_after_acceptance"))
      .def_readonly("operation_id", &StopAffectedOperationRevisionDTO::operation_id)
      .def_readonly("operation_revision_after_acceptance",
                    &StopAffectedOperationRevisionDTO::operation_revision_after_acceptance)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopPredecessorAcceptanceRefDTO>(module, "StopPredecessorAcceptanceRefDTO",
                                              py::is_final())
      .def(py::init<StopId, StopGeneration, ContentDigest>(), py::arg("predecessor_stop_id"),
           py::arg("predecessor_stop_generation"), py::arg("captured_scope_digest"))
      .def_readonly("predecessor_stop_id", &StopPredecessorAcceptanceRefDTO::predecessor_stop_id)
      .def_readonly("predecessor_stop_generation",
                    &StopPredecessorAcceptanceRefDTO::predecessor_stop_generation)
      .def_readonly("captured_scope_digest",
                    &StopPredecessorAcceptanceRefDTO::captured_scope_digest)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<StopAcceptedDTO>(module, "StopAcceptedDTO", py::is_final())
      .def(py::init<RequestId, RequestReplayStatusDTO, StopId, StopGeneration, ContentDigest,
                    StopResolutionDispositionDTO, StopScopeResolutionReasonDTO,
                    std::vector<StopScopeStaleDiagnosticDTO>, ContentDigest,
                    std::vector<StopPredecessorAcceptanceRefDTO>,
                    std::vector<StopAffectedOperationRevisionDTO>, std::int64_t>(),
           py::arg("request_id"), py::arg("replay_status"), py::arg("stop_id"),
           py::arg("stop_generation"), py::arg("requested_selector_digest"),
           py::arg("resolution_disposition"), py::arg("resolution_reason"),
           py::arg("stale_diagnostics"), py::arg("captured_scope_digest"),
           py::arg("joined_predecessor_stops"), py::arg("affected_operation_revisions"),
           py::arg("accepted_at_ns"))
      .def_readonly("request_id", &StopAcceptedDTO::request_id)
      .def_readonly("replay_status", &StopAcceptedDTO::replay_status)
      .def_readonly("stop_id", &StopAcceptedDTO::stop_id)
      .def_readonly("stop_generation", &StopAcceptedDTO::stop_generation)
      .def_readonly("requested_selector_digest", &StopAcceptedDTO::requested_selector_digest)
      .def_readonly("resolution_disposition", &StopAcceptedDTO::resolution_disposition)
      .def_readonly("resolution_reason", &StopAcceptedDTO::resolution_reason)
      .def_readonly("stale_diagnostics", &StopAcceptedDTO::stale_diagnostics)
      .def_readonly("captured_scope_digest", &StopAcceptedDTO::captured_scope_digest)
      .def_readonly("joined_predecessor_stops", &StopAcceptedDTO::joined_predecessor_stops)
      .def_readonly("affected_operation_revisions", &StopAcceptedDTO::affected_operation_revisions)
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
