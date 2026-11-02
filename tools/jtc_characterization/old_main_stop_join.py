"""Analytical one-joint reproduction of old-main Stop export/shift construction.

Source: ManyForge8ae05265 soft_on_path_stop_planner.cpp434–471,
1143–1169,1317–1348,1760–1867; ROS backend190–198,5995–6048,8705.
This fixture is not a replacement for the full Core planner or its limit checks.
"""
from __future__ import annotations

import math


def linear_stop(cursor: float, rate: float, lead: float, ramp: float,
                period: float = 0.01) -> list[dict]:
    points = []
    lead_count = max(1, math.ceil(lead / period))
    for index in range(lead_count):
        tau = lead * index / lead_count
        points.append(dict(t=tau, q=0.2 * (cursor + rate * tau), v=0.2 * rate, a=0.0))
    ramp_count = max(1, math.ceil(ramp / period))
    for index in range(ramp_count + 1):
        r = index / ramp_count
        position_factor = r - 2.5 * r**4 + 3 * r**5 - r**6
        velocity_factor = 1 - 10 * r**3 + 15 * r**4 - 6 * r**5
        acceleration_factor = -30 * r**2 + 60 * r**3 - 30 * r**4
        points.append(dict(t=lead + r * ramp,
                           q=0.2 * (cursor + rate * lead + rate * ramp * position_factor),
                           v=0.2 * rate * velocity_factor,
                           a=0.2 * rate / ramp * acceleration_factor))
    return points


def shift_latency(points: list[dict], latency: float, lead: float,
                  cursor: float, rate: float) -> list[dict]:
    if latency >= lead:
        raise ValueError('dispatch_latency_exceeds_lead_in')
    shifted = [dict(t=-0.010, q=0.2 * (cursor + rate * latency), v=0.2 * rate, a=0.0)]
    shifted.extend(dict(point, t=point['t'] - latency)
                   for point in points if point['t'] - latency >= 0.0)
    if len(shifted) == 1:
        raise ValueError('dispatch_latency_consumed_all_waypoints')
    return shifted


def wire_points(points: list[dict], *, old_main: bool, frozen_factor: float) -> list[dict]:
    if old_main:
        # Exact historical conversion clips the authored negative pin to0.
        return [dict(point, t=max(0.0, point['t'])) for point in points]
    # Proposed frozen-factor realization: physical seconds -> JTC path seconds;
    # derivatives -> path-time units. This is distinct from historical unit assertion.
    return [dict(t=point['t'] * frozen_factor, q=point['q'],
                 v=point['v'] / frozen_factor, a=point['a'] / frozen_factor**2)
            for point in points]


def sample_export(points: list[dict], t: float) -> tuple[float, float, float]:
    """PVA quintic interpolation of the exported points, excluding receiver-created joins."""
    if t >= points[-1]['t']:
        last = points[-1]
        return last['q'], last['v'], last['a']
    left, right = next((left, right) for left, right in zip(points, points[1:])
                       if left['t'] <= t < right['t'])
    dt, tau = right['t'] - left['t'], t - left['t']
    delta = right['q'] - (left['q'] + left['v'] * dt + left['a'] * dt**2 / 2)
    velocity = right['v'] - (left['v'] + left['a'] * dt)
    acceleration = right['a'] - left['a']
    c = [left['q'], left['v'], left['a'] / 2,
         (10 * delta - 4 * velocity * dt + acceleration * dt**2 / 2) / dt**3,
         (-15 * delta + 7 * velocity * dt - acceleration * dt**2) / dt**4,
         (6 * delta - 3 * velocity * dt + acceleration * dt**2 / 2) / dt**5]
    return (sum(value * tau**index for index, value in enumerate(c)),
            sum(index * c[index] * tau**(index - 1) for index in range(1, 6)),
            sum(index * (index - 1) * c[index] * tau**(index - 2) for index in range(2, 6)))
