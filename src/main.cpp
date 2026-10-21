#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <functional>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

// ---------------------------------------------------------------
// Settings (saved between sessions)
// ---------------------------------------------------------------
namespace cfg {
    inline bool get(char const* key) {
        return Mod::get()->getSavedValue<bool>(key, false);
    }
    inline void set(char const* key, bool v) {
        Mod::get()->setSavedValue<bool>(key, v);
    }
    constexpr float CLICKS_PER_SECOND = 10.f;
    constexpr float HOLD_TIME = 0.04f;
}

// ---------------------------------------------------------------
// Draggable floating button
// Tap = open menu, drag = move (position is saved)
// ---------------------------------------------------------------
class DraggableButton : public CCNode, public CCTouchDelegate {
protected:
    CCNode* m_visual = nullptr;
    std::function<void()> m_onTap;
    bool m_dragging = false;
    bool m_touching = false;
    CCPoint m_nodeStart;
    CCPoint m_touchStart;

    bool init(CCNode* visual, std::function<void()> onTap) {
        if (!CCNode::init()) return false;
        m_onTap = std::move(onTap);
        m_visual = visual;

        this->setContentSize({45.f, 45.f});
        this->setAnchorPoint({0.5f, 0.5f});
        visual->setPosition(this->getContentSize() / 2);
        this->addChild(visual);
        return true;
    }

    // Is this node actually on screen and usable?
    bool isUsable() {
        for (CCNode* n = this; n; n = n->getParent()) {
            if (!n->isVisible()) return false;
        }
        return true;
    }

public:
    static DraggableButton* create(CCNode* visual, std::function<void()> onTap) {
        auto ret = new DraggableButton();
        if (ret->init(visual, std::move(onTap))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void onEnter() override {
        CCNode::onEnter();
        CCTouchDispatcher::get()->addTargetedDelegate(this, -600, true);
    }

    void onExit() override {
        CCTouchDispatcher::get()->removeDelegate(this);
        CCNode::onExit();
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (!isUsable()) return false;

        auto local = this->convertToNodeSpace(touch->getLocation());
        auto size = this->getContentSize();
        if (local.x < 0 || local.y < 0 || local.x > size.width || local.y > size.height) {
            return false;
        }

        m_touching = true;
        m_dragging = false;
        m_nodeStart = this->getPosition();
        m_touchStart = this->getParent()
            ? this->getParent()->convertToNodeSpace(touch->getLocation())
            : touch->getLocation();
        m_visual->setScale(0.92f);
        return true; // swallow so the game doesn't also register a jump
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (!m_touching) return;

        auto parent = this->getParent();
        auto now = parent ? parent->convertToNodeSpace(touch->getLocation()) : touch->getLocation();
        auto diff = now - m_touchStart;

        if (!m_dragging && (std::abs(diff.x) > 6.f || std::abs(diff.y) > 6.f)) {
            m_dragging = true;
        }

        if (m_dragging) {
            auto win = CCDirector::sharedDirector()->getWinSize();
            auto p = m_nodeStart + diff;
            p.x = std::clamp(p.x, 25.f, win.width - 25.f);
            p.y = std::clamp(p.y, 25.f, win.height - 25.f);
            this->setPosition(p);
        }
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        if (!m_touching) return;
        m_touching = false;
        m_visual->setScale(1.f);

        if (m_dragging) {
            m_dragging = false;
            auto p = this->getPosition();
            Mod::get()->setSavedValue<float>("btn-x", p.x);
            Mod::get()->setSavedValue<float>("btn-y", p.y);
        } else if (m_onTap) {
            m_onTap();
        }
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_touching = false;
        m_dragging = false;
        m_visual->setScale(1.f);
    }
};

// ---------------------------------------------------------------
// Menu popup
// ---------------------------------------------------------------
class BotMenu : public Popup {
protected:
    bool init() {
        if (!Popup::init(280.f, 190.f)) return false;
        this->setTitle("Bot Menu");

        auto menu = CCMenu::create();
        menu->setPosition({140.f, 90.f});
        m_mainLayer->addChild(menu);

        this->addToggle(menu, "Noclip", "noclip", 40.f);
        this->addToggle(menu, "Autoclicker", "autoclicker", 0.f);
        this->addToggle(menu, "Layout Mode", "layout", -40.f);
        return true;
    }

    void addToggle(CCMenu* menu, char const* text, char const* key, float y) {
        auto on = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto off = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");

        auto toggler = CCMenuItemExt::createToggler(on, off, [key](CCMenuItemToggler* t) {
            // callback runs before the visual state flips
            cfg::set(key, !t->isToggled());
        });
        toggler->toggle(cfg::get(key));
        toggler->setPosition({-80.f, y});
        menu->addChild(toggler);

        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(0.55f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({-55.f, y});
        menu->addChild(label);
    }

public:
    static BotMenu* create() {
        auto ret = new BotMenu();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------
// PlayLayer hook
// ---------------------------------------------------------------
class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        bool prevLayout = false;
        float clickTimer = 0.f;
        float holdTimer = 0.f;
        bool holding = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        // Button visual
        auto bg = extension::CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({45.f, 45.f});
        bg->setOpacity(110);

        if (auto icon = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png")) {
            icon->setScale(0.9f);
            icon->setPosition(bg->getContentSize() / 2);
            bg->addChild(icon);
        }

        auto btn = DraggableButton::create(bg, [] {
            BotMenu::create()->show();
        });
        if (!btn) return true;

        auto win = CCDirector::sharedDirector()->getWinSize();
        float x = Mod::get()->getSavedValue<float>("btn-x", 40.f);
        float y = Mod::get()->getSavedValue<float>("btn-y", win.height - 40.f);
        btn->setPosition({
            std::clamp(x, 25.f, win.width - 25.f),
            std::clamp(y, 25.f, win.height - 25.f)
        });

        // UILayer lives inside the PlayLayer's own scene, so it's never the wrong scene
        if (m_uiLayer) m_uiLayer->addChild(btn, 1000);
        else this->addChild(btn, 1000);

        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (cfg::get("noclip")) return;
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->clickTimer = 0.f;
        m_fields->holdTimer = 0.f;
        m_fields->holding = false;
        // objects get reset, so re-apply layout visuals
        this->applyLayoutMode(cfg::get("layout"));
    }

    void onQuit() {
        // restore anything we touched
        this->applyLayoutMode(false);
        PlayLayer::onQuit();
    }

    void update(float dt) {
        PlayLayer::update(dt);

        if (cfg::get("autoclicker")) this->tickAutoclicker(dt);

        bool layout = cfg::get("layout");
        if (layout != m_fields->prevLayout) {
            m_fields->prevLayout = layout;
            this->applyLayoutMode(layout);
        }
    }

    void tickAutoclicker(float dt) {
        if (m_player1 && m_player1->m_isDead) return;
        auto f = m_fields.self();

        if (f->holding) {
            f->holdTimer -= dt;
            if (f->holdTimer <= 0.f) {
                f->holding = false;
                m_player1->releaseButton(PlayerButton::Jump);
                if (m_gameState.m_isDualMode && m_player2) {
                    m_player2->releaseButton(PlayerButton::Jump);
                }
            }
            return;
        }

        f->clickTimer += dt;
        if (f->clickTimer >= 1.f / cfg::CLICKS_PER_SECOND) {
            f->clickTimer = 0.f;
            f->holding = true;
            f->holdTimer = cfg::HOLD_TIME;
            m_player1->pushButton(PlayerButton::Jump);
            if (m_gameState.m_isDualMode && m_player2) {
                m_player2->pushButton(PlayerButton::Jump);
            }
        }
    }

    void applyLayoutMode(bool enable) {
        if (m_objects) {
            for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
                if (obj && obj->m_objectType == GameObjectType::Decoration) {
                    obj->setVisible(!enable);
                }
            }
        }
        if (m_background) {
            m_background->setColor(enable ? ccColor3B{40, 62, 255} : ccColor3B{255, 255, 255});
        }
    }
};
