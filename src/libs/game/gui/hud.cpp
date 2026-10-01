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

#include "reone/scene/render/pass/retro.h"
#include "reone/scene/render/pass/pbr.h"
#include "reone/game/gui/hud.h"

#include <algorithm>
#include <cstdint>
#include <variant>
#include <vector>

#include "reone/audio/mixer.h"
#include "reone/graphics/context.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/shaderregistry.h"
#include "reone/graphics/uniforms.h"
#include "reone/gui/guis.h"
#include "reone/gui/control/label.h"
#include "reone/resource/exception/notfound.h"
#include "reone/resource/provider/audioclips.h"
#include "reone/resource/strings.h"
#include "reone/system/logutil.h"

#include "reone/game/action/castspellatlocation.h"
#include "reone/game/action/castspellatobject.h"
#include "reone/game/action/usefeat.h"
#include "reone/game/d20/feat.h"
#include "reone/game/d20/feats.h"
#include "reone/game/d20/spell.h"
#include "reone/game/effect/visual.h"
#include "reone/game/gui/sounds.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/area.h"
#include "reone/game/object/camera.h"
#include "reone/game/object/item.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"

#include <cmath>
#include <optional>

using namespace reone::audio;

using namespace reone::graphics;
using namespace reone::gui;
using namespace reone::resource;

namespace reone {

namespace game {

// The scale grows in whole steps: a half for each full 1024 pixels of width
// beyond 1022, so it stays 1 up to 2045 pixels.
static float effectStackTextScalar(bool tsl, int screenWidth) {
    return tsl && screenWidth > 1022 ? 1.0f + ((screenWidth - 1022) / 1024) * 0.5f : 1.0f;
}
static int effectStackDrawCount(int count, bool leader) {
    if (leader) {
        return std::min(count, 9);
    }
    return count >= 6 ? 5 : count;
}
static int effectStackSpacing(int count, bool leader) {
    if (!leader) {
        return 5;
    }
    if (count <= 4) {
        return 10;
    }
    switch (count) {
    case 5:
        return 10;
    case 6:
        return 8;
    case 7:
        return 7;
    case 8:
        return 6;
    default:
        return 5;
    }
}
static std::vector<int> effectStackTopOffsets(
    int iconTop, int viewportTop, int viewportHeight,
    int count,
    bool leader,
    bool good, float textScalar = 1.0f) {

    count = effectStackDrawCount(count, leader);
    if (count <= 0) {
        return {};
    }

    int spacing = static_cast<uint8_t>(static_cast<int>(effectStackSpacing(count, leader) * textScalar));
    int top = iconTop;
    if (count <= 4) {
        int firstStep = spacing >> 1;
        int initialOffset = (count - 1) * firstStep;
        top += good ? -initialOffset : initialOffset;
    } else if (good) {
        top = viewportTop + 1;
    } else {
        int bottomMargin = leader ? 16 : 8;
        top = viewportTop + viewportHeight - bottomMargin - 1;
    }

    std::vector<int> result;
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        result.push_back(top);
        top += good ? spacing : -spacing;
        if ((count == 7 || count == 8) &&
            (index == 0 || index == 2)) {
            top += good ? -1 : 1;
        }
    }
    return result;
}

/** Pause indicator text for a pause reason; `press` shows the unpause prompt. */
struct PauseReasonText {
    int k1StrRef;
    int k2StrRef;
    bool press;
};

static std::optional<PauseReasonText> pauseReasonText(PauseReason reason) {
    switch (reason) {
    case PauseReason::Player: return PauseReasonText {1508, 1508, true};
    case PauseReason::EnemySighted: return PauseReasonText {48212, 48732, false};
    case PauseReason::EndOfCombatRound: return PauseReasonText {42432, 42432, true};
    case PauseReason::ActionMenu: return PauseReasonText {42482, 42482, true};
    case PauseReason::NewTargetSelected: return PauseReasonText {42481, 42481, true};
    case PauseReason::PartyKilled: return PauseReasonText {42397, 42397, true};
    case PauseReason::MineSighted: return PauseReasonText {49118, 48735, false};
    case PauseReason::CombatOrder: return PauseReasonText {48423, 48734, true};
    default: return std::nullopt;
    }
}

/** TSL's unpause prompt under the reason. */
static constexpr int kK2PausePressStrRef = 136311;

static void renderEffectStack(
    Label &label,
    const Control::Extent &viewportExtent,
    int count,
    bool leader,
    bool good,
    float textScalar,
    const glm::ivec2 &screenSize,
    const glm::ivec2 &controlOffset,
    scene::IRenderPass &pass,
    graphics::IContext &context) {

    auto topOffsets = effectStackTopOffsets(
        label.extent().top, viewportExtent.top, viewportExtent.height,
        count,
        leader,
        good, textScalar);
    if (topOffsets.empty()) {
        return;
    }

    glm::ivec4 scissorBounds(
        controlOffset.x + viewportExtent.left,
        screenSize.y - (controlOffset.y + viewportExtent.top + viewportExtent.height),
        viewportExtent.width,
        viewportExtent.height);

    bool wasVisible = label.isVisible();
    label.setVisible(true);
    context.withScissorTestNoClear(scissorBounds, [&]() {
        for (int top : topOffsets) {
            glm::ivec2 offset = controlOffset;
            offset.y += top - label.extent().top;
            label.render(screenSize, offset, pass);
        }
    });
    label.setVisible(wasVisible);
}

static std::string g_attackIcon("i_attack");

// A cast shows its spell's icon. The use of an item shows the item's icon
// while the item is still in the game, and nothing once it is gone.
static std::shared_ptr<graphics::Texture> castIcon(const Spell &spell, const std::optional<std::shared_ptr<Item>> &item) {
    if (!item) return spell.icon;
    return *item && (*item)->isRuntimeLive() ? (*item)->icon() : nullptr;
}

/** Authored-canvas gap between the minimap frame and the TSL bark bubble. */
static constexpr int kBarkBubbleMapGap = 8;

/** The combat line shown while the leader is held helpless. */
static constexpr int kLeaderDebilitatedCombatMessage = 47915;

static void tintHUDMenuButton(const std::shared_ptr<Button> &button, const glm::vec3 &baseColor) {
    if (!button) {
        return;
    }
    button->setBorderColor(baseColor);
    button->setTintBorderFill(true);
}

void HUD::preload(IGUI &gui) {
    gui.setResolution(800, 600);
    gui.setScaling(GUI::ScalingMode::PositionRelativeToCenter);

    static constexpr const char *kCentredCombatControls[] = {
        "BTN_CLEARALL", "BTN_CLEARONE", "BTN_CLEARONE2",
        "LBL_CMBTMODEMSG", "LBL_CMBTMSGBG", "LBL_COMBATBG1", "LBL_COMBATBG2", "LBL_COMBATBG3",
        "LBL_QUEUE0", "LBL_QUEUE1", "LBL_QUEUE2", "LBL_QUEUE3"};
    for (auto tag : kCentredCombatControls) {
        gui.setControlScaling(tag, GUI::ScalingMode::ScaledRelativeToCenter);
    }
}

void HUD::onGUILoaded() {
    bindControls();

    if (!_game.isTSL()) {
        // The GUI control tree outlives this wrapper in the GUI cache. Derive
        // from the live layout inputs so rebuilding a HUD cannot feed the
        // previous presentation scale back into itself.
        const float combatTextScale = _gui->scale() * _gui->textScale() * kK1CombatTextScale;
        for (Control *control : {
                 static_cast<Control *>(_controls.LBL_CMBTMODEMSG.get()),
                 static_cast<Control *>(_controls.BTN_CLEARALL.get()),
                 static_cast<Control *>(_controls.BTN_CLEARONE.get()),
                 static_cast<Control *>(_controls.BTN_CLEARONE2.get())}) {
            if (control) {
                control->setScale(combatTextScale);
            }
        }
        if (auto actionDescription = findControl<gui::Label>("LBL_ACTIONDESC")) {
            actionDescription->setScale(combatTextScale);
        }
    }

    // The menu strip and the action queue are icon artwork, magnified on any
    // modern screen, so they get their alpha edge reconstructed.
    for (auto &icon : {_controls.BTN_EQU, _controls.BTN_INV, _controls.BTN_CHAR,
                       _controls.BTN_ABI, _controls.BTN_MSG, _controls.BTN_JOU,
                       _controls.BTN_MAP, _controls.BTN_OPT}) {
        icon->setSharpenBorderFillAlpha(true);
    }
    for (auto &icon : {_controls.LBL_QUEUE0, _controls.LBL_QUEUE1,
                       _controls.LBL_QUEUE2, _controls.LBL_QUEUE3}) {
        icon->setSharpenBorderFillAlpha(true);
    }

    _actionBar.addDescription(
        findControl<gui::Label>("LBL_ACTIONDESC"),
        findControl<gui::Label>("LBL_ACTIONDESCBG"));

    // TSL adds a sixth slot, the forms.
    const int actionSlots = _game.isTSL() ? 6 : 5;
    for (int i = 0; i < actionSlots; ++i) {
        auto action = findControl<gui::Button>(str(boost::format("BTN_ACTION%d") % i));
        auto icon = findControl<gui::Button>(str(boost::format("LBL_ACTION%d") % i));
        auto up = findControl<gui::Button>(str(boost::format("BTN_ACTIONUP%d") % i));
        auto down = findControl<gui::Button>(str(boost::format("BTN_ACTIONDOWN%d") % i));

        if (action && icon && up && down) {
            _actionBar.addSlot(action, icon, up, down);
        }
    }

    _controls.BTN_CLEARALL->setVisible(false);
    _controls.BTN_TARGET0->setVisible(false);
    _controls.BTN_TARGET1->setVisible(false);
    _controls.BTN_TARGET2->setVisible(false);
    _controls.BTN_TARGETDOWN0->setVisible(false);
    _controls.BTN_TARGETDOWN1->setVisible(false);
    _controls.BTN_TARGETDOWN2->setVisible(false);
    _controls.BTN_TARGETUP0->setVisible(false);
    _controls.BTN_TARGETUP1->setVisible(false);
    _controls.BTN_TARGETUP2->setVisible(false);
    _controls.LBL_ARROW_MARGIN->setVisible(false);
    _controls.LBL_CASH->setVisible(false);
    _controls.LBL_CMBTEFCTINC1->setVisible(false);
    _controls.LBL_CMBTEFCTINC2->setVisible(false);
    _controls.LBL_CMBTEFCTINC3->setVisible(false);
    _controls.LBL_CMBTEFCTRED1->setVisible(false);
    _controls.LBL_CMBTEFCTRED2->setVisible(false);
    _controls.LBL_CMBTEFCTRED3->setVisible(false);
    _controls.LBL_CMBTMODEMSG->setVisible(false);
    _controls.LBL_CMBTMSGBG->setVisible(false);
    _controls.LBL_COMBATBG3->setVisible(false);
    _controls.LBL_DARKSHIFT->setVisible(false);
    _controls.LBL_DEBILATATED1->setVisible(false);
    _controls.LBL_DEBILATATED2->setVisible(false);
    _controls.LBL_DEBILATATED3->setVisible(false);
    _controls.LBL_DISABLE1->setVisible(false);
    _controls.LBL_DISABLE2->setVisible(false);
    _controls.LBL_DISABLE3->setVisible(false);
    _controls.LBL_JOURNAL->setVisible(false);
    _controls.LBL_HEALTHBG->setVisible(false);
    _controls.LBL_ITEMRCVD->setVisible(false);
    _controls.LBL_ITEMLOST->setVisible(false);
    _controls.LBL_LIGHTSHIFT->setVisible(false);
    _controls.LBL_MAP->setVisible(false);
    _controls.LBL_MOULDING1->setVisible(false);
    _controls.LBL_MOULDING3->setVisible(false);
    _controls.LBL_NAME->setVisible(false);
    _controls.LBL_NAMEBG->setVisible(false);
    _controls.LBL_PLOTXP->setVisible(false);
    _controls.LBL_STEALTHXP->setVisible(false);
    _controls.LBL_ARROW->setVisible(false);
    _controls.BTN_TARGET0->setVisible(false);

    if (_game.isTSL()) {
        // The weapon swap queues through the leader's combat round.
        _controls.BTN_SWAPWEAPONS->setOnClick([this]() {
            if (auto leader = _game.party().getLeader()) leader->requestSwitchWeapons(false);
        });
        tintHUDMenuButton(_controls.BTN_EQU, _baseColor);
        tintHUDMenuButton(_controls.BTN_INV, _baseColor);
        tintHUDMenuButton(_controls.BTN_CHAR, _baseColor);
        tintHUDMenuButton(_controls.BTN_ABI, _baseColor);
        tintHUDMenuButton(_controls.BTN_MSG, _baseColor);
        tintHUDMenuButton(_controls.BTN_JOU, _baseColor);
        tintHUDMenuButton(_controls.BTN_MAP, _baseColor);
        tintHUDMenuButton(_controls.BTN_OPT, _baseColor);
        // The bar backings are colour masks like the rest of the TSL HUD.
        for (auto &bar : {_controls.PB_VIT1, _controls.PB_VIT2, _controls.PB_VIT3,
                          _controls.PB_FORCE1, _controls.PB_FORCE2, _controls.PB_FORCE3}) {
            bar->setTintBorderFill(true);
        }
    } else {
        _controls.LBL_COMBATBG1->setVisible(false);
        _controls.LBL_COMBATBG2->setVisible(false);
        _controls.LBL_MOULDING2->setVisible(false);
    }

    _controls.BTN_EQU->setOnClick([this]() {
        _game.openInGameMenu(InGameMenuTab::Equipment);
    });
    _controls.BTN_INV->setOnClick([this]() {
        _game.openInGameMenu(InGameMenuTab::Inventory);
    });
    _controls.BTN_CHAR->setOnClick([this]() {
        _game.openInGameMenu(InGameMenuTab::Character);
    });
    _controls.BTN_ABI->setOnClick([this]() {
        _game.openInGameMenu(InGameMenuTab::Abilities);
    });
    _controls.BTN_MSG->setOnClick([this]() {
        if (_game.isTSL()) {
            _game.openInGameMenu(InGameMenuTab::Party);
        } else {
            _game.openInGameMenu(InGameMenuTab::Messages);
        }
    });
    _controls.BTN_JOU->setOnClick([this]() {
        _game.openInGameMenu(InGameMenuTab::Journal);
    });
    _controls.BTN_MAP->setOnClick([this]() {
        _game.openInGameMenu(InGameMenuTab::Map);
    });
    _controls.BTN_OPT->setOnClick([this]() {
        _game.openInGameMenu(InGameMenuTab::Options);
    });
    // The pause toggle stays as authored; its state follows play every frame.
    _controls.TB_PAUSE->setOnClick([this]() {
        _game.pressPauseButton();
    });
    _controls.TB_SOLO->setOnClick([this]() {
        _game.showSoloModeQuery(false);
    });
    _controls.TB_STEALTH->setOnClick([this]() {
        _game.requestStealth();
    });
    // Clear All leaves combat mode and clears the leader's actions, which one
    // that cannot be commanded keeps, its pending round entries and its targets.
    _controls.BTN_CLEARALL->setOnClick([this]() {
        const auto leader = _game.party().getLeader();
        if (!leader) return;
        leader->setClientCombatMode(false);
        _game.combat().clearAllOrders(*leader);
    });
    // Both clear-one buttons clear one action.
    _controls.BTN_CLEARONE->setOnClick([this]() { _game.clearOneAction(); });
    _controls.BTN_CLEARONE2->setOnClick([this]() { _game.clearOneAction(); });
    _controls.BTN_CHAR1->setOnClick([this]() {
        _game.openInGameMenu(InGameMenuTab::Equipment);
    });
    // A member who is down cannot take control.
    auto selectMember = [this](int index) {
        auto member = _game.party().getMember(index);
        if (!member || member->isDead() || member->isTemporarilyDead()) return;
        _game.party().setPartyLeaderByIndex(index);
    };
    _controls.BTN_CHAR2->setOnClick([selectMember]() { selectMember(1); });
    _controls.BTN_CHAR3->setOnClick([selectMember]() { selectMember(2); });

    _select.init(_controls.LBL_ARROW_MARGIN);

    _barkBubble = std::make_unique<BarkBubble>(_game, _services);
    if (_game.isTSL()) {
        // TSL authors the bark bubble over the minimap frame. Keep it below;
        // both GUIs share the 800x600 authored canvas.
        const auto &mapBorder = _controls.LBL_MAPBORDER->authoredExtent();
        _barkBubble->setAuthoredTop(mapBorder.top + mapBorder.height + kBarkBubbleMapGap);
    }
    _barkBubble->init();

    _statusSummary = std::make_unique<StatusSummary>(_game, _services, _game.statusSummary());
    _statusSummary->init();
    _statusSummary->setOnStatusFlash([this](StatusSummaryCategory category) { flashStatus(category); });

    _areaTransition = std::make_unique<AreaTransition>(_game, _services);
    _areaTransition->init();

    loadPausePanel();
}

void HUD::loadPausePanel() {
    _pauseGUI = _services.gui.guis.get(guiResRef("pause"), [this](IGUI &gui) {
        GameGUI::preload(gui);
        gui.setScaling(GUI::ScalingMode::PositionRelativeToCenter);
    });
    if (!_pauseGUI) {
        throw ResourceNotFoundException("GUI not found: " + guiResRef("pause"));
    }
    _pauseGUI->setEventListener(*this);
    auto find = [this](const std::string &tag) { return _pauseGUI->findControl(tag); };
    _pauseControls.BTN_UNPAUSE = std::static_pointer_cast<Button>(find("BTN_UNPAUSE"));
    _pauseControls.LBL_PAUSEREASON = std::static_pointer_cast<Label>(find("LBL_PAUSEREASON"));
    _pauseControls.LBL_PRESS = std::static_pointer_cast<Label>(find("LBL_PRESS"));
    // The prompt's authored text takes custom tokens.
    _pauseControls.LBL_PRESS->setTextMessage(_game.substituteCustomTokens(_pauseControls.LBL_PRESS->text().text));
    // A click anywhere on the indicator resumes play.
    _pauseControls.BTN_UNPAUSE->setOnClick([this]() {
        _game.togglePlayerPause();
    });
    _pauseLayoutReason.reset();
}

bool HUD::isPausePanelShown() const {
    return _pauseGUI && _game.isPaused() && pauseReasonText(_game.pauseReason()).has_value();
}

void HUD::updatePausePanel(float dt) {
    if (!_pauseGUI) return;
    if (!isPausePanelShown()) {
        if (_pauseLayoutReason) {
            _pauseGUI->clearSelection();
            _pauseLayoutReason.reset();
        }
        return;
    }
    const PauseReason reason = _game.pauseReason();
    if (_pauseLayoutReason != reason) layoutPausePanel(reason);
    _pauseGUI->update(dt);
}

void HUD::layoutPausePanel(PauseReason reason) {
    _pauseLayoutReason = reason;
    const auto text = *pauseReasonText(reason);
    const bool tsl = _game.isTSL();
    auto &root = _pauseGUI->rootControl();
    auto &reasonLabel = *_pauseControls.LBL_PAUSEREASON;
    auto &pressLabel = *_pauseControls.LBL_PRESS;
    auto &unpause = *_pauseControls.BTN_UNPAUSE;

    const float scale = _pauseGUI->scale();
    auto scaled = [scale](const Control::Extent &authored) {
        return Control::Extent {
            static_cast<int>(std::lround(authored.left * scale)),
            static_cast<int>(std::lround(authored.top * scale)),
            static_cast<int>(std::lround(authored.width * scale)),
            static_cast<int>(std::lround(authored.height * scale))};
    };
    auto textHeight = [](const Label &label) {
        if (!label.text().font) return 0;
        const int lineHeight = std::max(1, static_cast<int>(std::lround(label.text().font->height() * label.scale())));
        return std::max(1, static_cast<int>(label.textLines().size())) * lineHeight;
    };
    for (Control *control : {static_cast<Control *>(&root), static_cast<Control *>(&reasonLabel),
                             static_cast<Control *>(&pressLabel), static_cast<Control *>(&unpause)}) {
        control->setPresentationScale(scale);
    }

    // Panel-relative layout: the prompt sits 2 below the reason, and the
    // panel ends 5 below the last shown line.
    Control::Extent reasonExtent = scaled(reasonLabel.authoredExtent());
    reasonLabel.setExtent(reasonExtent);
    // TSL resolves the reason's tokens a second time.
    std::string reasonText = _game.getInterfaceText(tsl ? text.k2StrRef : text.k1StrRef);
    if (tsl) reasonText = _game.substituteCustomTokens(std::move(reasonText));
    reasonLabel.setTextMessage(reasonText);
    reasonExtent.height = textHeight(reasonLabel);
    Control::Extent last = reasonExtent;
    Control::Extent pressExtent = scaled(pressLabel.authoredExtent());
    pressLabel.setVisible(text.press);
    if (text.press) {
        pressLabel.setExtent(pressExtent);
        if (tsl) {
            pressLabel.setTextMessage(_game.getInterfaceText(kK2PausePressStrRef));
        }
        pressExtent.top = reasonExtent.top + reasonExtent.height + static_cast<int>(std::lround(2 * scale));
        pressExtent.height = textHeight(pressLabel);
        last = pressExtent;
    }
    Control::Extent panelExtent = scaled(root.authoredExtent());
    panelExtent.height = last.top + last.height + static_cast<int>(std::lround(5 * scale));
    Control::Extent unpauseExtent {reasonExtent.left, reasonExtent.top, reasonExtent.width, panelExtent.height};

    // Top-right, just under the menu bar.
    const int screenWidth = _game.options().graphics.width;
    const auto &menuButton = _controls.BTN_EQU->extent();
    panelExtent.top = menuButton.top + menuButton.height - 1;
    if (tsl) {
        const float textScalar = effectStackTextScalar(true, screenWidth);
        panelExtent.top += static_cast<int>(11.0f * textScalar);
        panelExtent.left = static_cast<int>(static_cast<float>(screenWidth - panelExtent.width) + textScalar * -6.0f);
    } else {
        panelExtent.top += 3;
        panelExtent.left = screenWidth - panelExtent.width - 3;
    }

    auto place = [&panelExtent](Control::Extent extent) {
        extent.left += panelExtent.left;
        extent.top += panelExtent.top;
        return extent;
    };
    root.setExtent(panelExtent);
    reasonLabel.setExtent(place(reasonExtent));
    if (text.press) pressLabel.setExtent(place(pressExtent));
    unpause.setExtent(place(unpauseExtent));
}

gui::Label *HUD::statusFlashLabel(StatusSummaryCategory category) const {
    switch (category) {
    case StatusSummaryCategory::Journal:
        return _controls.LBL_JOURNAL.get();
    case StatusSummaryCategory::Credits:
        return _controls.LBL_CASH.get();
    case StatusSummaryCategory::PlotXP:
        return _controls.LBL_PLOTXP.get();
    case StatusSummaryCategory::StealthXP:
        return _controls.LBL_STEALTHXP.get();
    case StatusSummaryCategory::DarkSideShift:
        return _controls.LBL_DARKSHIFT.get();
    case StatusSummaryCategory::LightSideShift:
        return _controls.LBL_LIGHTSHIFT.get();
    case StatusSummaryCategory::ItemsReceived:
        return _controls.LBL_ITEMRCVD.get();
    case StatusSummaryCategory::ItemsLost:
        return _controls.LBL_ITEMLOST.get();
    default:
        return nullptr;
    }
}

void HUD::presentAlignmentShift(const StatusSummaryBatch &batch) {
    static constexpr int kDarkSideVisual = 9014;
    static constexpr int kLightSideVisual = 9015;
    if (!_game.isTSL()) return;
    const int dark = batch.entry(StatusSummaryCategory::DarkSideShift).amount;
    const int light = batch.entry(StatusSummaryCategory::LightSideShift).amount;
    if (dark == light) return;
    _services.audio.mixer.play(_services.game.guiSounds.getAlignmentShift(light > dark), AudioType::Sound);
    auto leader = _game.party().getLeader();
    if (!leader || !leader->isPlayerCreated()) return;
    auto visual = std::make_shared<VisualEffect>(light > dark ? kLightSideVisual : kDarkSideVisual, false, _services);
    auto instance = visual->saveFacingInstance();
    instance.effect = visual;
    instance.creatorId = leader->id();
    _game.bindEffectCreator(instance);
    instance.setDuration(DurationType::Instant, 0.0f);
    leader->applyEffect(std::move(instance));
}

void HUD::flashStatus(StatusSummaryCategory category) {
    auto shared = [](StatusSummaryCategory flashed) -> std::optional<StatusSummaryCategory> {
        switch (flashed) {
        case StatusSummaryCategory::PlotXP:
            return StatusSummaryCategory::StealthXP;
        case StatusSummaryCategory::StealthXP:
            return StatusSummaryCategory::PlotXP;
        case StatusSummaryCategory::DarkSideShift:
            return StatusSummaryCategory::LightSideShift;
        case StatusSummaryCategory::LightSideShift:
            return StatusSummaryCategory::DarkSideShift;
        default:
            return std::nullopt;
        }
    };
    if (auto other = shared(category)) {
        _statusFlashes[static_cast<size_t>(*other)].reset();
        if (auto label = statusFlashLabel(*other)) label->setVisible(false);
    }
    auto label = statusFlashLabel(category);
    if (!label) return;
    _statusFlashes[static_cast<size_t>(category)].activate();
    label->setVisible(true);
}

void HUD::resetStatusSummaryPresentation() {
    for (size_t i = 0; i < _statusFlashes.size(); ++i) {
        _statusFlashes[i].reset();
        if (auto label = statusFlashLabel(static_cast<StatusSummaryCategory>(i))) label->setVisible(false);
    }
    if (_statusSummary) {
        _statusSummary->reset();
    }
}

void HUD::onClick(const std::string &control) {
    // The pause toggle clicks like a checkbox.
    if (control == "TB_PAUSE" || control == "TB_SOLO" || control == "TB_STEALTH") {
        if (auto clip = _services.game.guiSounds.getCheckboxCheck()) {
            _audioSource = _services.audio.mixer.play(std::move(clip), AudioType::Sound);
        }
        return;
    }
    // Action submission owns its accept/reject sound. Do not precede a
    // rejection with the generic GUI click emitted before the button handler.
    if (!_actionBar.ownsActionControl(control)) GameGUI::onClick(control);
}

bool HUD::handle(const input::Event &event) {
    if (_statusSummary && _statusSummary->isVisible()) {
        return _statusSummary->handle(event);
    }
    // The pause indicator is not modal: it takes only what lands on it.
    if (isPausePanelShown() && _pauseLayoutReason) {
        if (_pauseGUI->handle(event)) return true;
        if (event.type == input::EventType::MouseButtonDown &&
            _pauseControls.BTN_UNPAUSE->extent().contains(event.button.x, event.button.y)) {
            return true;
        }
    }
    if (_select.handle(event)) {
        return true;
    }
    return _gui->handle(event);
}

void HUD::update(float dt) {
    _gui->update(dt);
    // The toggle shows any pause, not only the player's.
    _controls.TB_PAUSE->setOn(_game.isPaused());
    // Solo mode shows with companions; stealth when the leader can use it.
    _controls.TB_SOLO->setVisible(_game.party().companionCount() > 0);
    _controls.TB_SOLO->setOn(_game.party().isSoloMode());
    auto stealthLeader = _game.party().getLeader();
    _controls.TB_STEALTH->setVisible(stealthLeader && stealthLeader->isStealthCapable());
    _controls.TB_STEALTH->setOn(stealthLeader && stealthLeader->isStealthed());
    updatePausePanel(dt);

    // Module/script work is updated before the HUD. Snapshotting here gives
    // all status events in that batch one deterministic coalescing boundary,
    // without a user-visible timer or synchronous popup from the mutation.
    // With the summary option off, what is pending is dropped unseen.
    if (_statusSummary && (_game.feedbackOptions() & feedbackoption::kStatusSummary) == 0) {
        // With the summary option off, the pending rows only flash their
        // indicators, and the batch is dropped.
        const auto &pending = _game.statusSummary().pending();
        for (auto category : pending.activeCategories()) {
            if (pending.flashes(category, _game.isTSL())) flashStatus(category);
        }
        presentAlignmentShift(pending);
        const bool reported = !pending.empty();
        _game.statusSummary().discardPending();
        if (reported) _game.finishStatusSummaryCycle();
    } else if (_statusSummary && _statusSummary->presentPending()) {
        // The summary takes all input, releases included; steering stops.
        _game.stopMovement();
        auto clip = _services.resource.audioClips.get("gui_quest");
        _audioSource = _services.audio.mixer.play(std::move(clip), AudioType::Sound);
        presentAlignmentShift(*_game.statusSummary().displayed());
    }

    Party &party = _game.party();
    std::vector<Label *> charLabels {
        _controls.LBL_CHAR1.get(),
        _controls.LBL_CHAR2.get(),
        _controls.LBL_CHAR3.get()};
    std::vector<Label *> backLabels {
        _controls.LBL_BACK1.get(),
        _controls.LBL_BACK2.get(),
        _controls.LBL_BACK3.get()};
    std::vector<Label *> lvlUpBgLabels {
        _controls.LBL_LVLUPBG1.get(),
        _controls.LBL_LVLUPBG2.get(),
        _controls.LBL_LVLUPBG3.get()};
    std::vector<Label *> levevlUpLabels {
        _controls.LBL_LEVELUP1.get(),
        _controls.LBL_LEVELUP2.get(),
        _controls.LBL_LEVELUP3.get()};
    std::array<Label *, 3> debilitatedLabels {
        _controls.LBL_DEBILATATED1.get(),
        _controls.LBL_DEBILATATED2.get(),
        _controls.LBL_DEBILATATED3.get()};
    std::array<ProgressBar *, 3> vitalityBars {
        _controls.PB_VIT1.get(),
        _controls.PB_VIT2.get(),
        _controls.PB_VIT3.get()};
    std::array<ProgressBar *, 3> forceBars {
        _controls.PB_FORCE1.get(),
        _controls.PB_FORCE2.get(),
        _controls.PB_FORCE3.get()};

    for (int i = 0; i < 3; ++i) {
        Label &LBL_CHAR = *charLabels[i];
        Label &LBL_BACK = *backLabels[i];
        Label &LBL_LEVELUP = *levevlUpLabels[i];

        Label *LBL_LVLUPBG;
        if (!_game.isTSL()) {
            LBL_LVLUPBG = lvlUpBgLabels[i];
        }

        std::shared_ptr<Creature> member(party.getMember(i));
        if (member) {
            LBL_CHAR.setVisible(true);
            LBL_CHAR.setBorderFill(member->portrait());
            LBL_BACK.setVisible(true);
            // A fallen member shows no mark; otherwise a pending level-up
            // shows ahead of being held helpless.
            const bool down = member->isDead() || member->isTemporarilyDead();
            const bool levelUp = !down && member->isLevelUpPending();
            const bool debilitated = !down && !levelUp && member->isDebilitated();
            LBL_LEVELUP.setVisible(levelUp);
            if (!_game.isTSL()) {
                LBL_LVLUPBG->setVisible(levelUp);
            }
            debilitatedLabels[i]->setVisible(debilitated);
            // A helpless leader says so on the combat line. Any member able
            // to act, the leader included, returns it to the idle line, so
            // it stays only while the whole party is helpless or fallen.
            if (i == 0 && debilitated) setCombatMessage(kLeaderDebilitatedCombatMessage);
            if (!member->isDebilitated() && _combatMessageStrref == kLeaderDebilitatedCombatMessage) {
                setCombatMessage(party.idleCombatMessage());
            }
            int maxHitPoints = member->maxHitPoints();
            vitalityBars[i]->setVisible(true);
            vitalityBars[i]->setValue(maxHitPoints > 0
                ? std::clamp(100 * member->currentHitPoints() / maxHitPoints, 0, 100)
                : 0);

            int forcePoints = member->maxForcePoints();
            forceBars[i]->setVisible(forcePoints > 0);
            forceBars[i]->setValue(forcePoints > 0
                ? std::clamp(100 * member->currentForce() / forcePoints, 0, 100)
                : 0);
        } else {
            LBL_CHAR.setVisible(false);
            LBL_BACK.setVisible(false);
            LBL_LEVELUP.setVisible(false);
            if (!_game.isTSL()) {
                LBL_LVLUPBG->setVisible(false);
            }
            debilitatedLabels[i]->setVisible(false);
            vitalityBars[i]->setVisible(false);
            forceBars[i]->setVisible(false);
        }
    }

    // With no one under control there is no combat mode to show.
    if (auto leader = party.getLeader(); leader && leader->clientCombatMode()) {
        toggleCombat(true);
        refreshActionQueueItems();
    } else {
        toggleCombat(false);
    }

    updateCombatMessage(dt);
    for (size_t i = 0; i < _statusFlashes.size(); ++i) {
        auto label = statusFlashLabel(static_cast<StatusSummaryCategory>(i));
        if (!label) continue;
        _statusFlashes[i].update(dt);
        label->setVisible(_statusFlashes[i].visible());
    }

    _select.update(dt);
    _actionBar.update(dt);
    updateTransitionPresentation();
    _barkBubble->update(dt);
    if (_statusSummary && _statusSummary->isVisible()) {
        _statusSummary->update(dt);
    }

    // Hide minimap when there is no image to display
    _controls.LBL_MAPBORDER->setVisible(_game.map().isLoaded());

    if (_capturePresentation) {
        _controls.LBL_CHAR1->setVisible(true);
        _controls.LBL_BACK1->setVisible(true);
        _controls.PB_HEALTH->setVisible(true);
        _controls.PB_HEALTH->setValue(65);
        _controls.PB_VIT1->setVisible(true);
        _controls.PB_VIT1->setValue(65);
        _controls.PB_FORCE1->setVisible(true);
        _controls.PB_FORCE1->setValue(45);
        if (_captureCombatPresentation) {
            toggleCombat(true);
            for (auto &queue : {_controls.LBL_QUEUE0, _controls.LBL_QUEUE1,
                                _controls.LBL_QUEUE2, _controls.LBL_QUEUE3}) {
                queue->setBorderFill(g_attackIcon);
            }
        }
    }
}

void HUD::showCapturePresentation(bool combat) {
    _capturePresentation = true;
    _captureCombatPresentation = combat;
    _gui->rootControl().setVisible(true);
}

void HUD::showTransitionCapturePresentation(const std::string &destination) {
    if (!_areaTransition) {
        throw std::runtime_error("Area-transition GUI is unavailable");
    }
    _captureTransitionPresentation = true;
    _areaTransition->show(destination);
}

void HUD::clearCapturePresentation() {
    _capturePresentation = false;
    _captureCombatPresentation = false;
    _captureTransitionPresentation = false;
    if (_barkBubble) {
        _barkBubble->setBarkText("", 0.0f);
    }
    if (_areaTransition) {
        _areaTransition->hide();
    }
}

void HUD::updateTransitionPresentation() {
    if (!_areaTransition) {
        return;
    }
    if (_captureTransitionPresentation) {
        return;
    }
    auto candidate = currentTransitionCandidate();
    if (candidate) {
        _areaTransition->show(candidate->destination);
    } else {
        _areaTransition->hide();
    }
}

std::optional<TransitionPortal> HUD::currentTransitionCandidate() const {
    auto module = _game.module();
    if (!module || !module->area()) {
        return std::nullopt;
    }
    auto leader = _game.party().getLeader();
    if (!leader) {
        return std::nullopt;
    }
    auto camera = _game.getActiveCamera();
    if (!camera) {
        return std::nullopt;
    }
    auto cameraSceneNode = camera->cameraSceneNode();
    if (!cameraSceneNode || !cameraSceneNode->camera()) {
        return std::nullopt;
    }
    auto &graphicsCamera = *cameraSceneNode->camera();

    TransitionView view;
    view.leaderPosition = leader->position();
    view.cameraViewProjection = graphicsCamera.projection() * graphicsCamera.view();

    return pickTransitionPortal(module->area()->transitionPresentationPortals(), view);
}

void HUD::render() {
    _gui->render();
    if (isPausePanelShown() && _pauseLayoutReason) _pauseGUI->render();

    renderEffectStacks();
    renderMinimap();


    _barkBubble->render();
    if (_areaTransition && _areaTransition->isVisible()) {
        _areaTransition->render();
    }
    _select.render();
    _actionBar.render(_gui->scale());
    _game.floatingText().render();
}

void HUD::renderModal() {
    if (_statusSummary && _statusSummary->isVisible()) {
        _statusSummary->render();
    }
}

void HUD::renderMinimap() {
    const Control::Extent &extent = _controls.LBL_MAPVIEW->extent();

    glm::vec4 bounds;
    bounds[0] = static_cast<float>(_gui->controlOffset().x + extent.left);
    bounds[1] = static_cast<float>(_gui->controlOffset().y + extent.top);
    bounds[2] = static_cast<float>(extent.width);
    bounds[3] = static_cast<float>(extent.height);

    std::shared_ptr<Area> area(_game.module()->area());
    // The frame is a control and scales with the layout; the map drawn inside
    // it is not, so it has to be told.
    _game.map().render(Map::Mode::Minimap, bounds, _gui->scale());
}

static constexpr float kCombatMessageDuration = 2.5f;
static const glm::vec3 kCombatMessageColor {0.7f, 0.7f, 0.6f};

// TSL draws every combat-mode line in one colour; KotOR colours it by line.
static glm::vec3 combatMessageColor(bool tsl, int strref) {
    if (tsl) return kCombatMessageColor;
    switch (strref) {
    case 48208: return {0.74f, 0.11f, 0.0f};  // red
    case 47915: return {0.98f, 0.45f, 0.0f};  // orange
    case 42476:
    case 42478: return {0.28f, 0.92f, 0.11f}; // green
    case 47859: return {0.95f, 0.0f, 0.85f};  // purple
    default: return {0.0f, 0.66f, 0.98f};     // blue
    }
}

void HUD::setCombatMessage(int strref) {
    if (_combatMessageRemaining > 0.0f) return;
    const int idle = _game.party().idleCombatMessage();
    // Queue-state lines give way to an autopause.
    if ((strref == 42476 || strref == 42477 || strref == 47859) && _game.isAutoPaused()) return;
    const bool persistent = strref == idle || strref == kLeaderDebilitatedCombatMessage || strref == 42476 || strref == 42477;
    auto &label = *_controls.LBL_CMBTMODEMSG;
    label.setTextColor(combatMessageColor(_game.isTSL(), strref));
    label.setTextMessage(_game.getInterfaceText(strref));
    label.setTextOpacity(1.0f);
    _combatMessageStrref = strref;
    _combatMessageDuration = persistent ? 0.0f : kCombatMessageDuration;
    _combatMessageRemaining = _combatMessageDuration;
}

// A timed line holds for half its time, fades over the rest, then yields to
// the controlled character's idle line.
void HUD::updateCombatMessage(float dt) {
    if (_combatMessageRemaining <= 0.0f) return;
    _combatMessageRemaining -= dt;
    if (_combatMessageRemaining > 0.0f) {
        _controls.LBL_CMBTMODEMSG->setTextOpacity(
            std::min(1.0f, 2.0f * _combatMessageRemaining / _combatMessageDuration));
        return;
    }
    _combatMessageRemaining = 0.0f;
    if (auto leader = _game.party().getLeader())
        _game.party().setCombatMessage(*leader, _game.party().idleCombatMessage());
}

void HUD::toggleCombat(bool enabled) {
    _controls.BTN_CLEARALL->setVisible(enabled);
    _controls.BTN_CLEARONE->setVisible(enabled);
    _controls.BTN_CLEARONE2->setVisible(enabled);
    _controls.LBL_CMBTMODEMSG->setVisible(enabled);
    _controls.LBL_CMBTMSGBG->setVisible(enabled);
    _controls.LBL_QUEUE0->setVisible(enabled);
    _controls.LBL_QUEUE1->setVisible(enabled);
    _controls.LBL_QUEUE2->setVisible(enabled);
    _controls.LBL_QUEUE3->setVisible(enabled);

    if (_game.isTSL()) {
        // The first two TSL backings are persistent HUD art. The third is the
        // combat-sequence extension and follows combat visibility.
        _controls.LBL_COMBATBG3->setVisible(enabled);
    } else {
        _controls.LBL_COMBATBG1->setVisible(enabled);
        _controls.LBL_COMBATBG2->setVisible(enabled);
        _controls.LBL_COMBATBG3->setVisible(enabled);
    }
}

void HUD::refreshActionQueueItems() const {
    const auto leader = _game.party().getLeader();
    auto &actions = leader->actions();
    std::vector<Label *> queueLabels {
        _controls.LBL_QUEUE0.get(),
        _controls.LBL_QUEUE1.get(),
        _controls.LBL_QUEUE2.get(),
        _controls.LBL_QUEUE3.get()};

    // The icon an order shows: a power or item use its own, a feat its own.
    auto commandIcon = [this](Action &action) -> std::optional<std::shared_ptr<graphics::Texture>> {
        if (auto *cast = dyn_cast<CastSpellAtObjectAction>(&action)) return castIcon(*cast->spell(), cast->item());
        if (auto *cast = dyn_cast<CastSpellAtLocationAction>(&action)) return castIcon(*cast->spell(), cast->item());
        if (auto *feat = dyn_cast<UseFeatAction>(&action)) {
            std::shared_ptr<Feat> row(_services.game.feats.get(feat->feat()));
            return row ? row->icon : nullptr;
        }
        return std::nullopt;
    };
    // Each queued order takes a slot; the round dispatcher shows each entry
    // it holds, or else the stance or attack its round serves. Entries still
    // pending with no dispatcher in the queue follow the orders.
    std::vector<std::variant<std::string, std::shared_ptr<graphics::Texture>>> icons;
    const auto pending = _game.combat().pendingScheduled(*leader);
    auto addPending = [&]() {
        for (const auto &[kind, command] : pending) {
            if (icons.size() >= queueLabels.size()) break;
            if (auto icon = command ? commandIcon(command->combatAction()) : std::nullopt) {
                icons.emplace_back(*icon);
                continue;
            }
            switch (kind) {
            case 6: icons.emplace_back(std::string("i_equip")); break;
            case 7: icons.emplace_back(std::string("i_unequip")); break;
            case 13: icons.emplace_back(std::string("i_stancedef")); break;
            case 14: icons.emplace_back(std::string("i_swtchwpn")); break;
            default: icons.emplace_back(g_attackIcon); break;
            }
        }
    };
    bool dispatcherShown = false;
    for (const auto &queued : actions) {
        if (icons.size() >= queueLabels.size()) break;
        if (queued->isCompleted() || queued->isCancelled()) continue;
        Action &displayAction = queued->combatAction();
        switch (displayAction.type()) {
        case ActionType::AttackObject:
            icons.emplace_back(g_attackIcon);
            break;
        case ActionType::CombatDispatch:
            dispatcherShown = true;
            if (pending.empty()) {
                icons.emplace_back(leader->isInTotalDefense() ? std::string("i_stancedef") : g_attackIcon);
            } else {
                addPending();
            }
            break;
        default:
            if (auto icon = commandIcon(displayAction)) {
                icons.emplace_back(*icon);
            } else {
                icons.emplace_back(std::string());
            }
            break;
        }
    }

    if (!dispatcherShown) addPending();

    for (size_t i = 0; i < queueLabels.size(); ++i) {
        Label &item = *queueLabels[i];
        if (i >= icons.size()) {
            item.setBorderFill("");
        } else if (auto *name = std::get_if<std::string>(&icons[i])) {
            item.setBorderFill(*name);
        } else {
            item.setBorderFill(std::get<std::shared_ptr<graphics::Texture>>(icons[i]));
        }
    }
}

void HUD::renderEffectStacks() {

    Party &party = _game.party();
    std::array<Label *, 3> backLabels {
        _controls.LBL_BACK1.get(),
        _controls.LBL_BACK2.get(),
        _controls.LBL_BACK3.get()};
    std::array<Label *, 3> positiveLabels {
        _controls.LBL_CMBTEFCTINC1.get(),
        _controls.LBL_CMBTEFCTINC2.get(),
        _controls.LBL_CMBTEFCTINC3.get()};
    std::array<Label *, 3> negativeLabels {
        _controls.LBL_CMBTEFCTRED1.get(),
        _controls.LBL_CMBTEFCTRED2.get(),
        _controls.LBL_CMBTEFCTRED3.get()};

    auto &options = _game.options().graphics;
    glm::ivec2 screenSize(options.width, options.height);
    const glm::ivec2 &controlOffset = _gui->controlOffset();

    _services.graphics.context.withBlendMode(BlendMode::Normal, [&]() {
        scene::RetroRenderPass retroPass(
            options,
            _services.graphics.context,
            _services.graphics.shaderRegistry,
            _services.graphics.statistic,
            _services.graphics.meshRegistry,
            _services.graphics.textureRegistry,
            _services.graphics.uniforms);
        scene::PBRRenderPass pbrPass(
            options,
            _services.graphics.context,
            _services.graphics.shaderRegistry,
            _services.graphics.statistic,
            _services.graphics.meshRegistry,
            _services.graphics.pbrTextures,
            _services.graphics.textureRegistry,
            _services.graphics.uniforms);
        scene::IRenderPass &pass = options.pbr
                                      ? static_cast<scene::IRenderPass &>(pbrPass)
                                      : static_cast<scene::IRenderPass &>(retroPass);

        for (int memberIndex = 0; memberIndex < 3; ++memberIndex) {
            auto member = party.getMember(memberIndex);
            if (!member) {
                continue;
            }

            auto counts = member->effectStackCounts();
            renderEffectStack(
                *positiveLabels[memberIndex],
                backLabels[memberIndex]->extent(),
                counts.positive,
                memberIndex == 0,
                true,
                effectStackTextScalar(_game.isTSL(), screenSize.x),
                screenSize,
                controlOffset,
                pass,
                _services.graphics.context);
            renderEffectStack(
                *negativeLabels[memberIndex],
                backLabels[memberIndex]->extent(),
                counts.negative,
                memberIndex == 0,
                false,
                effectStackTextScalar(_game.isTSL(), screenSize.x),
                screenSize,
                controlOffset,
                pass,
                _services.graphics.context);
        }
    });
}

} // namespace game

} // namespace reone
