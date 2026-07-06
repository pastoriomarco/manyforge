#include <manyforge_core/trajectory/trajectory_sampler.hpp>

#include <algorithm>
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

void emit_linear(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b,
                 double s, double T, std::size_t joint_count,
                 TrajectorySampleResult& out) {
  out.positions_rad.resize(joint_count);
  out.velocities_rad_s.assign(joint_count, 0.0);
  out.accelerations_rad_s2.assign(joint_count, 0.0);
  const double inv_T = 1.0 / T;
  for (std::size_t j = 0; j < joint_count; ++j) {
    const double pa = a.positions_rad[j];
    const double pb = b.positions_rad[j];
    out.positions_rad[j] = (1.0 - s) * pa + s * pb;
    out.velocities_rad_s[j] = (pb - pa) * inv_T;
  }
  out.degree = 1;
}

void emit_cubic(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b,
                double s, double T, std::size_t joint_count,
                TrajectorySampleResult& out) {
  out.positions_rad.resize(joint_count);
  out.velocities_rad_s.resize(joint_count);
  out.accelerations_rad_s2.resize(joint_count);

  const double s2 = s * s;
  const double s3 = s2 * s;

  const double H00 = 1.0 - 3.0 * s2 + 2.0 * s3;
  const double H10 = s - 2.0 * s2 + s3;
  const double H01 = 3.0 * s2 - 2.0 * s3;
  const double H11 = -s2 + s3;

  const double dH00 = -6.0 * s + 6.0 * s2;
  const double dH10 = 1.0 - 4.0 * s + 3.0 * s2;
  const double dH01 = 6.0 * s - 6.0 * s2;
  const double dH11 = -2.0 * s + 3.0 * s2;

  const double ddH00 = -6.0 + 12.0 * s;
  const double ddH10 = -4.0 + 6.0 * s;
  const double ddH01 = 6.0 - 12.0 * s;
  const double ddH11 = -2.0 + 6.0 * s;

  const double inv_T = 1.0 / T;
  const double inv_T2 = inv_T * inv_T;

  for (std::size_t j = 0; j < joint_count; ++j) {
    const double pa = a.positions_rad[j];
    const double pb = b.positions_rad[j];
    const double va = a.velocities_rad_s[j];
    const double vb = b.velocities_rad_s[j];
    const double Tva = T * va;
    const double Tvb = T * vb;

    out.positions_rad[j] = pa * H00 + Tva * H10 + pb * H01 + Tvb * H11;
    out.velocities_rad_s[j] =
        (pa * dH00 + Tva * dH10 + pb * dH01 + Tvb * dH11) * inv_T;
    out.accelerations_rad_s2[j] =
        (pa * ddH00 + Tva * ddH10 + pb * ddH01 + Tvb * ddH11) * inv_T2;
  }
  out.degree = 3;
}

void emit_quintic(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b,
                  double s, double T, std::size_t joint_count,
                  TrajectorySampleResult& out) {
  out.positions_rad.resize(joint_count);
  out.velocities_rad_s.resize(joint_count);
  out.accelerations_rad_s2.resize(joint_count);

  const double s2 = s * s;
  const double s3 = s2 * s;
  const double s4 = s3 * s;
  const double s5 = s4 * s;

  const double N0 = 1.0 - 10.0 * s3 + 15.0 * s4 - 6.0 * s5;
  const double N1 = s - 6.0 * s3 + 8.0 * s4 - 3.0 * s5;
  const double N2 = 0.5 * s2 - 1.5 * s3 + 1.5 * s4 - 0.5 * s5;
  const double N3 = 10.0 * s3 - 15.0 * s4 + 6.0 * s5;
  const double N4 = -4.0 * s3 + 7.0 * s4 - 3.0 * s5;
  const double N5 = 0.5 * s3 - s4 + 0.5 * s5;

  const double dN0 = -30.0 * s2 + 60.0 * s3 - 30.0 * s4;
  const double dN1 = 1.0 - 18.0 * s2 + 32.0 * s3 - 15.0 * s4;
  const double dN2 = s - 4.5 * s2 + 6.0 * s3 - 2.5 * s4;
  const double dN3 = 30.0 * s2 - 60.0 * s3 + 30.0 * s4;
  const double dN4 = -12.0 * s2 + 28.0 * s3 - 15.0 * s4;
  const double dN5 = 1.5 * s2 - 4.0 * s3 + 2.5 * s4;

  const double ddN0 = -60.0 * s + 180.0 * s2 - 120.0 * s3;
  const double ddN1 = -36.0 * s + 96.0 * s2 - 60.0 * s3;
  const double ddN2 = 1.0 - 9.0 * s + 18.0 * s2 - 10.0 * s3;
  const double ddN3 = 60.0 * s - 180.0 * s2 + 120.0 * s3;
  const double ddN4 = -24.0 * s + 84.0 * s2 - 60.0 * s3;
  const double ddN5 = 3.0 * s - 12.0 * s2 + 10.0 * s3;

  const double inv_T = 1.0 / T;
  const double inv_T2 = inv_T * inv_T;
  const double T2 = T * T;

  for (std::size_t j = 0; j < joint_count; ++j) {
    const double pa = a.positions_rad[j];
    const double pb = b.positions_rad[j];
    const double va = a.velocities_rad_s[j];
    const double vb = b.velocities_rad_s[j];
    const double aa = a.accelerations_rad_s2[j];
    const double ab = b.accelerations_rad_s2[j];
    const double Tva = T * va;
    const double Tvb = T * vb;
    const double T2aa = T2 * aa;
    const double T2ab = T2 * ab;

    out.positions_rad[j] = pa * N0 + Tva * N1 + T2aa * N2 +
                           pb * N3 + Tvb * N4 + T2ab * N5;
    out.velocities_rad_s[j] =
        (pa * dN0 + Tva * dN1 + T2aa * dN2 +
         pb * dN3 + Tvb * dN4 + T2ab * dN5) * inv_T;
    out.accelerations_rad_s2[j] =
        (pa * ddN0 + Tva * ddN1 + T2aa * ddN2 +
         pb * ddN3 + Tvb * ddN4 + T2ab * ddN5) * inv_T2;
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

  const auto& a = trajectory.waypoints[seg_idx];
  const auto& b = trajectory.waypoints[seg_idx + 1];
  const double T = b.time_from_start_s - a.time_from_start_s;
  // T > 0 here: duplicate times were rejected earlier, and non-monotonic
  // times were rejected too.
  const double s = (time_s - a.time_from_start_s) / T;

  EndpointSamples classification = classify_endpoints(a, b, joint_count);

  out.segment_index = seg_idx;
  out.clamped = false;

  if (classification.both_have_velocity &&
      classification.both_have_acceleration) {
    emit_quintic(a, b, s, T, joint_count, out);
    out.fallback_reason = TrajectorySampleFallback::kNone;
  } else if (classification.both_have_velocity) {
    emit_cubic(a, b, s, T, joint_count, out);
    // If the natural target was quintic but we degraded because of
    // acceleration data, report the acceleration-side reason; otherwise
    // this is the natural cubic path.
    out.fallback_reason = classification.acceleration_reason;
  } else {
    emit_linear(a, b, s, T, joint_count, out);
    // Report a velocity-side reason if velocity data was present but
    // unusable; otherwise leave it as kNone (natural linear path). If
    // acceleration data was supplied without velocity, the acceleration
    // reason still applies because we couldn't use it.
    if (classification.velocity_reason !=
        TrajectorySampleFallback::kNone) {
      out.fallback_reason = classification.velocity_reason;
    } else {
      out.fallback_reason = classification.acceleration_reason;
    }
  }

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

}  // namespace manyforge::core
