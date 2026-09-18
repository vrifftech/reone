/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace reone {
namespace game {
namespace shape {

// A short vector becomes +X, not zero or NaN. The threshold is a double.
inline glm::vec3 normalize(const glm::vec3 &vector) {
    float length = std::sqrt(glm::dot(vector, vector));
    if (static_cast<double>(length) < 1.0e-9) {
        return glm::vec3(1.0f, 0.0f, 0.0f);
    }
    return vector * (1.0f / length);
}

// Projection returns an absolute point, including for a zero axis.
inline glm::vec3 project(const glm::vec3 &origin,
                        const glm::vec3 &target,
                        const glm::vec3 &position) {
    glm::vec3 axis = target - origin;
    float length2 = glm::dot(axis, axis);
    float fraction = length2 == 0.0f
                         ? 0.0f
                         : glm::dot(axis, position - origin) / length2;
    return origin + axis * fraction;
}

// World-X bounds belong to the live area scan. GetNext does not repeat
// GetFirst's lower-bound search, so the geometric predicates must not do so.
inline bool matchCone(const glm::vec3 &position,
                      const glm::vec3 &target,
                      const glm::vec3 &origin,
                      float size) {
    glm::vec3 axis = target - origin;
    if (!(glm::dot(normalize(axis), normalize(position - origin)) > 0.0f)) {
        return false;
    }
    glm::vec3 projected = project(origin, target, position);
    glm::vec3 perpendicular = position - projected;
    // K2 branch 0x1003140a0, also present in K1: this is deliberately the
    // magnitude of the *absolute* projected point, not projected-origin.
    float radius = (size / std::sqrt(glm::dot(axis, axis))) *
                   std::sqrt(glm::dot(projected, projected));
    return radius * radius >= glm::dot(perpendicular, perpendicular);
}

inline bool matchSpellCone(const glm::vec3 &position,
                           const glm::vec3 &target,
                           const glm::vec3 &origin,
                           float size) {
    // K2 branch 0x1003142ec: fixed cosine, then projected length, not a
    // spherical cap. Do not add an unconditional near-origin acceptance.
    if (!(glm::dot(normalize(target - origin),
                   normalize(position - origin)) >= 0.866f)) {
        return false;
    }
    glm::vec3 along = project(origin, target, position) - origin;
    return size * size >= glm::dot(along, along);
}

} // namespace shape
} // namespace game
} // namespace reone
