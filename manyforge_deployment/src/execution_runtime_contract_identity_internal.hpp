#pragma once

#include <string>
#include <string_view>

namespace manyforge::deployment::detail::execution_runtime_contract_identity {

[[nodiscard]] std::string contract_revision(std::string_view domain,
                                          std::string_view description);

} // namespace manyforge::deployment::detail::execution_runtime_contract_identity
