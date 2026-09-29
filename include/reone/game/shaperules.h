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

// World-X bounds belong to the area scan. Continued enumeration does not
// repeat the initial lower-bound search; geometric predicates must not repeat it either.
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
    // Use the magnitude of the absolute projected point, not its displacement from the
    // origin.
    float radius = (size / std::sqrt(glm::dot(axis, axis))) *
                   std::sqrt(glm::dot(projected, projected));
    return radius * radius >= glm::dot(perpendicular, perpendicular);
}

inline bool matchSpellCone(const glm::vec3 &position,
                           const glm::vec3 &target,
                           const glm::vec3 &origin,
                           float size) {
    // Check the fixed cosine before the projected length. This is not a spherical cap;
    // points near the origin do not bypass the checks.
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
