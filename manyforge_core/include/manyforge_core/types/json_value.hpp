#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::core {

/// ROS-free JSON-data-model value used by schema-constrained configuration
/// fields. Parsing, schema validation, I-JSON checks, and canonicalization are
/// deliberately outside this passive value contract.
struct JsonValueDTO {
  using Array = std::vector<JsonValueDTO>;
  using Object = std::map<std::string, JsonValueDTO, std::less<>>;
  using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  Value value = nullptr;

  friend bool operator==(const JsonValueDTO&, const JsonValueDTO&) = default;
};

using JsonObjectDTO = JsonValueDTO::Object;

}  // namespace manyforge::core
