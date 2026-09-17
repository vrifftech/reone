/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <memory>
#include "types.h"
namespace reone::game {
class Game; class Creature; class Object; struct ServicesView; struct SavingThrowBreakdown;
void addSavingThrowFeedback(Game &, ServicesView &, const Creature &, SavingThrow,
                           const SavingThrowBreakdown &, int roll, int difficultyClass);
void addPoisonImmunityFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &, const Creature &);
void addSlowImmunityFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &, const Creature &);
void addEntangleImmunityFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &, const Creature &);
void addHitPointHealingFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &, const Creature &, int amount);
void addBlindnessImmunityFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &, const Creature &);
void addForceHealingFeedback(Game &, ServicesView &, const Creature &, int amount);
void addPoisonedFeedback(Game &, ServicesView &, Creature &, int poisonNameStrRef);
} // namespace reone::game
