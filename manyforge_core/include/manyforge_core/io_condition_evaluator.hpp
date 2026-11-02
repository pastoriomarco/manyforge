#pragma once

#include <manyforge_core/types/io_condition.hpp>
#include <span>

namespace manyforge::core {

class CheckedIoCondition;

/// Bool/string: equality and inequality. Numbers: all six comparisons,
/// including exact mixed int64/finite-double ordering without rounding the int.
/// Bool/numeric, string/other, nonfinite values and unsupported nodes refuse.
[[nodiscard]] Result<CheckedIoCondition> CheckIoCondition(IoConditionDefinitionDTO definition,
                                                          IoConditionLimitsDTO limits);

/// Pure evaluation of one complete, already-qualified snapshot. Missing values
/// or type mismatches return an error, never false. This API defines no unknown
/// short-circuit algebra, freshness, source-loss policy, clock or live cache.
/// Allocate workspace once before entering the signal callback; successful
/// evaluation performs no allocation and reuses its bounded byte span.
[[nodiscard]] Result<bool> EvaluateIoCondition(const CheckedIoCondition& condition,
                                               std::span<const IoConditionValueDTO> known_values,
                                               std::span<std::uint8_t> workspace);

/// Borrow immutable typed inputs without copying retained string payloads. The
/// owner keeps every pointed-to value alive throughout this synchronous call;
/// a null input is unknown and refuses before evaluating any node.
[[nodiscard]] Result<bool> EvaluateIoCondition(
    const CheckedIoCondition& condition, std::span<const IoConditionValueDTO* const> known_values,
    std::span<std::uint8_t> workspace);

class CheckedIoCondition final {
 public:
  [[nodiscard]] std::span<const IoConditionInputDTO> inputs() const noexcept;
  [[nodiscard]] std::size_t workspace_size() const noexcept;

 private:
  CheckedIoCondition(IoConditionDefinitionDTO definition, IoConditionLimitsDTO limits);
  IoConditionDefinitionDTO definition_;
  IoConditionLimitsDTO limits_;

  friend Result<CheckedIoCondition> CheckIoCondition(IoConditionDefinitionDTO,
                                                     IoConditionLimitsDTO);
  friend Result<bool> EvaluateIoCondition(const CheckedIoCondition&,
                                          std::span<const IoConditionValueDTO>,
                                          std::span<std::uint8_t>);
  friend Result<bool> EvaluateIoCondition(const CheckedIoCondition&,
                                          std::span<const IoConditionValueDTO* const>,
                                          std::span<std::uint8_t>);
};

}  // namespace manyforge::core
