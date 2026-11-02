#include <manyforge_deployment/execution_runtime_contract.hpp>

#include "execution_runtime_contract_identity_internal.hpp"
#include <manyforge_deployment/content_identity.hpp>

namespace manyforge::deployment::detail::execution_runtime_contract_identity {

[[nodiscard]] std::string contract_revision(std::string_view domain,
                                            std::string_view description) {
  std::string preimage("manyforge.native-execution-runtime-contract.v1");
  preimage.push_back('\0');
  preimage.append(domain);
  preimage.push_back('\0');
  preimage.append(description);
  return sha256_digest(preimage);
}

} // namespace manyforge::deployment::detail::execution_runtime_contract_identity

namespace manyforge::deployment::execution_runtime_contract {

using detail::execution_runtime_contract_identity::contract_revision;

const core::ManagerLivenessPolicyDTO& manager_liveness_policy() {
  static const core::ManagerLivenessPolicyDTO policy{
      core::RuntimePolicyId(std::string(
          kManagerLivenessPolicyId)),
      core::RuntimePolicyRevision(contract_revision(
          "manager-liveness-policy",
          "retained-v1;heartbeat-period-ns=250000000;"
          "expiry-timeout-ns=1000000000;"
          "local-safe=manyforge.execution_host.manager_loss_safe")),
      250'000'000,
      1'000'000'000,
      core::ProductContractId(std::string(
          kManagerLivenessLocalSafeContractId)),
      core::ProductContractRevision(contract_revision(
          "manager-loss-local-safe",
          "native-kernel-direct-fail-safe;full-stop;loss-inhibit-v1"))};
  return policy;
}

} // namespace manyforge::deployment::execution_runtime_contract
