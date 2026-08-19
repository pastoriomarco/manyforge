#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_core/types/runtime_contract_manifest.hpp>

#include "binding_registrations.hpp"

#include <string>
#include <utility>

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

template <typename StrongType>
void bind_strong_string(py::module_& module, const char* python_name) {
  const std::string type_name{python_name};
  py::class_<StrongType>(module, python_name, py::is_final())
      .def(py::init<std::string>(), py::arg("value"))
      .def_property_readonly("value", &StrongType::value)
      .def("__str__", [](const StrongType& value) { return value.value(); })
      .def("__repr__",
           [type_name](const StrongType& value) {
             return py::str("{}({})").format(type_name, py::repr(py::str(value.value())));
           })
      .def("__eq__",
           [](const StrongType& left, const StrongType& right) {
             return left == right;
           },
           py::is_operator())
      .def("__ne__",
           [](const StrongType& left, const StrongType& right) {
             return left != right;
           },
           py::is_operator())
      .def("__hash__", [type_name](const StrongType& value) {
        return py::hash(py::make_tuple(type_name, value.value()));
      });
}

template <typename ResultType>
void require_ok(const ResultType& result) {
  if (!result.ok()) {
    const auto& error = result.error();
    const auto reason = error.details.find("reason");
    throw py::value_error(
        reason == error.details.end() ? error.message : reason->second);
  }
}

void bind_capability_ref(py::module_& module) {
  py::class_<RuntimeCapabilityContractRefDTO>(
      module, "RuntimeCapabilityContractRefDTO", py::is_final())
      .def(py::init<CapabilityId, CapabilityDefinitionRevision>(),
           py::arg("capability_id"),
           py::arg("capability_definition_revision"))
      .def_readonly("capability_id",
                    &RuntimeCapabilityContractRefDTO::capability_id)
      .def_readonly("capability_definition_revision",
                    &RuntimeCapabilityContractRefDTO::
                        capability_definition_revision)
      .def("__eq__",
           [](const RuntimeCapabilityContractRefDTO& left,
              const RuntimeCapabilityContractRefDTO& right) {
             return left == right;
           },
           py::is_operator())
      .def("__ne__",
           [](const RuntimeCapabilityContractRefDTO& left,
              const RuntimeCapabilityContractRefDTO& right) {
             return left != right;
           },
           py::is_operator());
}

void bind_offer(py::module_& module) {
  py::class_<RuntimeContractOfferDTO>(module, "RuntimeContractOfferDTO",
                                      py::is_final())
      .def(py::init<RuntimeContractOfferDigest, RuntimeImplementationFamilyId,
                    std::vector<ApiContractId>,
                    std::vector<RuntimeCapabilityContractRefDTO>>(),
           py::arg("offer_digest"), py::arg("runtime_implementation_id"),
           py::arg("provided_api_contract_ids"),
           py::arg("provided_capabilities"))
      .def_readonly("offer_digest", &RuntimeContractOfferDTO::offer_digest)
      .def_readonly("runtime_implementation_id",
                    &RuntimeContractOfferDTO::runtime_implementation_id)
      .def_readonly("provided_api_contract_ids",
                    &RuntimeContractOfferDTO::provided_api_contract_ids)
      .def_readonly("provided_capabilities",
                    &RuntimeContractOfferDTO::provided_capabilities)
      .def("__eq__",
           [](const RuntimeContractOfferDTO& left,
              const RuntimeContractOfferDTO& right) { return left == right; },
           py::is_operator())
      .def("__ne__",
           [](const RuntimeContractOfferDTO& left,
              const RuntimeContractOfferDTO& right) {
             return left != right;
           },
           py::is_operator());
}

void bind_requirement(py::module_& module) {
  py::class_<RuntimeContractRequirementDTO>(
      module, "RuntimeContractRequirementDTO", py::is_final())
      .def(py::init<RuntimeContractRequirementDigest,
                    RuntimeImplementationFamilyId, std::vector<ApiContractId>,
                    std::vector<RuntimeCapabilityContractRefDTO>>(),
           py::arg("requirement_digest"),
           py::arg("runtime_implementation_id"),
           py::arg("required_api_contract_ids"),
           py::arg("required_capabilities"))
      .def_readonly("requirement_digest",
                    &RuntimeContractRequirementDTO::requirement_digest)
      .def_readonly("runtime_implementation_id",
                    &RuntimeContractRequirementDTO::runtime_implementation_id)
      .def_readonly("required_api_contract_ids",
                    &RuntimeContractRequirementDTO::required_api_contract_ids)
      .def_readonly("required_capabilities",
                    &RuntimeContractRequirementDTO::required_capabilities)
      .def("__eq__",
           [](const RuntimeContractRequirementDTO& left,
              const RuntimeContractRequirementDTO& right) {
             return left == right;
           },
           py::is_operator())
      .def("__ne__",
           [](const RuntimeContractRequirementDTO& left,
              const RuntimeContractRequirementDTO& right) {
             return left != right;
           },
           py::is_operator());
}

void bind_manifest_set(py::module_& module) {
  py::class_<RuntimeContractManifestSetDTO>(
      module, "RuntimeContractManifestSetDTO", py::is_final())
      .def(py::init<RuntimeContractManifestSetDigest,
                    std::vector<RuntimeContractOfferDTO>>(),
           py::arg("manifest_set_digest"), py::arg("offers"))
      .def_readonly("manifest_set_digest",
                    &RuntimeContractManifestSetDTO::manifest_set_digest)
      .def_readonly("offers", &RuntimeContractManifestSetDTO::offers)
      .def("__eq__",
           [](const RuntimeContractManifestSetDTO& left,
              const RuntimeContractManifestSetDTO& right) {
             return left == right;
           },
           py::is_operator())
      .def("__ne__",
           [](const RuntimeContractManifestSetDTO& left,
              const RuntimeContractManifestSetDTO& right) {
             return left != right;
           },
           py::is_operator());
}

}  // namespace

void register_runtime_contract_manifest_bindings(py::module_& module) {
  bind_strong_string<RuntimeImplementationFamilyId>(
      module, "RuntimeImplementationFamilyId");
  bind_strong_string<ApiContractId>(module, "ApiContractId");
  bind_strong_string<RuntimeContractOfferDigest>(
      module, "RuntimeContractOfferDigest");
  bind_strong_string<RuntimeContractRequirementDigest>(
      module, "RuntimeContractRequirementDigest");
  bind_strong_string<RuntimeContractManifestSetDigest>(
      module, "RuntimeContractManifestSetDigest");

  bind_capability_ref(module);
  bind_offer(module);
  bind_requirement(module);
  bind_manifest_set(module);

  module.attr("RUNTIME_CONTRACT_OFFER_API_VERSION") =
      std::string(kRuntimeContractOfferApiVersion);
  module.attr("RUNTIME_CONTRACT_REQUIREMENT_API_VERSION") =
      std::string(kRuntimeContractRequirementApiVersion);
  module.attr("RUNTIME_CONTRACT_MANIFEST_SET_API_VERSION") =
      std::string(kRuntimeContractManifestSetApiVersion);
  module.attr("RUNTIME_CONTRACT_OFFER_IDENTITY_DOMAIN") =
      std::string(kRuntimeContractOfferIdentityDomain);
  module.attr("RUNTIME_CONTRACT_REQUIREMENT_IDENTITY_DOMAIN") =
      std::string(kRuntimeContractRequirementIdentityDomain);
  module.attr("RUNTIME_CONTRACT_MANIFEST_SET_IDENTITY_DOMAIN") =
      std::string(kRuntimeContractManifestSetIdentityDomain);
  module.attr("MAX_RUNTIME_CONTRACT_API_CONTRACT_IDS") =
      kMaxRuntimeContractApiContractIds;
  module.attr("MAX_RUNTIME_CONTRACT_API_CONTRACTS") =
      kMaxRuntimeContractApiContracts;
  module.attr("MAX_RUNTIME_CONTRACT_CAPABILITY_REFS") =
      kMaxRuntimeContractCapabilityRefs;
  module.attr("MAX_RUNTIME_CONTRACT_CAPABILITIES") =
      kMaxRuntimeContractCapabilities;
  module.attr("MAX_RUNTIME_CONTRACT_MANIFEST_OFFERS") =
      kMaxRuntimeContractManifestOffers;
  module.attr("MAX_RUNTIME_CONTRACT_OFFERS") = kMaxRuntimeContractOffers;

  module.def(
      "make_runtime_contract_offer",
      [](RuntimeImplementationFamilyId family,
         std::vector<ApiContractId> api_contract_ids,
         std::vector<RuntimeCapabilityContractRefDTO> capabilities) {
        const auto result = MakeRuntimeContractOffer(
            std::move(family), std::move(api_contract_ids),
            std::move(capabilities));
        require_ok(result);
        return result.value();
      },
      py::arg("runtime_implementation_id"),
      py::arg("provided_api_contract_ids"),
      py::arg("provided_capabilities"));
  module.def("verify_runtime_contract_offer",
             [](const RuntimeContractOfferDTO& offer) {
               require_ok(VerifyRuntimeContractOffer(offer));
             });
  module.def("encode_runtime_contract_offer",
             [](const RuntimeContractOfferDTO& offer) {
               const auto result = EncodeRuntimeContractOffer(offer);
               require_ok(result);
               return result.value();
             });
  module.def("decode_runtime_contract_offer",
             [](std::string canonical_bytes) {
               const auto result =
                   DecodeRuntimeContractOffer(canonical_bytes);
               require_ok(result);
               return result.value();
             });

  module.def(
      "make_runtime_contract_requirement",
      [](RuntimeImplementationFamilyId family,
         std::vector<ApiContractId> api_contract_ids,
         std::vector<RuntimeCapabilityContractRefDTO> capabilities) {
        const auto result = MakeRuntimeContractRequirement(
            std::move(family), std::move(api_contract_ids),
            std::move(capabilities));
        require_ok(result);
        return result.value();
      },
      py::arg("runtime_implementation_id"),
      py::arg("required_api_contract_ids"),
      py::arg("required_capabilities"));
  module.def("verify_runtime_contract_requirement",
             [](const RuntimeContractRequirementDTO& requirement) {
               require_ok(VerifyRuntimeContractRequirement(requirement));
             });
  module.def("encode_runtime_contract_requirement",
             [](const RuntimeContractRequirementDTO& requirement) {
               const auto result =
                   EncodeRuntimeContractRequirement(requirement);
               require_ok(result);
               return result.value();
             });
  module.def("decode_runtime_contract_requirement",
             [](std::string canonical_bytes) {
               const auto result =
                   DecodeRuntimeContractRequirement(canonical_bytes);
               require_ok(result);
               return result.value();
             });

  module.def("make_runtime_contract_manifest_set",
             [](std::vector<RuntimeContractOfferDTO> offers) {
               const auto result =
                   MakeRuntimeContractManifestSet(std::move(offers));
               require_ok(result);
               return result.value();
             });
  module.def("verify_runtime_contract_manifest_set",
             [](const RuntimeContractManifestSetDTO& manifest_set) {
               require_ok(VerifyRuntimeContractManifestSet(manifest_set));
             });
  module.def("encode_runtime_contract_manifest_set",
             [](const RuntimeContractManifestSetDTO& manifest_set) {
               const auto result =
                   EncodeRuntimeContractManifestSet(manifest_set);
               require_ok(result);
               return result.value();
             });
  module.def("decode_runtime_contract_manifest_set",
             [](std::string canonical_bytes) {
               const auto result =
                   DecodeRuntimeContractManifestSet(canonical_bytes);
               require_ok(result);
               return result.value();
             });
  module.def("check_runtime_contract_compatibility",
             [](const RuntimeContractOfferDTO& offer,
                const RuntimeContractRequirementDTO& requirement) {
               require_ok(
                   CheckRuntimeContractCompatibility(offer, requirement));
             });
}

}  // namespace manyforge::core::python
