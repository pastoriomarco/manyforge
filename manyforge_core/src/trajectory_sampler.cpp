#include <manyforge_core/trajectory/trajectory_sampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace manyforge::core {

namespace {

// Polynomial interpolation derived from boundary constraints.
//
// Linear (matches positions at both endpoints):
//   p(s)   = (1 - s) * p_a + s * p_b
//   v(s)   = (p_b - p_a) / T          (constant within segment)
//   a(s)   = 0
//
// Cubic Hermite (matches position and velocity at both endpoints):
//   Basis derived from the four boundary conditions
//   Q(0) = p_a, Q'(0) = v_a * T, Q(1) = p_b, Q'(1) = v_b * T (in s coordinates).
//     H00(s) = 1 - 3 s^2 + 2 s^3      H00'(s) = -6 s + 6 s^2      H00''(s) = -6 + 12 s
//     H10(s) = s - 2 s^2 + s^3        H10'(s) = 1 - 4 s + 3 s^2   H10''(s) = -4 + 6 s
//     H01(s) = 3 s^2 - 2 s^3          H01'(s) = 6 s - 6 s^2       H01''(s) = 6 - 12 s
//     H11(s) = -s^2 + s^3             H11'(s) = -2 s + 3 s^2      H11''(s) = -2 + 6 s
//   Q(s)  = p_a * H00 + T * v_a * H10 + p_b * H01 + T * v_b * H11
//   p(s)  = Q(s);  v(s) = Q'(s) / T;  a(s) = Q''(s) / T^2.
//
// Quintic Hermite (matches position, velocity, acceleration at both endpoints):
//   Basis derived from the six boundary conditions
//   Q(0) = p_a, Q'(0) = v_a * T, Q''(0) = a_a * T^2,
//   Q(1) = p_b, Q'(1) = v_b * T, Q''(1) = a_b * T^2.
//     N0(s) = 1 - 10 s^3 + 15 s^4 - 6 s^5
//     N1(s) = s - 6 s^3 + 8 s^4 - 3 s^5
//     N2(s) = (1/2) s^2 - (3/2) s^3 + (3/2) s^4 - (1/2) s^5
//     N3(s) = 10 s^3 - 15 s^4 + 6 s^5
//     N4(s) = -4 s^3 + 7 s^4 - 3 s^5
//     N5(s) = (1/2) s^3 - s^4 + (1/2) s^5
//   Q(s) = p_a * N0 + T * v_a * N1 + T^2 * a_a * N2
//        + p_b * N3 + T * v_b * N4 + T^2 * a_b * N5
//   p(s) = Q(s);  v(s) = Q'(s) / T;  a(s) = Q''(s) / T^2.

struct EndpointSamples {
  // Each is true when the corresponding derivative vector is present at
  // BOTH endpoints AND has the same size as the position vector at both.
  bool both_have_velocity{false};
  bool both_have_acceleration{false};

  // Fallback that the validation produced, if any. Reported even when the
  // sampler then degenerates to a still-lower degree (e.g. acceleration
  // mismatch downgrades to cubic, which itself may downgrade further if
  // velocity mismatch is also present in the same segment).
  TrajectorySampleFallback velocity_reason{TrajectorySampleFallback::kNone};
  TrajectorySampleFallback acceleration_reason{TrajectorySampleFallback::kNone};
};

EndpointSamples classify_endpoints(const TrajectoryWaypointDTO& a,
                                   const TrajectoryWaypointDTO& b,
                                   std::size_t joint_count) {
  EndpointSamples out;

  const bool a_has_v = !a.velocities_rad_s.empty();
  const bool b_has_v = !b.velocities_rad_s.empty();
  if (a_has_v && b_has_v) {
    if (a.velocities_rad_s.size() == joint_count &&
        b.velocities_rad_s.size() == joint_count) {
      out.both_have_velocity = true;
    } else {
      out.velocity_reason = TrajectorySampleFallback::kJointCountMismatchVelocity;
    }
  } else if (a_has_v != b_has_v) {
    out.velocity_reason = TrajectorySampleFallback::kIncompleteVelocityEndpoint;
  }

  const bool a_has_a = !a.accelerations_rad_s2.empty();
  const bool b_has_a = !b.accelerations_rad_s2.empty();

  if (out.both_have_velocity) {
    // Velocity is usable; acceleration only matters when present at
    // both endpoints with matching joint counts.
    if (a_has_a && b_has_a) {
      if (a.accelerations_rad_s2.size() == joint_count &&
          b.accelerations_rad_s2.size() == joint_count) {
        out.both_have_acceleration = true;
      } else {
        out.acceleration_reason =
            TrajectorySampleFallback::kJointCountMismatchAcceleration;
      }
    } else if (a_has_a != b_has_a) {
      out.acceleration_reason =
          TrajectorySampleFallback::kIncompleteAccelerationEndpoint;
    }
  } else if (a_has_a || b_has_a) {
    // Acceleration data is present but velocity is missing or
    // unusable. The quintic basis is unreachable and the sample will
    // fall back to linear. Surface velocity as the gating absence
    // so trace consumers see that derivative data was supplied but
    // could not be honored — silently reporting kNone here would hide
    // a meaningful diagnostic.
    if (out.velocity_reason == TrajectorySampleFallback::kNone) {
      out.velocity_reason =
          TrajectorySampleFallback::kIncompleteVelocityEndpoint;
    }
  }

  return out;
}

// One joint's value and first three time derivatives on one segment. The
// third derivative is the segment polynomial's own; across knots it is only
// as continuous as the interpolation (degree 1 and 3 are not C2).
struct JointDerivatives {
  double position;
  double velocity;
  double acceleration;
  double jerk;
};

// Basis values shared by every joint of one segment sample.
struct SegmentBasis {
  double s;
  double T;
  double inv_T;
  double inv_T2;
  double inv_T3;
  // Cubic: H10, H01, H11 and their first two derivatives (index = order).
  std::array<double, 3> H10{};
  std::array<double, 3> H01{};
  std::array<double, 3> H11{};
  // Quintic: N1..N5 (index 0 unused) and their first three derivatives.
  std::array<std::array<double, 4>, 6> N{};
};

SegmentBasis make_basis(double s, double T, std::uint8_t degree) noexcept {
  SegmentBasis basis{};
  basis.s = s;
  basis.T = T;
  basis.inv_T = 1.0 / T;
  basis.inv_T2 = basis.inv_T * basis.inv_T;
  basis.inv_T3 = basis.inv_T2 * basis.inv_T;
  const double s2 = s * s;
  const double s3 = s2 * s;
  if (degree == 3) {
    basis.H10 = {s - 2.0 * s2 + s3, 1.0 - 4.0 * s + 3.0 * s2, -4.0 + 6.0 * s};
    basis.H01 = {3.0 * s2 - 2.0 * s3, 6.0 * s - 6.0 * s2, 6.0 - 12.0 * s};
    basis.H11 = {-s2 + s3, -2.0 * s + 3.0 * s2, -2.0 + 6.0 * s};
  } else if (degree == 5) {
    const double s4 = s3 * s;
    const double s5 = s4 * s;
    basis.N[1] = {s - 6.0 * s3 + 8.0 * s4 - 3.0 * s5, 1.0 - 18.0 * s2 + 32.0 * s3 - 15.0 * s4,
                  -36.0 * s + 96.0 * s2 - 60.0 * s3, -36.0 + 192.0 * s - 180.0 * s2};
    basis.N[2] = {0.5 * s2 - 1.5 * s3 + 1.5 * s4 - 0.5 * s5, s - 4.5 * s2 + 6.0 * s3 - 2.5 * s4,
                  1.0 - 9.0 * s + 18.0 * s2 - 10.0 * s3, -9.0 + 36.0 * s - 30.0 * s2};
    basis.N[3] = {10.0 * s3 - 15.0 * s4 + 6.0 * s5, 30.0 * s2 - 60.0 * s3 + 30.0 * s4,
                  60.0 * s - 180.0 * s2 + 120.0 * s3, 60.0 - 360.0 * s + 360.0 * s2};
    basis.N[4] = {-4.0 * s3 + 7.0 * s4 - 3.0 * s5, -12.0 * s2 + 28.0 * s3 - 15.0 * s4,
                  -24.0 * s + 84.0 * s2 - 60.0 * s3, -24.0 + 168.0 * s - 180.0 * s2};
    basis.N[5] = {0.5 * s3 - s4 + 0.5 * s5, 1.5 * s2 - 4.0 * s3 + 2.5 * s4,
                  3.0 * s - 12.0 * s2 + 10.0 * s3, 3.0 - 24.0 * s + 30.0 * s2};
  }
  return basis;
}

JointDerivatives linear_joint(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b,
                              const SegmentBasis& basis, std::size_t j) noexcept {
  const double pa = a.positions_rad[j];
  const double pb = b.positions_rad[j];
  return {(1.0 - basis.s) * pa + basis.s * pb, (pb - pa) * basis.inv_T, 0.0, 0.0};
}

JointDerivatives cubic_joint(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b,
                             const SegmentBasis& basis, std::size_t j) noexcept {
  const double s = basis.s;
  const double T = basis.T;
  const double pa = a.positions_rad[j];
  const double pb = b.positions_rad[j];
  const double delta = pb - pa;
  const double va = a.velocities_rad_s[j];
  const double vb = b.velocities_rad_s[j];
  const double Tva = T * va;
  const double Tvb = T * vb;
  if (pa == pb && va == 0.0 && vb == 0.0) return {pa, 0.0, 0.0, 0.0};
  // H00 + H01 = 1, while their derivatives sum to zero. Work in
  // displacement coordinates so a joint origin cannot pollute derivatives.
  const auto& H10 = basis.H10;
  const auto& H01 = basis.H01;
  const auto& H11 = basis.H11;
  return {s == 0.0   ? pa
          : s == 1.0 ? pb
                     : pa + (Tva * H10[0] + delta * H01[0] + Tvb * H11[0]),
          s == 0.0   ? va
          : s == 1.0 ? vb
                     : (Tva * H10[1] + delta * H01[1] + Tvb * H11[1]) * basis.inv_T,
          (Tva * H10[2] + delta * H01[2] + Tvb * H11[2]) * basis.inv_T2,
          // Constant third derivatives: H10 -> 6, H01 -> -12, H11 -> 6.
          (6.0 * Tva - 12.0 * delta + 6.0 * Tvb) * basis.inv_T3};
}

JointDerivatives quintic_joint(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b,
                               const SegmentBasis& basis, std::size_t j) noexcept {
  const double s = basis.s;
  const double T = basis.T;
  const double T2 = T * T;
  const double pa = a.positions_rad[j];
  const double pb = b.positions_rad[j];
  const double delta = pb - pa;
  const double va = a.velocities_rad_s[j];
  const double vb = b.velocities_rad_s[j];
  const double aa = a.accelerations_rad_s2[j];
  const double ab = b.accelerations_rad_s2[j];
  const double Tva = T * va;
  const double Tvb = T * vb;
  const double T2aa = T2 * aa;
  const double T2ab = T2 * ab;
  if (pa == pb && va == 0.0 && vb == 0.0 && aa == 0.0 && ab == 0.0) return {pa, 0.0, 0.0, 0.0};
  const auto& N = basis.N;
  const auto combine = [&](std::size_t order) {
    return Tva * N[1][order] + T2aa * N[2][order] + delta * N[3][order] + Tvb * N[4][order] +
           T2ab * N[5][order];
  };
  const double jerk = combine(3) * basis.inv_T3;
  // Endpoint P/V/A is authoritative even when the displacement of extreme
  // finite endpoints is not representable (avoid infinity multiplied by zero).
  if (s == 0.0 || s == 1.0) {
    const auto& point = s == 0.0 ? a : b;
    return {point.positions_rad[j], point.velocities_rad_s[j], point.accelerations_rad_s2[j],
            jerk};
  }
  // N0 + N3 = 1; differentiate only the local displacement.
  return {pa + combine(0), combine(1) * basis.inv_T, combine(2) * basis.inv_T2, jerk};
}

void emit_linear(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b,
                 double s, double T, std::size_t joint_count,
                 TrajectorySampleResult& out) {
  out.positions_rad.resize(joint_count);
  out.velocities_rad_s.assign(joint_count, 0.0);
  out.accelerations_rad_s2.assign(joint_count, 0.0);
  const SegmentBasis basis = make_basis(s, T, 1);
  for (std::size_t j = 0; j < joint_count; ++j) {
    const auto value = linear_joint(a, b, basis, j);
    out.positions_rad[j] = value.position;
    out.velocities_rad_s[j] = value.velocity;
  }
  out.degree = 1;
}

void emit_cubic(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b,
                double s, double T, std::size_t joint_count,
                TrajectorySampleResult& out) {
  out.positions_rad.resize(joint_count);
  out.velocities_rad_s.resize(joint_count);
  out.accelerations_rad_s2.resize(joint_count);
  const SegmentBasis basis = make_basis(s, T, 3);
  for (std::size_t j = 0; j < joint_count; ++j) {
    const auto value = cubic_joint(a, b, basis, j);
    out.positions_rad[j] = value.position;
    out.velocities_rad_s[j] = value.velocity;
    out.accelerations_rad_s2[j] = value.acceleration;
  }
  out.degree = 3;
}

void emit_quintic(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b, double s,
                  double T, std::size_t joint_count, TrajectorySampleResult& out) {
  // Endpoint P/V/A is authoritative: copy the stored vectors unchanged.
  if (s == 0.0 || s == 1.0) {
    const auto& point = s == 0.0 ? a : b;
    out.positions_rad = point.positions_rad;
    out.velocities_rad_s = point.velocities_rad_s;
    out.accelerations_rad_s2 = point.accelerations_rad_s2;
    out.degree = 5;
    return;
  }
  out.positions_rad.resize(joint_count);
  out.velocities_rad_s.resize(joint_count);
  out.accelerations_rad_s2.resize(joint_count);
  const SegmentBasis basis = make_basis(s, T, 5);
  for (std::size_t j = 0; j < joint_count; ++j) {
    const auto value = quintic_joint(a, b, basis, j);
    out.positions_rad[j] = value.position;
    out.velocities_rad_s[j] = value.velocity;
    out.accelerations_rad_s2[j] = value.acceleration;
  }
  out.degree = 5;
}

Result<TrajectorySampleResult> make_error(TrajectorySampleError code) {
  ErrorDTO dto;
  dto.code = std::string(to_string(code));
  return Result<TrajectorySampleResult>::Err(std::move(dto));
}

Result<void> make_void_error(TrajectorySampleError code) {
  ErrorDTO dto;
  dto.code = std::string(to_string(code));
  return Result<void>::Err(std::move(dto));
}

void sample_prevalidated_segment(const TrajectoryDTO& trajectory, double time_s,
                                 std::size_t segment_index,
                                 TrajectorySampleResult& out) {
  const auto& a = trajectory.waypoints[segment_index];
  const auto& b = trajectory.waypoints[segment_index + 1];
  const double T = b.time_from_start_s - a.time_from_start_s;
  const double s = (time_s - a.time_from_start_s) / T;
  const std::size_t joint_count = trajectory.joint_names.size();
  const EndpointSamples classification =
      classify_endpoints(a, b, joint_count);

  out.segment_index = segment_index;
  out.clamped = false;

  if (classification.both_have_velocity &&
      classification.both_have_acceleration) {
    emit_quintic(a, b, s, T, joint_count, out);
    out.fallback_reason = TrajectorySampleFallback::kNone;
  } else if (classification.both_have_velocity) {
    emit_cubic(a, b, s, T, joint_count, out);
    out.fallback_reason = classification.acceleration_reason;
  } else {
    emit_linear(a, b, s, T, joint_count, out);
    out.fallback_reason =
        classification.velocity_reason != TrajectorySampleFallback::kNone
            ? classification.velocity_reason
            : classification.acceleration_reason;
  }
}

// Fill `out` with a clamped hold at `positions` (or an empty clamp when
// `positions == nullptr`). Every field is written so `out` can be reused
// across calls without leaking prior state.
void fill_clamped(const std::vector<double>* positions,
                  std::size_t joint_count, TrajectorySampleResult& out) {
  if (positions != nullptr) {
    out.positions_rad = *positions;
    out.velocities_rad_s.assign(joint_count, 0.0);
    out.accelerations_rad_s2.assign(joint_count, 0.0);
  } else {
    out.positions_rad.clear();
    out.velocities_rad_s.clear();
    out.accelerations_rad_s2.clear();
  }
  out.segment_index = std::nullopt;
  out.degree = 0;
  out.clamped = true;
  out.fallback_reason = TrajectorySampleFallback::kNone;
}

}  // namespace

Result<void> validate_trajectory_for_sampling(const TrajectoryDTO& trajectory) {
  // Empty trajectory: sampling yields the clamped empty result, so there
  // is nothing to validate (matching sample_trajectory's precedence, where
  // the empty check runs before the joint_names check).
  if (trajectory.waypoints.empty()) {
    return Result<void>::Ok();
  }

  // joint_names declares the per-joint column count. Required for any
  // non-empty trajectory because every consumer downstream of the
  // sampler keys on the joint set.
  if (trajectory.joint_names.empty()) {
    return make_void_error(TrajectorySampleError::kEmptyJointNames);
  }

  const std::size_t joint_count = trajectory.joint_names.size();

  // Validate every waypoint's position vector size and time field up
  // front. Catching this at the top means downstream code can assume
  // uniform widths and finite times.
  for (const auto& wp : trajectory.waypoints) {
    if (wp.positions_rad.size() != joint_count) {
      return make_void_error(TrajectorySampleError::kPositionJointCountMismatch);
    }
    if (!std::isfinite(wp.time_from_start_s)) {
      return make_void_error(TrajectorySampleError::kNonFiniteWaypointTime);
    }
  }

  // Validate monotonic, non-duplicate waypoint times. Times are
  // already known to be finite from the loop above.
  for (std::size_t i = 1; i < trajectory.waypoints.size(); ++i) {
    const double t_prev = trajectory.waypoints[i - 1].time_from_start_s;
    const double t_curr = trajectory.waypoints[i].time_from_start_s;
    if (t_curr < t_prev) {
      return make_void_error(TrajectorySampleError::kNonMonotonicWaypointTimes);
    }
    if (t_curr == t_prev) {
      return make_void_error(TrajectorySampleError::kDuplicateWaypointTimes);
    }
  }
  return Result<void>::Ok();
}

Result<void> sample_trajectory_prevalidated(const TrajectoryDTO& trajectory,
                                            double time_s,
                                            TrajectorySampleResult& out) {
  // Reject non-finite sample times before any comparison runs. NaN
  // compares false against everything (including the clamp predicates
  // below), which would otherwise let the call fall through to
  // out-of-range segment indexing.
  if (!std::isfinite(time_s)) {
    return make_void_error(TrajectorySampleError::kNonFiniteSampleTime);
  }

  // Empty trajectory: produce a clamped empty result.
  if (trajectory.waypoints.empty()) {
    fill_clamped(nullptr, 0, out);
    return Result<void>::Ok();
  }

  const std::size_t joint_count = trajectory.joint_names.size();

  // Single-waypoint trajectories hold that waypoint's position with zero
  // velocity / acceleration at all times.
  if (trajectory.waypoints.size() == 1) {
    fill_clamped(&trajectory.waypoints.front().positions_rad, joint_count, out);
    return Result<void>::Ok();
  }

  const double t_first = trajectory.waypoints.front().time_from_start_s;
  const double t_last = trajectory.waypoints.back().time_from_start_s;

  // Pre-start clamp.
  if (time_s < t_first) {
    fill_clamped(&trajectory.waypoints.front().positions_rad, joint_count, out);
    return Result<void>::Ok();
  }

  // Post-end clamp.
  if (time_s > t_last) {
    fill_clamped(&trajectory.waypoints.back().positions_rad, joint_count, out);
    return Result<void>::Ok();
  }

  // We are inside [t_first, t_last]. Pick a segment index using the
  // right-continuous rule: segment i covers [t_i, t_{i+1}). An exact
  // internal waypoint time t_k for 0 < k < N is sampled as the start of
  // segment k. The final endpoint t_N samples segment N-1 at alpha = 1.
  const std::size_t n_segments = trajectory.waypoints.size() - 1;
  std::size_t seg_idx = 0;
  if (time_s >= t_last) {
    // Exact final endpoint (post-end clamp handled above): sample the
    // final segment at alpha = 1.
    seg_idx = n_segments - 1;
  } else {
    // Find the first waypoint whose time is strictly greater than
    // time_s. That waypoint terminates the segment containing time_s.
    auto it = std::upper_bound(
        trajectory.waypoints.begin(), trajectory.waypoints.end(), time_s,
        [](double t, const TrajectoryWaypointDTO& wp) {
          return t < wp.time_from_start_s;
        });
    if (it == trajectory.waypoints.begin()) {
      // time_s == t_first; sample segment 0.
      seg_idx = 0;
    } else {
      const std::size_t upper_idx = static_cast<std::size_t>(
          std::distance(trajectory.waypoints.begin(), it));
      seg_idx = upper_idx - 1;
    }
  }

  // Segment duration is positive because structural validation rejected
  // duplicate and regressing waypoint times.
  sample_prevalidated_segment(trajectory, time_s, seg_idx, out);

  return Result<void>::Ok();
}

Result<void> sample_trajectory_prevalidated(const TrajectoryDTO& trajectory,
                                            double time_s,
                                            std::size_t segment_index,
                                            TrajectorySampleResult& out) {
  if (!std::isfinite(time_s)) {
    return make_void_error(TrajectorySampleError::kNonFiniteSampleTime);
  }
  if (trajectory.waypoints.size() < 2 ||
      segment_index >= trajectory.waypoints.size() - 1) {
    return make_void_error(TrajectorySampleError::kSegmentHintMismatch);
  }
  const auto& a = trajectory.waypoints[segment_index];
  const auto& b = trajectory.waypoints[segment_index + 1];
  const bool final_endpoint =
      segment_index + 1 == trajectory.waypoints.size() - 1 &&
      time_s == b.time_from_start_s;
  if (time_s < a.time_from_start_s ||
      (time_s >= b.time_from_start_s && !final_endpoint)) {
    return make_void_error(TrajectorySampleError::kSegmentHintMismatch);
  }
  sample_prevalidated_segment(trajectory, time_s, segment_index, out);
  return Result<void>::Ok();
}

Result<TrajectorySampleResult> sample_trajectory(
    const TrajectoryDTO& trajectory, double time_s) {
  // Time-first precedence: a non-finite time_s is rejected before any
  // structural validation, matching the original single-call contract.
  if (!std::isfinite(time_s)) {
    return make_error(TrajectorySampleError::kNonFiniteSampleTime);
  }
  auto valid = validate_trajectory_for_sampling(trajectory);
  if (!valid.ok()) {
    return Result<TrajectorySampleResult>::Err(valid.error());
  }
  TrajectorySampleResult out;
  auto sampled = sample_trajectory_prevalidated(trajectory, time_s, out);
  if (!sampled.ok()) {
    return Result<TrajectorySampleResult>::Err(sampled.error());
  }
  return Result<TrajectorySampleResult>::Ok(std::move(out));
}

bool evaluate_trajectory_segment(const TrajectoryDTO& trajectory, std::size_t segment_index,
                                 double time_s, std::uint8_t degree,
                                 std::span<double> position, std::span<double> velocity,
                                 std::span<double> acceleration,
                                 std::span<double> jerk) noexcept {
  if (!std::isfinite(time_s) || (degree != 1 && degree != 3 && degree != 5) ||
      trajectory.waypoints.size() < 2 || segment_index >= trajectory.waypoints.size() - 1) {
    return false;
  }
  const auto& a = trajectory.waypoints[segment_index];
  const auto& b = trajectory.waypoints[segment_index + 1];
  const bool final_endpoint = segment_index + 2 == trajectory.waypoints.size() &&
                              time_s == b.time_from_start_s;
  const double T = b.time_from_start_s - a.time_from_start_s;
  const std::size_t joint_count = position.size();
  if (!std::isfinite(T) || !(T > 0.0) || time_s < a.time_from_start_s ||
      (time_s >= b.time_from_start_s && !final_endpoint) || velocity.size() != joint_count ||
      acceleration.size() != joint_count || jerk.size() != joint_count ||
      a.positions_rad.size() != joint_count || b.positions_rad.size() != joint_count ||
      (degree >= 3 && (a.velocities_rad_s.size() != joint_count ||
                       b.velocities_rad_s.size() != joint_count)) ||
      (degree == 5 && (a.accelerations_rad_s2.size() != joint_count ||
                       b.accelerations_rad_s2.size() != joint_count))) {
    return false;
  }
  const SegmentBasis basis = make_basis((time_s - a.time_from_start_s) / T, T, degree);
  for (std::size_t j = 0; j < joint_count; ++j) {
    const auto value = degree == 5   ? quintic_joint(a, b, basis, j)
                       : degree == 3 ? cubic_joint(a, b, basis, j)
                                     : linear_joint(a, b, basis, j);
    if (!std::isfinite(value.position) || !std::isfinite(value.velocity) ||
        !std::isfinite(value.acceleration) || !std::isfinite(value.jerk)) {
      return false;
    }
    position[j] = value.position;
    velocity[j] = value.velocity;
    acceleration[j] = value.acceleration;
    jerk[j] = value.jerk;
  }
  return true;
}

}  // namespace manyforge::core
