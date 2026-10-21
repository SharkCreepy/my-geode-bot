#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/ui/Popup.hpp>
#include <functional>
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>

using namespace geode::prelude;

// ===============================================================
// Shared state (cached in memory, saved to disk when changed)
// ===============================================================
#if defined(GEODE_IS_ANDROID) || defined(GEODE_IS_IOS)
constexpr float DEFAULT_HUD_SCALE = 1.3f;   // bigger text on phones/tablets
#else
constexpr float DEFAULT_HUD_SCALE = 1.0f;
#endif

struct State {
    bool noclip = false;
    bool counter = false;
    bool jump = false;
    bool clicker = false;
    bool layout = false;
    bool speedOn = false;
    bool trajectory = false;
    bool bot = false;
    float speed = 1.f;
    float cps = 10.f;
    float hudScale = DEFAULT_HUD_SCALE;

    // not saved
    bool hudEdit = false;
    bool hudResetRequested = false;

    void load() {
        auto m = Mod::get();
        noclip     = m->getSavedValue<bool>("noclip", false);
        counter    = m->getSavedValue<bool>("counter", false);
        jump       = m->getSavedValue<bool>("jump", false);
        clicker    = m->getSavedValue<bool>("clicker", false);
        layout     = m->getSavedValue<bool>("layout", false);
        speedOn    = m->getSavedValue<bool>("speedOn", false);
        trajectory = m->getSavedValue<bool>("trajectory", false);
        bot        = m->getSavedValue<bool>("bot", false);
        speed      = m->getSavedValue<float>("speed", 1.f);
        cps        = m->getSavedValue<float>("cps", 10.f);
        hudScale   = m->getSavedValue<float>("hudScale", DEFAULT_HUD_SCALE);
    }

    void save() {
        auto m = Mod::get();
        m->setSavedValue<bool>("noclip", noclip);
        m->setSavedValue<bool>("counter", counter);
        m->setSavedValue<bool>("jump", jump);
        m->setSavedValue<bool>("clicker", clicker);
        m->setSavedValue<bool>("layout", layout);
        m->setSavedValue<bool>("speedOn", speedOn);
        m->setSavedValue<bool>("trajectory", trajectory);
        m->setSavedValue<bool>("bot", bot);
        m->setSavedValue<float>("speed", speed);
        m->setSavedValue<float>("cps", cps);
        m->setSavedValue<float>("hudScale", hudScale);
    }
};

inline State g;

$on_mod(Loaded) {
    g.load();
}

// ===============================================================
// Speedhack: scale the scheduler's delta time.
// (CCScheduler::setTimeScale gets overridden by the game on some
// platforms; scaling dt directly works on desktop + mobile.)
// ===============================================================
class $modify(SpeedScheduler, CCScheduler) {
    void update(float dt) {
        if (g.speedOn && PlayLayer::get()) dt *= g.speed;
        CCScheduler::update(dt);
    }
};

// ===============================================================
// Simplified cube physics simulator (used by trajectory + bot)
//
// Only models: cube, flat ground, solid blocks, hazards.
// Does NOT model: orbs, pads, portals, ship/ball/ufo/wave/etc,
// slopes, moving objects, gravity flips, mini size.
// ===============================================================
namespace sim {
    struct Box { float x0, y0, x1, y1; bool hazard; };

    inline std::vector<Box> boxes;
    inline float maxW = 30.f;
    inline float floorY = 90.f;

    constexpr float HALF = 15.f;       // cube half size (solid collisions)
    constexpr float HAZ_HALF = 11.5f;  // cube hazard hitbox half size (slightly conservative)
    inline float jumpVel = 11.180032f; // auto-calibrated from your real jumps
    inline float grav = 0.958199f;
    constexpr float TERMINAL = -15.f;
    constexpr int SUB = 2;             // substeps per 60fps frame

    struct S { float x, y, vy; bool ground; bool dead; };

    inline bool overlap(float x, float y, float h, Box const& b) {
        return x + h > b.x0 && x - h < b.x1 && y + h > b.y0 && y - h < b.y1;
    }

    template <class F>
    inline void forEachNear(float x, F&& fn) {
        auto it = std::lower_bound(boxes.begin(), boxes.end(), x - HALF - maxW,
            [](Box const& b, float v) { return b.x0 < v; });
        for (; it != boxes.end() && it->x0 <= x + HALF; ++it) fn(*it);
    }

    // advance one 60fps frame. vx = horizontal units per 60fps frame
    inline void step(S& s, float vx, bool jump) {
        if (s.dead) return;
        if (jump && s.ground) { s.vy = jumpVel; s.ground = false; }

        constexpr float dt = 1.f / SUB;
        for (int i = 0; i < SUB && !s.dead; i++) {
            // ---- vertical ----
            float prevBottom = s.y - HALF;
            float prevTop = s.y + HALF;
            s.vy = std::max(s.vy - grav * dt, TERMINAL);
            s.y += s.vy * dt;
            s.ground = false;

            if (s.y - HALF <= floorY) {
                s.y = floorY + HALF;
                if (s.vy < 0.f) s.vy = 0.f;
                s.ground = true;
            }

            forEachNear(s.x, [&](Box const& b) {
                if (b.hazard || s.dead) return;
                if (!overlap(s.x, s.y, HALF, b)) return;
                if (s.vy <= 0.f && prevBottom >= b.y1 - 1.f) {
                    s.y = b.y1 + HALF;
                    s.vy = 0.f;
                    s.ground = true;
                } else if (s.vy > 0.f && prevTop <= b.y0 + 1.f) {
                    s.dead = true; // hit the underside
                }
            });
            if (s.dead) break;

            // ---- horizontal ----
            s.x += vx * dt;
            forEachNear(s.x, [&](Box const& b) {
                if (s.dead) return;
                if (b.hazard) {
                    if (overlap(s.x, s.y, HAZ_HALF, b)) s.dead = true;
                    return;
                }
                if (overlap(s.x, s.y, HALF, b)) {
                    if (s.y - HALF >= b.y1 - 2.f) {
                        s.y = b.y1 + HALF; s.vy = 0.f; s.ground = true;
                    } else {
                        s.dead = true; // hit the side of a block
                    }
                }
            });
        }
    }

    inline int horizon(float vx) {
        return std::clamp(static_cast<int>(450.f / std::max(vx, 1.f)), 40, 110);
    }

    // jump arc on flat ground with the given constants -> apex height + airtime (60fps frames)
    inline void flatJump(float v0, float g, float& H, float& T) {
        float y = 0.f, vy = v0;
        H = 0.f; T = 200.f;
        constexpr float dt = 1.f / SUB;
        for (int i = 0; i < SUB * 200; i++) {
            vy = std::max(vy - g * dt, TERMINAL);
            y += vy * dt;
            H = std::max(H, y);
            if (y <= 0.f && vy < 0.f) { T = (i + 1) * dt; return; }
        }
    }

    // Feed in a MEASURED flat jump (apex height in units, airtime in frames).
    // Nudges jumpVel/grav so the simulator matches the real game.
    inline bool calibrate(float Hm, float Tm) {
        float Hs, Ts;
        flatJump(jumpVel, grav, Hs, Ts);
        if (Hs <= 0.f || Ts <= 0.f || Hm <= 0.f || Tm <= 0.f) return false;
        float rH = Hm / Hs, rT = Tm / Ts;
        if (rH < 0.75f || rH > 1.3f || rT < 0.75f || rT > 1.3f) return false;
        float nv = jumpVel * rH / rT;
        float ng = grav * rH / (rT * rT);
        jumpVel = std::clamp(jumpVel + (nv - jumpVel) * 0.5f, 7.f, 16.f);
        grav = std::clamp(grav + (ng - grav) * 0.5f, 0.5f, 1.5f);
        return true;
    }

    // Lookahead search: does ANY sequence of <= jumpsLeft jumps survive until frame H?
    struct Planner {
        int H = 70;                      // lookahead frames
        float vx = 5.19f;
        int budget = 0;

        bool survive(S s, int frame, int jumpsLeft) {
            while (frame < H) {
                if (--budget <= 0) return true; // out of budget: assume ok
                if (s.ground && jumpsLeft > 0 && (frame & 1) == 0) {
                    S j = s;
                    step(j, vx, true);
                    if (!j.dead && survive(j, frame + 1, jumpsLeft - 1)) return true;
                }
                step(s, vx, false);
                if (s.dead) return false;
                frame++;
            }
            return true;
        }
    };
}

$on_mod(Loaded) {
    sim::jumpVel = std::clamp(Mod::get()->getSavedValue<float>("jumpVel", sim::jumpVel), 7.f, 16.f);
    sim::grav = std::clamp(Mod::get()->getSavedValue<float>("grav", sim::grav), 0.5f, 1.5f);
}

// ===============================================================
// Draggable node (tap = callback, drag = move, position saved)
// ===============================================================
class DraggableNode : public CCNode, public CCTouchDelegate {
protected:
    std::string m_keyX, m_keyY;
    std::function<void()> m_onTap;
    std::function<bool()> m_canDrag; // empty = always allowed
    CCNode* m_scaleTarget = nullptr;
    bool m_dragging = false;
    bool m_touching = false;
    CCPoint m_nodeStart, m_touchStart;

    bool init(CCSize size, std::string kx, std::string ky,
              std::function<bool()> canDrag, std::function<void()> onTap) {
        if (!CCNode::init()) return false;
        m_keyX = std::move(kx);
        m_keyY = std::move(ky);
        m_canDrag = std::move(canDrag);
        m_onTap = std::move(onTap);
        this->setContentSize(size);
        this->setAnchorPoint({0.5f, 0.5f});
        return true;
    }

    bool isUsable() {
        for (CCNode* n = this; n; n = n->getParent()) {
            if (!n->isVisible()) return false;
        }
        return true;
    }

public:
    static DraggableNode* create(CCSize size, std::string kx, std::string ky,
                                 std::function<bool()> canDrag, std::function<void()> onTap) {
        auto ret = new DraggableNode();
        if (ret->init(size, std::move(kx), std::move(ky), std::move(canDrag), std::move(onTap))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void setVisualScaleTarget(CCNode* n) { m_scaleTarget = n; }

    void restorePosition(CCPoint def) {
        auto win = CCDirector::sharedDirector()->getWinSize();
        float x = Mod::get()->getSavedValue<float>(m_keyX, def.x);
        float y = Mod::get()->getSavedValue<float>(m_keyY, def.y);
        this->setPosition({
            std::clamp(x, 20.f, win.width - 20.f),
            std::clamp(y, 20.f, win.height - 20.f)
        });
    }

    void resetPosition(CCPoint def) {
        Mod::get()->setSavedValue<float>(m_keyX, def.x);
        Mod::get()->setSavedValue<float>(m_keyY, def.y);
        this->setPosition(def);
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
        if (m_canDrag && !m_canDrag()) return false; // let the touch through to the game

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
        if (m_scaleTarget) m_scaleTarget->setScale(0.92f);
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
            p.x = std::clamp(p.x, 20.f, win.width - 20.f);
            p.y = std::clamp(p.y, 20.f, win.height - 20.f);
            this->setPosition(p);
        }
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        if (!m_touching) return;
        m_touching = false;
        if (m_scaleTarget) m_scaleTarget->setScale(1.f);

        if (m_dragging) {
            m_dragging = false;
            auto p = this->getPosition();
            Mod::get()->setSavedValue<float>(m_keyX, p.x);
            Mod::get()->setSavedValue<float>(m_keyY, p.y);
        } else if (m_onTap) {
            m_onTap();
        }
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_touching = false;
        m_dragging = false;
        if (m_scaleTarget) m_scaleTarget->setScale(1.f);
    }
};

// ===============================================================
// Menu popup
// ===============================================================
class BotMenu : public Popup {
protected:
    CCLabelBMFont* m_speedLabel = nullptr;
    CCLabelBMFont* m_cpsLabel = nullptr;
    CCLabelBMFont* m_hudLabel = nullptr;

    bool init() {
        if (!Popup::init(340.f, 290.f)) return false;
        this->setTitle("Bot Menu");

        auto menu = CCMenu::create();
        menu->setPosition({170.f, 145.f});
        m_mainLayer->addChild(menu);

        constexpr float L = -150.f, R = 15.f;
        this->addToggle(menu, "Noclip",        g.noclip,     L, 100.f);
        this->addToggle(menu, "Noclip Stats",  g.counter,    R, 100.f);
        this->addToggle(menu, "Autoclicker",   g.clicker,    L, 72.f);
        this->addToggle(menu, "Infinite Jump", g.jump,       R, 72.f);
        this->addToggle(menu, "Layout Mode",   g.layout,     L, 44.f);
        this->addToggle(menu, "Speedhack",     g.speedOn,    R, 44.f);
        this->addToggle(menu, "Trajectory",    g.trajectory, L, 16.f);
        this->addToggle(menu, "Auto Bot",      g.bot,        R, 16.f);
        this->addToggle(menu, "Move HUD",      g.hudEdit,    L, -12.f);

        auto resetSpr = ButtonSprite::create("Reset HUD", 80, true, "bigFont.fnt", "GJ_button_04.png", 24.f, 0.5f);
        auto resetBtn = CCMenuItemExt::createSpriteExtra(resetSpr, [](CCMenuItemSpriteExtra*) {
            g.hudResetRequested = true;
        });
        resetBtn->setPosition({R + 55.f, -12.f});
        menu->addChild(resetBtn);

        m_speedLabel = this->addStepper(menu, -45.f, [this](int dir) {
            g.speed = std::clamp(std::round((g.speed + dir * 0.1f) * 10.f) / 10.f, 0.1f, 5.f);
            g.save();
            m_speedLabel->setString(fmt::format("Speed: {:.1f}x", g.speed).c_str());
        });
        m_speedLabel->setString(fmt::format("Speed: {:.1f}x", g.speed).c_str());

        m_cpsLabel = this->addStepper(menu, -75.f, [this](int dir) {
            g.cps = std::clamp(std::round(g.cps + dir), 1.f, 30.f);
            g.save();
            m_cpsLabel->setString(fmt::format("Click CPS: {:.0f}", g.cps).c_str());
        });
        m_cpsLabel->setString(fmt::format("Click CPS: {:.0f}", g.cps).c_str());

        m_hudLabel = this->addStepper(menu, -105.f, [this](int dir) {
            g.hudScale = std::clamp(std::round((g.hudScale + dir * 0.1f) * 10.f) / 10.f, 0.5f, 2.5f);
            g.save();
            m_hudLabel->setString(fmt::format("HUD Size: {:.1f}x", g.hudScale).c_str());
        });
        m_hudLabel->setString(fmt::format("HUD Size: {:.1f}x", g.hudScale).c_str());

        return true;
    }

    void addToggle(CCMenu* menu, char const* text, bool& ref, float x, float y) {
        auto on = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto off = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");

        bool* p = &ref;
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
        auto left = CCMenuItemExt::createSpriteExtra(leftSpr, [change](CCMenuItemSpriteExtra*) { change(-1); });
        left->setPosition({-105.f, y});
        menu->addChild(left);

        auto rightSpr = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
        rightSpr->setScale(0.6f);
        auto right = CCMenuItemExt::createSpriteExtra(rightSpr, [change](CCMenuItemSpriteExtra*) { change(1); });
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

// ===============================================================
// Layout Mode: hide decorations at the source
// ===============================================================
class $modify(LayoutGameObject, GameObject) {
    void setVisible(bool v) {
        if (g.layout && m_objectType == GameObjectType::Decoration) v = false;
        GameObject::setVisible(v);
    }
};

// ===============================================================
// Infinite Jump
// ===============================================================
class $modify(JumpPlayer, PlayerObject) {
    bool pushButton(PlayerButton btn) {
        if (g.jump && btn == PlayerButton::Jump && PlayLayer::get()) {
            m_isOnGround = true;
        }
        return PlayerObject::pushButton(btn);
    }
};

// ===============================================================
// Ticker: our own per-frame callback.
// Hooking PlayLayer::update is unreliable (especially on mobile),
// so a plain scheduled node drives everything instead.
// ===============================================================
class Ticker : public CCNode {
protected:
    std::function<void(float)> m_fn;

public:
    static Ticker* create(std::function<void(float)> fn) {
        auto ret = new Ticker();
        if (ret->init()) {
            ret->m_fn = std::move(fn);
            ret->autorelease();
            ret->scheduleUpdate();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void update(float dt) override {
        if (m_fn) m_fn(dt);
    }
};

// ===============================================================
// PlayLayer hook
// ===============================================================
class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        bool prevLayout = false;

        // autoclicker
        float clickTimer = 0.f;
        float holdTimer = 0.f;
        bool holding = false;

        // noclip stats
        // (counted per physics step when the step hook works, per frame otherwise)
        bool hitFlag = false;
        bool stepHookSeen = false;
        int stepsSinceHit = 1000;
        int deaths = 0;
        long totalSteps = 0;
        long deadSteps = 0;
        int labelTick = 0;

        // jump-arc measurement for auto-calibration
        bool arcActive = false;
        bool arcWasGround = true;
        float arcStartX = 0.f, arcStartY = 0.f, arcApex = 0.f;
        float arcPrevX = 0.f, arcPrevY = 0.f;

        // HUD
        DraggableNode* hud = nullptr;
        CCLabelBMFont* hudLabel = nullptr;
        extension::CCScale9Sprite* hudBg = nullptr;

        // physics measurement (60fps-frame units)
        bool haveLast = false;
        float lastX = 0.f, lastY = 0.f;
        float vx = 5.193f;   // 311.58 u/s at 1x speed
        float vy = 0.f;

        // trajectory + bot
        CCDrawNode* traj = nullptr;
        int botRelease = 0;
        bool havePlan = false;
        float planX = 0.f;
        bool botHolding = false;
        bool geometryBuilt = false;
        CCLabelBMFont* status = nullptr;
    };

    // ---------- setup ----------
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto win = CCDirector::sharedDirector()->getWinSize();
        CCNode* ui = m_uiLayer ? static_cast<CCNode*>(m_uiLayer) : static_cast<CCNode*>(this);

        // ---- menu button ----
        auto bg = extension::CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({45.f, 45.f});
        bg->setOpacity(110);
        if (auto icon = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png")) {
            icon->setScale(0.9f);
            icon->setPosition(bg->getContentSize() / 2);
            bg->addChild(icon);
        }
        if (auto btn = DraggableNode::create({45.f, 45.f}, "btn-x", "btn-y", nullptr,
                                             [] { BotMenu::create()->show(); })) {
            bg->setPosition({22.5f, 22.5f});
            btn->addChild(bg);
            btn->setVisualScaleTarget(bg);
            btn->restorePosition({40.f, win.height - 40.f});
            ui->addChild(btn, 1000);
        }

        // ---- noclip stats HUD (movable, scalable, for any screen) ----
        if (auto hud = DraggableNode::create({170.f, 26.f}, "hud-x", "hud-y",
                                             [] { return g.hudEdit; }, nullptr)) {
            auto hbg = extension::CCScale9Sprite::create("square02_small.png");
            hbg->setContentSize({170.f, 26.f});
            hbg->setOpacity(120);
            hbg->setPosition({85.f, 13.f});
            hud->addChild(hbg);

            auto label = CCLabelBMFont::create("Deaths: 0   Acc: 100.0%", "bigFont.fnt");
            label->setScale(0.38f);
            label->setPosition({85.f, 13.f});
            hud->addChild(label);

            hud->restorePosition({win.width / 2.f, win.height - 48.f});
            hud->setScale(g.hudScale);
            hud->setVisible(false);
            ui->addChild(hud, 999);

            m_fields->hud = hud;
            m_fields->hudLabel = label;
            m_fields->hudBg = hbg;
        }

        // ---- trajectory layer (scrolls with the level) ----
        if (m_objectLayer) {
            auto node = CCDrawNode::create();
            m_objectLayer->addChild(node, 1000);
            m_fields->traj = node;
        }

        // ---- small status line (tells you why bot/trajectory might be idle) ----
        auto status = CCLabelBMFont::create("", "bigFont.fnt");
        status->setScale(0.35f);
        status->setOpacity(200);
        status->setPosition({win.width / 2.f, 14.f});
        status->setVisible(false);
        ui->addChild(status, 999);
        m_fields->status = status;

        // ---- per-frame driver ----
        if (auto ticker = Ticker::create([this](float dt) { this->tickAll(dt); })) {
            this->addChild(ticker);
        }

        return true;
    }

    void buildGeometry() {
        sim::boxes.clear();
        sim::maxW = 30.f;
        sim::floorY = 90.f;
        if (!m_objects) return;

        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            bool hazard = obj->m_objectType == GameObjectType::Hazard;
            bool solid = obj->m_objectType == GameObjectType::Solid;
            if (!hazard && !solid) continue;

            auto r = obj->getObjectRect();
            if (r.size.width <= 0.f || r.size.height <= 0.f) continue;

            sim::boxes.push_back({
                r.origin.x, r.origin.y,
                r.origin.x + r.size.width, r.origin.y + r.size.height,
                hazard
            });
            sim::maxW = std::max(sim::maxW, r.size.width);
        }
        std::sort(sim::boxes.begin(), sim::boxes.end(),
                  [](sim::Box const& a, sim::Box const& b) { return a.x0 < b.x0; });
    }

    // ---------- input helper (goes through the game's real input path) ----------
    void pressJump(bool down) {
        this->handleButton(down, 1, true);
        if (m_gameState.m_isDualMode && m_player2) {
            this->handleButton(down, 1, false);
        }
    }

    // ---------- noclip ----------
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g.noclip) {
            m_fields->hitFlag = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        if (f->holding || f->botHolding) this->pressJump(false);
        f->clickTimer = 0.f;
        f->holdTimer = 0.f;
        f->holding = false;
        f->botHolding = false;
        f->botRelease = 0;
        f->havePlan = false;
        f->deaths = 0;
        f->totalSteps = 0;
        f->deadSteps = 0;
        f->stepsSinceHit = 1000;
        f->hitFlag = false;
        f->arcActive = false;
        f->haveLast = false;
        this->applyLayoutMode(g.layout);
    }

    // ---------- per-frame ----------
    void tickAll(float dt) {
        auto f = m_fields.self();

        // build level geometry once the objects exist
        if (!f->geometryBuilt && m_objects && m_objects->count() > 0) {
            f->geometryBuilt = true;
            this->buildGeometry();
        }

        this->updateHud();
        this->tickNoclipStats();
        this->measure(dt);

        // release any held input if the features were switched off
        if (!g.clicker && f->holding) { f->holding = false; this->pressJump(false); }
        if (!g.bot && f->botHolding) { f->botHolding = false; f->botRelease = 0; this->pressJump(false); }

        if (g.bot) this->tickBot();
        else if (g.clicker) this->tickAutoclicker(dt);

        this->drawTrajectory();
        this->updateStatus();

        if (g.layout != f->prevLayout) {
            f->prevLayout = g.layout;
            this->applyLayoutMode(g.layout);
        }
        if (g.layout) this->forceFlatColors();
    }

    // ---------- status line ----------
    void updateStatus() {
        auto f = m_fields.self();
        if (!f->status) return;

        bool show = g.bot || g.trajectory;
        f->status->setVisible(show);
        if (!show) return;

        std::string msg;
        if (!m_player1) msg = "no player";
        else if (!this->cubeMode()) msg = "Not in cube mode (bot + trajectory are cube only)";
        else if (sim::boxes.empty()) msg = "No level geometry found";
        else msg = fmt::format("{} objects | {} | jump {:.2f}/{:.3f}", sim::boxes.size(),
                               g.bot ? "bot active" : "trajectory on", sim::jumpVel, sim::grav);
        f->status->setString(msg.c_str());
    }

    // ---------- HUD ----------
    void updateHud() {
        auto f = m_fields.self();
        if (!f->hud) return;

        auto win = CCDirector::sharedDirector()->getWinSize();
        if (g.hudResetRequested) {
            g.hudResetRequested = false;
            g.hudScale = DEFAULT_HUD_SCALE;
            g.save();
            f->hud->resetPosition({win.width / 2.f, win.height - 48.f});
        }

        f->hud->setScale(g.hudScale);
        bool show = g.counter || g.hudEdit;
        f->hud->setVisible(show);

        // yellow tint while in "Move HUD" mode so it's obvious it can be dragged
        if (f->hudBg) f->hudBg->setColor(g.hudEdit ? ccColor3B{255, 220, 60} : ccColor3B{255, 255, 255});
    }

    // One call per physics step (or per frame as a fallback).
    // A "death" = one continuous contact with a spike/block. Accuracy = share of
    // steps NOT spent inside something deadly.
    void registerStep(bool fromStepHook) {
        auto f = m_fields.self();
        bool hit = f->hitFlag;
        f->hitFlag = false;

        if (!g.noclip) {
            f->stepsSinceHit = 1000;
            return;
        }

        int gap = fromStepHook ? 6 : 2; // tiny gaps inside one contact don't count as a new death
        f->totalSteps++;
        if (hit) {
            if (f->stepsSinceHit <= gap) {
                f->deadSteps += f->stepsSinceHit;
            } else {
                f->deaths++;
            }
            f->deadSteps++;
            f->stepsSinceHit = 0;
        } else if (f->stepsSinceHit < 1000) {
            f->stepsSinceHit++;
        }
    }

    // called from the PlayerObject::update hook (once per physics step)
    void onPhysicsStep() {
        auto f = m_fields.self();
        f->stepHookSeen = true;
        this->registerStep(true);
        this->sampleArc();
    }

    // Measures real jumps (flat ground -> same height) to calibrate the simulator
    void sampleArc() {
        auto f = m_fields.self();
        if (!m_player1) return;
        float x = m_player1->getPositionX();
        float y = m_player1->getPositionY();
        bool ground = m_player1->m_isOnGround;

        if (!this->cubeMode()) {
            f->arcActive = false;
        } else if (!f->arcActive) {
            if (f->arcWasGround && !ground && y > f->arcPrevY + 0.01f) {
                f->arcActive = true;
                f->arcStartX = f->arcPrevX;
                f->arcStartY = f->arcPrevY;
                f->arcApex = y - f->arcStartY;
            }
        } else {
            f->arcApex = std::max(f->arcApex, y - f->arcStartY);
            float dx = x - f->arcStartX;
            if (dx < 0.f || dx > f->vx * 70.f) {
                f->arcActive = false;               // teleport / too long: ignore
            } else if (ground) {
                f->arcActive = false;
                float dy = y - f->arcStartY;
                if (std::abs(dy) < 0.5f && f->vx > 1.f) {
                    float airFrames = dx / f->vx;
                    if (sim::calibrate(f->arcApex, airFrames)) {
                        Mod::get()->setSavedValue<float>("jumpVel", sim::jumpVel);
                        Mod::get()->setSavedValue<float>("grav", sim::grav);
                    }
                }
            }
        }

        f->arcWasGround = ground;
        f->arcPrevX = x;
        f->arcPrevY = y;
    }

    void tickNoclipStats() {
        auto f = m_fields.self();

        if (!f->stepHookSeen) {      // fallback if the physics-step hook isn't firing
            this->registerStep(false);
            this->sampleArc();
        }

        if (f->hudLabel && ++f->labelTick >= 6) {
            f->labelTick = 0;
            float acc = f->totalSteps > 0
                ? 100.f * (1.f - static_cast<float>(f->deadSteps) / static_cast<float>(f->totalSteps))
                : 100.f;
            f->hudLabel->setString(fmt::format("Deaths: {}   Acc: {:.1f}%", f->deaths, acc).c_str());
            float w = f->hudLabel->getContentSize().width;
            float sc = std::min(0.38f, 160.f / std::max(w, 1.f));
            f->hudLabel->setScale(sc);
        }
    }

    // ---------- autoclicker ----------
    void tickAutoclicker(float dt) {
        if (!m_player1 || m_player1->m_isDead) return;
        auto f = m_fields.self();
        float interval = 1.f / g.cps;
        float hold = std::min(0.04f, interval * 0.5f);

        if (f->holding) {
            f->holdTimer -= dt;
            if (f->holdTimer <= 0.f) {
                f->holding = false;
                this->pressJump(false);
            }
            return;
        }

        f->clickTimer += dt;
        if (f->clickTimer >= interval) {
            f->clickTimer = 0.f;
            f->holding = true;
            f->holdTimer = hold;
            this->pressJump(true);
        }
    }

    // ---------- physics measurement ----------
    void measure(float dt) {
        if (!m_player1) return;
        auto f = m_fields.self();
        float x = m_player1->getPositionX();
        float y = m_player1->getPositionY();

        if (f->haveLast && dt > 0.f) {
            float k = 1.f / (dt * 60.f);
            float dx = (x - f->lastX) * k;
            float dy = (y - f->lastY) * k;
            if (dx > 0.5f && dx < 20.f) f->vx = dx;     // ignore teleports/respawns
            f->vy = std::clamp(dy, -30.f, 30.f);
        }
        f->lastX = x;
        f->lastY = y;
        f->haveLast = true;
    }

    bool cubeMode() {
        auto p = m_player1;
        return p && !p->m_isDead && !p->m_isShip && !p->m_isBird && !p->m_isBall &&
               !p->m_isDart && !p->m_isRobot && !p->m_isSpider && !p->m_isSwing &&
               !p->m_isUpsideDown;
    }

    sim::S currentSimState() {
        auto f = m_fields.self();
        sim::S s;
        s.x = m_player1->getPositionX();
        s.y = m_player1->getPositionY();
        s.ground = m_player1->m_isOnGround;
        s.vy = s.ground ? 0.f : f->vy;
        s.dead = false;

        // learn the floor height: if we're grounded and not on a block, we're on the floor
        if (s.ground) {
            bool onBlock = false;
            float bottom = s.y - sim::HALF;
            sim::forEachNear(s.x, [&](sim::Box const& b) {
                if (!b.hazard && std::abs(b.y1 - bottom) < 1.5f &&
                    s.x + sim::HALF > b.x0 && s.x - sim::HALF < b.x1) onBlock = true;
            });
            if (!onBlock && bottom > 84.f && bottom < 96.f) sim::floorY = bottom;
        }
        return s;
    }

    // ---------- trajectory ----------
    void drawTrajectory() {
        auto f = m_fields.self();
        if (!f->traj) return;
        f->traj->clear();
        if (!g.trajectory || !this->cubeMode()) return;

        auto start = this->currentSimState();

        auto drawPath = [&](bool firstJump, ccColor4F col) {
            auto s = start;
            auto prev = CCPoint{s.x, s.y};
            int H = sim::horizon(f->vx);
            for (int i = 0; i < H; i++) {
                sim::step(s, f->vx, firstJump && i == 0);
                CCPoint p{s.x, s.y};
                f->traj->drawSegment(prev, p, 1.2f, col);
                prev = p;
                if (s.dead) {
                    f->traj->drawDot(p, 4.f, ccc4f(1.f, 0.f, 0.f, 1.f));
                    break;
                }
            }
        };

        drawPath(false, ccc4f(1.f, 0.4f, 0.4f, 0.9f));                          // if you do nothing
        if (start.ground) drawPath(true, ccc4f(0.3f, 1.f, 0.4f, 0.9f));         // if you jump now
    }

    // ---------- bot ----------
    // Looks ahead with the simulator, finds the window of frames in which a jump
    // survives, and jumps in the middle of that window.
    void tickBot() {
        auto f = m_fields.self();

        // release after the tap
        if (f->botRelease > 0 && --f->botRelease == 0) {
            f->botHolding = false;
            this->pressJump(false);
        }

        if (!this->cubeMode() || sim::boxes.empty()) return;
        if (f->botHolding) return;

        auto s = this->currentSimState();
        if (!s.ground) return;

        sim::Planner p;
        p.vx = f->vx;
        p.H = sim::horizon(p.vx);

        // forget a stale plan (we've already driven past it)
        if (f->havePlan && f->planX < s.x - 2.f * p.vx) f->havePlan = false;

        // 1) Can we just keep going without jumping?
        p.budget = 30000;
        if (p.survive(s, 0, 0)) {
            f->havePlan = false;
            return;
        }

        // 2) Find the contiguous window of "jump now"-frames that survive
        int first = -1, last = -1;
        auto w = s;
        p.budget = 80000;
        for (int k = 0; k < 50; k++) {
            auto j = w;
            sim::step(j, p.vx, true);
            bool ok = !j.dead && p.survive(j, 1, 1);
            if (ok) {
                if (first < 0) first = k;
                last = k;
            } else if (first >= 0) {
                break;
            }
            sim::step(w, p.vx, false);
            if (w.dead || !w.ground) break;
        }

        // 3) Aim for the CENTER of the window (measured in x, so speed changes don't matter)
        bool doJump = false;
        if (first < 0) {
            doJump = true; // no safe window found: try anyway
        } else {
            float centerX = s.x + p.vx * (first + last) * 0.5f;
            if (first > 0) {
                f->havePlan = true;       // window still ahead: keep refining the target
                f->planX = centerX;
            } else {
                if (!f->havePlan) {       // window already open and we never saw it coming
                    f->havePlan = true;
                    f->planX = centerX;
                }
                if (s.x + p.vx * 0.5f >= f->planX || last <= 1) doJump = true;
            }
        }

        if (doJump) {
            f->havePlan = false;
            f->botHolding = true;
            f->botRelease = 2;
            this->pressJump(true);
        }
    }

    // ---------- layout mode ----------
    void applyLayoutMode(bool enable) {
        if (m_objects) {
            for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
                if (obj && obj->m_objectType == GameObjectType::Decoration) {
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

    void forceFlatColors() {
        if (m_background) m_background->setColor({40, 62, 255});
        if (m_groundLayer) {
            m_groundLayer->updateGround01Color({25, 40, 170});
            m_groundLayer->updateGround02Color({25, 40, 170});
        }
    }
};


// ===============================================================
// Per-physics-step hook (240 steps/s on most setups). Used for exact
// noclip stats and to measure real jumps for auto-calibration.
// ===============================================================
class $modify(StepPlayer, PlayerObject) {
    void update(float dt) {
        if (auto pl = PlayLayer::get()) {
            if (pl->m_player1 == this) {
                static_cast<BotPlayLayer*>(pl)->onPhysicsStep();
            }
        }
        PlayerObject::update(dt);
    }
};
