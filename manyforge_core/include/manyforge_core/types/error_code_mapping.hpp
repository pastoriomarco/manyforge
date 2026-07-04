#pragma once

#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

ErrorCode error_code_from_dto(const ErrorDTO& error);

}  // namespace manyforge::core
