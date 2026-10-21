#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/ui/Popup.hpp>
#include <functional>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

// ---------------------------------------------------------------
// Shared state (cached in memory, saved to disk when changed)
// ---------------------------------------------------------------
struct State {
    bool noclip = false;
    bool counter = false;     // show noclip hits / accuracy
    bool jump = false;        // infinite jump
    bool clicker = false;
    bool layout = false;
    bool speedOn = false;
    float speed = 1.f;
    float cps = 10.f;

    void load() {
        auto m = Mod::get();
        noclip  = m->getSavedValue<bool>("noclip", false);
        counter = m->getSavedValue<bool>("counter", false);
        jump    = m->getSavedValue<bool>("jump", false);
        clicker = m->getSavedValue<bool>("clicker", false);
        layout  = m->getSavedValue<bool>("layout", false);
        speedOn = m->getSavedValue<bool>("speedOn", false);
        speed   = m->getSavedValue<float>("speed", 1.f);
        cps     = m->getSavedValue<float>("cps", 10.f);
    }

    void save() {
        auto m = Mod::get();
        m->setSavedValue<bool>("noclip", noclip);
        m->setSavedValue<bool>("counter", counter);
        m->setSavedValue<bool>("jump", jump);
        m->setSavedValue<bool>("clicker", clicker);
        m->setSavedValue<bool>("layout", layout);
        m->setSavedValue<bool>("speedOn", speedOn);
        m->setSavedValue<float>("speed", speed);
        m->setSavedValue<float>("cps", cps);
    }
};

inline State g;

$on_mod(Loaded) {
    g.load();
}

static void resetTimeScale() {
    auto sched = CCDirector::sharedDirector()->getScheduler();
    if (sched && sched->getTimeScale() != 1.f) sched->setTimeScale(1.f);
}

// ---------------------------------------------------------------
// Draggable floating button (tap = open menu, drag = move)
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
        resetTimeScale(); // never leave the whole game sped up after leaving a level
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
        return true;
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
    CCLabelBMFont* m_speedLabel = nullptr;
    CCLabelBMFont* m_cpsLabel = nullptr;

    bool init() {
        if (!Popup::init(340.f, 230.f)) return false;
        this->setTitle("Bot Menu");

        auto menu = CCMenu::create();
        menu->setPosition({170.f, 115.f});
        m_mainLayer->addChild(menu);

        // two columns of toggles
        this->addToggle(menu, "Noclip",        g.noclip,  -140.f, 70.f);
        this->addToggle(menu, "Noclip Stats",  g.counter,   20.f, 70.f);
        this->addToggle(menu, "Autoclicker",   g.clicker, -140.f, 40.f);
        this->addToggle(menu, "Infinite Jump", g.jump,      20.f, 40.f);
        this->addToggle(menu, "Layout Mode",   g.layout,  -140.f, 10.f);
        this->addToggle(menu, "Speedhack",     g.speedOn,   20.f, 10.f);

        // value steppers
        m_speedLabel = this->addStepper(menu, -30.f, [this](int dir) {
            g.speed = std::clamp(std::round((g.speed + dir * 0.1f) * 10.f) / 10.f, 0.1f, 5.f);
            g.save();
            m_speedLabel->setString(fmt::format("Speed: {:.1f}x", g.speed).c_str());
        });
        m_speedLabel->setString(fmt::format("Speed: {:.1f}x", g.speed).c_str());

        m_cpsLabel = this->addStepper(menu, -65.f, [this](int dir) {
            g.cps = std::clamp(std::round(g.cps + dir), 1.f, 30.f);
            g.save();
            m_cpsLabel->setString(fmt::format("Click CPS: {:.0f}", g.cps).c_str());
        });
        m_cpsLabel->setString(fmt::format("Click CPS: {:.0f}", g.cps).c_str());

        return true;
    }

    void addToggle(CCMenu* menu, char const* text, bool& ref, float x, float y) {
        auto on = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto off = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");

        bool* p = &ref; // g is a global, so this pointer stays valid
        auto toggler = CCMenuItemExt::createToggler(on, off, [p](CCMenuItemToggler* t) {
            *p = !t->isToggled(); // callback fires before the visual flips
            g.save();
        });
        toggler->setScale(0.6f);
        toggler->toggle(ref);
        toggler->setPosition({x, y});
        menu->addChild(toggler);

        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(0.42f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({x + 20.f, y});
        menu->addChild(label);
    }

    CCLabelBMFont* addStepper(CCMenu* menu, float y, std::function<void(int)> change) {
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(0.5f);
        label->setPosition({0.f, y});
        menu->addChild(label);

        auto leftSpr = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
        leftSpr->setFlipX(true);
        leftSpr->setScale(0.6f);
        auto left = CCMenuItemExt::createSpriteExtra(leftSpr, [change](CCMenuItemSpriteExtra*) {
            change(-1);
        });
        left->setPosition({-105.f, y});
        menu->addChild(left);

        auto rightSpr = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
        rightSpr->setScale(0.6f);
        auto right = CCMenuItemExt::createSpriteExtra(rightSpr, [change](CCMenuItemSpriteExtra*) {
            change(1);
        });
        right->setPosition({105.f, y});
        menu->addChild(right);

        return label;
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
// Layout Mode: decorations get hidden at the source.
// GD's object culling calls setVisible every frame and would undo
// a one-time hide, so we intercept it here instead.
// ---------------------------------------------------------------
class $modify(LayoutGameObject, GameObject) {
    void setVisible(bool v) {
        if (g.layout && m_objectType == GameObjectType::Decoration) v = false;
        GameObject::setVisible(v);
    }
};

// ---------------------------------------------------------------
// Infinite Jump
// ---------------------------------------------------------------
class $modify(JumpPlayer, PlayerObject) {
    bool pushButton(PlayerButton btn) {
        if (g.jump && btn == PlayerButton::Jump && PlayLayer::get()) {
            m_isOnGround = true;
        }
        return PlayerObject::pushButton(btn);
    }
};

// ---------------------------------------------------------------
// PlayLayer hook
// ---------------------------------------------------------------
class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        // layout
        bool prevLayout = false;
        // autoclicker
        float clickTimer = 0.f;
        float holdTimer = 0.f;
        bool holding = false;
        // noclip stats
        bool hitThisFrame = false;
        bool hitLastFrame = false;
        int hits = 0;
        int totalFrames = 0;
        int deadFrames = 0;
        int labelTick = 0;
        CCLabelBMFont* statsLabel = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto win = CCDirector::sharedDirector()->getWinSize();
        CCNode* parent = m_uiLayer ? static_cast<CCNode*>(m_uiLayer) : static_cast<CCNode*>(this);

        // menu button
        auto bg = extension::CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({45.f, 45.f});
        bg->setOpacity(110);

        if (auto icon = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png")) {
            icon->setScale(0.9f);
            icon->setPosition(bg->getContentSize() / 2);
            bg->addChild(icon);
        }

        if (auto btn = DraggableButton::create(bg, [] { BotMenu::create()->show(); })) {
            float x = Mod::get()->getSavedValue<float>("btn-x", 40.f);
            float y = Mod::get()->getSavedValue<float>("btn-y", win.height - 40.f);
            btn->setPosition({
                std::clamp(x, 25.f, win.width - 25.f),
                std::clamp(y, 25.f, win.height - 25.f)
            });
            parent->addChild(btn, 1000);
        }

        // noclip stats label
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(0.4f);
        label->setOpacity(170);
        label->setPosition({win.width / 2.f, win.height - 14.f});
        label->setVisible(false);
        parent->addChild(label, 999);
        m_fields->statsLabel = label;

        return true;
    }

    // ---- noclip ----
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g.noclip) {
            m_fields->hitThisFrame = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        f->clickTimer = 0.f;
        f->holdTimer = 0.f;
        f->holding = false;
        f->hits = 0;
        f->totalFrames = 0;
        f->deadFrames = 0;
        f->hitThisFrame = false;
        f->hitLastFrame = false;
        this->applyLayoutMode(g.layout);
    }

    void onQuit() {
        resetTimeScale();
        PlayLayer::onQuit();
    }

    // ---- per-frame ----
    void update(float dt) {
        // speedhack
        auto sched = CCDirector::sharedDirector()->getScheduler();
        float target = g.speedOn ? g.speed : 1.f;
        if (sched->getTimeScale() != target) sched->setTimeScale(target);

        PlayLayer::update(dt);

        if (g.clicker) this->tickAutoclicker(dt);
        this->tickNoclipStats();

        // layout mode
        if (g.layout != m_fields->prevLayout) {
            m_fields->prevLayout = g.layout;
            this->applyLayoutMode(g.layout);
        }
        if (g.layout) this->forceFlatColors();
    }

    void tickNoclipStats() {
        auto f = m_fields.self();
        if (!f->statsLabel) return;

        bool show = g.noclip && g.counter;
        f->statsLabel->setVisible(show);

        if (g.noclip) {
            f->totalFrames++;
            if (f->hitThisFrame) {
                f->deadFrames++;
                if (!f->hitLastFrame) f->hits++; // new hit, not the same one continuing
            }
        }
        f->hitLastFrame = f->hitThisFrame;
        f->hitThisFrame = false;

        if (show && ++f->labelTick >= 6) {
            f->labelTick = 0;
            float acc = f->totalFrames > 0
                ? 100.f * (1.f - static_cast<float>(f->deadFrames) / f->totalFrames)
                : 100.f;
            f->statsLabel->setString(fmt::format("Hits: {}   Acc: {:.1f}%", f->hits, acc).c_str());
        }
    }

    void tickAutoclicker(float dt) {
        if (!m_player1 || m_player1->m_isDead) return;
        auto f = m_fields.self();
        float interval = 1.f / g.cps;
        float hold = std::min(0.04f, interval * 0.5f);

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
        if (f->clickTimer >= interval) {
            f->clickTimer = 0.f;
            f->holding = true;
            f->holdTimer = hold;
            m_player1->pushButton(PlayerButton::Jump);
            if (m_gameState.m_isDualMode && m_player2) {
                m_player2->pushButton(PlayerButton::Jump);
            }
        }
    }

    // ---- layout mode ----
    void applyLayoutMode(bool enable) {
        if (m_objects) {
            for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
                if (obj && obj->m_objectType == GameObjectType::Decoration) {
                    // our setVisible hook keeps them hidden while enabled;
                    // when disabled, culling re-shows whatever is on screen
                    obj->setVisible(!enable);
                }
            }
        }
        if (!enable) {
            if (m_background) m_background->setColor({255, 255, 255});
            if (m_groundLayer) {
                m_groundLayer->updateGround01Color({255, 255, 255});
                m_groundLayer->updateGround02Color({255, 255, 255});
            }
        }
    }

    // color triggers would otherwise recolor the flat layout look
    void forceFlatColors() {
        if (m_background) m_background->setColor({40, 62, 255});
        if (m_groundLayer) {
            m_groundLayer->updateGround01Color({25, 40, 170});
            m_groundLayer->updateGround02Color({25, 40, 170});
        }
    }
};
