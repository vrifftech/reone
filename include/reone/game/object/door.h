/*
 * Copyright (c) 2020-2023 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <array>
#include <optional>

#include "reone/resource/format/gffreader.h"
#include "reone/scene/node/walkmesh.h"

#include "../object.h"

namespace reone {

namespace game {

class Creature;
class Trigger;
struct AttackEventFields;

class Door : public Object {
public:
    std::string getOnSpellCastAt() const override { return _onSpellCastAt; }
    Door(
        uint32_t id,
        std::string sceneName,
        Game &game,
        ServicesView &services) :
        Object(
            id,
            ObjectType::Door,
            std::move(sceneName),
            game,
            services) {
    }

    static bool classof(const Object *from) {
        return from->type() == ObjectType::Door;
    }

    void loadFromBlueprint(const std::string &resRef);
    void deserialize(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);

    bool isSelectable() const override;
    void damage(
        int amount,
        const std::shared_ptr<Object> &damager) override;
    void update(float dt) override;

    void open();
    void close();

    /**
     * Resting state the door last physically reached. A door in transition
     * still reports the state it is leaving, because that is where it still
     * stands, and its collision, pose and open flag all follow from it.
     */
    DoorState state() const { return _state; }

    /** Transition the door is playing, if any. */
    DoorTransition transition() const { return _transition; }

    /**
     * True while the door is swinging open. It has not reached its opened pose
     * yet, so the doorway is still blocking and isOpen is still false.
     */
    bool isOpening() const { return _transition == DoorTransition::Opening; }

    /**
     * True while the door is swinging shut. It has not reached its closed pose
     * yet, so the doorway is still passable and isOpen is still true.
     */
    bool isClosing() const { return _transition == DoorTransition::Closing; }

    // Destruction animation and pending deletion do not change the HP predicate.
    bool isDead() const override { return !_plot && currentHitPoints() <= 0; }
    bool isLocked() const { return _locked; }
    bool isStatic() const { return _static; }
    bool isKeyRequired() const { return _keyRequired; }
    bool isAutoRemoveKey() const { return _autoRemoveKey; }
    bool isNotBlastable() const { return _notBlastable; }

    const std::string &keyName() const { return _keyName; }
    uint8_t closeLockDC() const { return _closeLockDC; }

    void onOpen(uint32_t triggererId);
    /** Run the script for being locked. */
    void onLocked();
    /**
     * The door failed to open for the opener: its script for that runs, and a
     * door still locked tells the opener so, unless quiet or the door is to
     * open itself.
     */
    void onFailToOpen(uint32_t openerId, bool quiet);

    const std::string &getOnOpen() const { return _onOpen; }
    const std::string &getOnFailToOpen() const { return _onFailToOpen; }

    // Traps

    bool isTrapped() const { return _trapFlag != 0; }
    bool trapDisarmable() const { return _trapDisarmable; }
    bool trapDetectable() const { return _trapDetectable; }
    int ownerDemolitionsSkill() const { return _ownerDemolitionsSkill; }
    bool trapOneShot() const { return _trapOneShot; }
    uint8_t trapBaseType() const { return _trapType; }
    const std::string &trapKeyTag() const { return _keyName; }
    int trapDetectDC() const;
    int trapDisarmDC() const;
    TrapDetection &trapDetection() { return _trapDetection; }
    const TrapDetection &trapDetection() const { return _trapDetection; }
    /** Hostile when neither faction nor standing is shared with the creature. */
    bool isTrapHostileTo(const Creature &creature) const;
    /** An armed trap goes off on the caller; forced firing skips the standing test. */
    void triggerTrap(const std::shared_ptr<Creature> &caller, bool force);
    /** The trap is disarmed by the caller. */
    void disarmTrap(const Object &caller);
    /** Arm a set mine; the linked mine trigger marks the trap in the world (TSL). */
    void armMine(int trapType, int detectDC, int disarmDC, int ownerDemolitionsSkill,
                 const std::shared_ptr<Trigger> &linkedMine,
                 const std::shared_ptr<Creature> &setter, int blastBonus, bool blast);

    // END Traps

    int genericType() const { return _genericType; }
    /** The doortypes.2da row, the low byte of the appearance; 0 for a generic door. */
    uint8_t appearance() const { return _appearance; }
    Faction faction() const { return _faction; }
    /** The door's own saving throw from its template; effects never change it. */
    int savingThrow(SavingThrow save) const;
    void setFaction(Faction faction) { _faction = faction; }
    const std::string &linkedToModule() const { return _linkedToModule; }
    const std::string &linkedTo() const { return _linkedTo; }
    uint8_t linkedToFlags() const { return _linkedToFlags; }
    const std::string &transitionDestin() const { return _transitionDestin.str(); }
    const resource::LocString &transitionDestination() const { return _transitionDestin; }
    const std::vector<glm::vec3> &linkedTransitionGeometry() const { return _linkedTransitionGeometry; }

    void enterDestroyedState();
    void receiveDamagedSignal(const std::shared_ptr<Object> &damager);
    void receiveDeathSignal(uint32_t killerId);
    void receiveAttackEvent(uint32_t attackerId, const AttackEventFields *fields);

    void setLocked(bool locked);

    /** Whether a creature steps right up to the door's use point to use it while it is locked. */
    bool isPreciseUse() const { return _preciseUse; }
    /** Whether the door's look blocks sight (a see-through door does not block a sight line that may pass one). */
    bool blocksSight() const { return _blocksSight; }

    /**
     * The point a creature at from goes to in order to use the door: of the
     * two use points of the state the door stands in (or, with closed, of its
     * closed state), the nearer one over walkable ground, the farther one
     * only when the nearer one is the first; then, for a door not closed, a
     * point of the closed state that is nearer still and over walkable
     * ground. The world origin when no point qualifies.
     */
    glm::vec3 nearestActionPoint(const glm::vec3 &from, bool closed) const;

    // Walkmeshes

    std::shared_ptr<scene::WalkmeshSceneNode> walkmeshOpen1() const { return _walkmeshOpen1; }
    std::shared_ptr<scene::WalkmeshSceneNode> walkmeshOpen2() const { return _walkmeshOpen2; }
    std::shared_ptr<scene::WalkmeshSceneNode> walkmeshClosed() const { return _walkmeshClosed; }

    // END Walkmeshes

    void applyDamageEffect(
        int amount,
        const std::shared_ptr<Object> &damager,
        std::optional<DamageReaction> reaction) override;

private:
    void removeLinkedMine();
    void updateMineBlast(float dt);
    void openAfterBlast();
    friend class ModuleSnapshotBuilder;
    friend class TestGameModule;
    // Serializable
    resource::LocString _locName;
    uint8_t _appearance {0};
    uint8_t _genericType {0};
    DoorState _state {DoorState::Closed};
    bool _autoRemoveKey {false};
    Faction _faction {Faction::Invalid};
    uint8_t _fort {0};
    uint8_t _will {0};
    uint8_t _ref {0};
    std::string _keyName;
    bool _keyRequired {false};
    uint8_t _openLockDC {0};
    uint8_t _closeLockDC {0};
    uint8_t _secretDoorDC {0};
    uint16_t _portraitId {0};
    uint8_t _hardness {0};

    std::string _onClosed;
    std::string _onDamaged;
    std::string _onDeath;
    std::string _onDisarm;
    std::string _onLock;
    std::string _onMeleeAttacked;
    std::string _onOpen;
    std::string _onSpellCastAt;
    std::string _onTrapTriggered;
    std::string _onUnlock;
    std::string _onClick;
    std::string _onFailToOpen;
    std::string _onDialog;

    uint8_t _trapType {0};
    bool _trapDisarmable {true};
    bool _trapDetectable {true};
    uint8_t _disarmDC {0};
    uint8_t _trapDetectDC {0};
    uint8_t _trapFlag {0};
    bool _trapOneShot {true};
    TrapDetection _trapDetection;
    RuntimeObjectRef<Trigger> _linkedMine;
    int _ownerDemolitionsSkill {0};
    // A mine set on a locked object tries to blow it open (TSL).
    RuntimeObjectRef<Creature> _mineSetter;
    int _mineBlastBonus {0};
    float _mineBlastDelay {-1.0f};
    float _mineOpenDelay {-1.0f};
    bool _locked {false};
    bool _lockable {false};
    uint8_t _linkedToFlags {0};
    std::string _linkedTo;
    std::string _linkedToModule;
    uint16_t _loadScreenId {0};
    resource::LocString _description;
    bool _static {false};
    bool _notBlastable {false};
    resource::LocString _transitionDestin;
    // END Serializable

    bool _preciseUse {false};
    bool _blocksSight {true};

    // Walkmeshes

    std::shared_ptr<scene::WalkmeshSceneNode> _walkmeshOpen1;
    std::shared_ptr<scene::WalkmeshSceneNode> _walkmeshOpen2;
    std::shared_ptr<scene::WalkmeshSceneNode> _walkmeshClosed;
    std::vector<glm::vec3> _linkedTransitionGeometry;

    // END Walkmeshes

    // Scripts

    // END Scripts

    /**
     * The transition in flight. It names an animation to wait on and nothing
     * else: collision, pose and the open flag are read off _state alone, and
     * _state only moves when a transition arrives. A reversal overwrites this,
     * which is what stops the superseded transition from ever completing.
     */
    DoorTransition _transition {DoorTransition::None};

    void runDamagedScript();
    void runDeathScript();

    void deserializeAll(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);
    void loadAppearance();

    /** Put model pose, walkmeshes and the open flag into correspondence with _state. */
    void applyRestingState();
    void applyRestingPose();
    void enableStateWalkmeshes();
    void beginTransition(DoorTransition transition);
    void finishTransition();
    bool isTransitionComplete() const;
    static const char *transitionAnimation(DoorTransition transition);
    static DoorState transitionTarget(DoorTransition transition);
    /** The state the door is in or, during a transition, is moving to. */
    DoorState actionState() const;
    /** The two use points of a resting state in the world; none for a destroyed door. */
    std::optional<std::array<glm::vec3, 2>> actionPoints(DoorState state) const;
    void loadLinkedTransitionGeometry(const graphics::Walkmesh &walkmesh);
    void updateTransform() override;
};

/**
 * Whether door can be bashed open. This is the single definition of door bash
 * eligibility, shared by the player context action and by the DOOR_ACTION_BASH
 * script routines. Standing toward the door plays no part.
 */
bool canBashDoor(const Door &door);

} // namespace game

} // namespace reone
