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
constexpr float DEFAULT_HUD_SCALE = 1.3f;
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
// Speedhack
// ===============================================================
class $modify(SpeedScheduler, CCScheduler) {
    void update(float dt) {
        if (g.speedOn && PlayLayer::get()) dt *= g.speed;
        CCScheduler::update(dt);
    }
};

// ===============================================================
// Simplified cube physics simulator
// ===============================================================
namespace sim {
    enum Kind : int {
        Solid = 0, Hazard,
        PortalNormal, PortalInverse,
        PadYellow, PadPink, PadRed, PadBlue,
        OrbYellow, OrbPink, OrbRed, OrbBlue, OrbGreen,
        KindCount
    };

    struct Box { float x0, y0, x1, y1; int kind; };

    inline std::vector<Box> boxes;
    inline std::vector<Box> wide;
    inline float maxW = 30.f;
    inline float floorY = 90.f;
    inline float ceilY = 390.f;
    inline int nSolid = 0, nHazard = 0, nPad = 0, nOrb = 0, nPortal = 0;

    constexpr float HALF = 15.f;
    constexpr float HAZ_HALF = 11.5f;
    constexpr float ORB_HALF = 12.f;
    constexpr float TERMINAL = -15.f;
    constexpr int SUB = 2;

    inline float jumpVel = 11.180032f;
    inline float grav = 0.958199f;
    inline float portalMul = 0.5f;

    inline float launch[KindCount] = {
        0.f, 0.f, 0.f, 0.f,
        1.43f, 0.93f, 1.79f, -0.9f,
        1.00f, 0.68f, 1.30f, -0.3f, -0.9f
    };

    struct S {
        float x = 0.f, y = 0.f, vy = 0.f;
        bool ground = false;
        bool dead = false;
        bool flip = false;
        int lock = -1;
    };

    inline bool isPad(int k) { return k >= PadYellow && k <= PadBlue; }
    inline bool isOrb(int k) { return k >= OrbYellow && k <= OrbGreen; }
    inline bool isPortal(int k) { return k == PortalNormal || k == PortalInverse; }
    inline bool flipsGravity(int k) { return k == PadBlue || k == OrbBlue || k == OrbGreen; }

    inline bool overlap(float x, float y, float h, Box const& b) {
        return x + h > b.x0 && x - h < b.x1 && y + h > b.y0 && y - h < b.y1;
    }

    inline std::vector<Box>::iterator firstNear(float x) {
        return std::lower_bound(boxes.begin(), boxes.end(), x - HALF - maxW - 20.f,
            [](Box const& b, float v) { return b.x0 < v; });
    }

    template <class F>
    inline void forEachNear(float x, F&& fn) {
        for (auto it = firstNear(x); it != boxes.end() && it->x0 <= x + HALF + 20.f; ++it) fn(*it);
        for (auto const& b : wide) fn(b);
    }

    inline int indexOf(Box const& b) { return static_cast<int>(&b - boxes.data()); }

    inline int findOrb(S const& s) {
        for (auto it = firstNear(s.x); it != boxes.end() && it->x0 <= s.x + HALF + 20.f; ++it) {
            if (!isOrb(it->kind)) continue;
            int idx = static_cast<int>(it - boxes.begin());
            if (idx == s.lock) continue;
            if (overlap(s.x, s.y, ORB_HALF, *it)) return idx;
        }
        return -1;
    }

    inline bool canAct(S const& s) {
        return !s.dead && (s.ground || findOrb(s) >= 0);
    }

    inline void activate(S& s, Box const& b, int idx) {
        if (flipsGravity(b.kind)) s.flip = !s.flip;
        s.vy = jumpVel * launch[b.kind];
        s.ground = false;
        s.lock = idx;
    }

    inline void step(S& s, float vx, bool press) {
        if (s.dead) return;

        if (press) {
            if (s.ground) {
                s.vy = jumpVel;
                s.ground = false;
            } else {
                int o = findOrb(s);
                if (o >= 0) activate(s, boxes[o], o);
            }
        }

        constexpr float dt = 1.f / SUB;
        for (int i = 0; i < SUB && !s.dead; i++) {
            float up = s.flip ? -1.f : 1.f;
            float ly = s.y * up;
            float floorL = s.flip ? -ceilY : floorY;
            float prevBottom = ly - HALF;
            float prevTop = ly + HALF;

            s.vy = std::max(s.vy - grav * dt, TERMINAL);
            ly += s.vy * dt;
            s.ground = false;

            if (ly - HALF <= floorL) {
                ly = floorL + HALF;
                if (s.vy < 0.f) s.vy = 0.f;
                s.ground = true;
            }
            s.y = ly * up;

            forEachNear(s.x, [&](Box const& b) {
                if (b.kind != Solid || s.dead) return;
                if (!overlap(s.x, s.y, HALF, b)) return;
                float by0 = s.flip ? -b.y1 : b.y0;
                float by1 = s.flip ? -b.y0 : b.y1;
                if (s.vy <= 0.f && prevBottom >= by1 - 1.f) {
                    ly = by1 + HALF;
                    s.y = ly * up;
                    s.vy = 0.f;
                    s.ground = true;
                } else if (s.vy > 0.f && prevTop <= by0 + 1.f) {
                    s.dead = true;
                }
            });
            if (s.dead) break;

            s.x += vx * dt;
            forEachNear(s.x, [&](Box const& b) {
                if (s.dead) return;
                float u = s.flip ? -1.f : 1.f;
                switch (b.kind) {
                    case Hazard:
                        if (overlap(s.x, s.y, HAZ_HALF, b)) s.dead = true;
                        break;
                    case Solid:
                        if (overlap(s.x, s.y, HALF, b)) {
                            float by1 = s.flip ? -b.y0 : b.y1;
                            float lyNow = s.y * u;
                            if (lyNow - HALF >= by1 - 2.f) {
                                s.y = (by1 + HALF) * u;
                                s.vy = 0.f;
                                s.ground = true;
                            } else {
                                s.dead = true;
                            }
                        }
                        break;
                    case PortalNormal:
                    case PortalInverse: {
                        bool target = (b.kind == PortalInverse);
                        if (s.flip != target && overlap(s.x, s.y, ORB_HALF, b)) {
                            float absV = s.vy * u;
                            s.flip = target;
                            s.vy = absV * portalMul * (s.flip ? -1.f : 1.f);
                            s.ground = false;
                        }
                        break;
                    }
                    default:
                        if (isPad(b.kind)) {
                            int idx = indexOf(b);
                            if (idx != s.lock && overlap(s.x, s.y, ORB_HALF, b)) activate(s, b, idx);
                        }
                        break;
                }
            });

            if (s.lock >= 0) {
                if (s.lock >= static_cast<int>(boxes.size()) ||
                    !overlap(s.x, s.y, ORB_HALF + 4.f, boxes[s.lock])) {
                    s.lock = -1;
                }
            }
        }
    }

    inline int horizon(float vx) {
        return std::clamp(static_cast<int>(450.f / std::max(vx, 1.f)), 40, 110);
    }

    inline int deathFrame(S s, float vx, int H) {
        for (int i = 0; i < H; i++) {
            step(s, vx, false);
            if (s.dead) return i;
        }
        return -1;
    }

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

    struct Planner {
        int H = 70;
        float vx = 5.19f;
        int budget = 0;

        struct Cand { int f; S s; };

        bool survive(S s, int frame, int pressesLeft) {
            Cand cands[64];
            int nc = 0;
            S t = s;
            int f = frame;
            bool died = false;

            while (f < H) {
                if (--budget <= 0) return false;
                if (pressesLeft > 0 && (f & 1) == 0 && nc < 64 && canAct(t)) {
                    cands[nc++] = {f, t};
                }
                step(t, vx, false);
                if (t.dead) { died = true; break; }
                f++;
            }
            if (!died) return true;

            for (int i = nc - 1; i >= 0; i--) {
                S j = cands[i].s;
                step(j, vx, true);
                if (!j.dead && survive(j, cands[i].f + 1, pressesLeft - 1)) return true;
            }
            return false;
        }
    };
}

$on_mod(Loaded) {
    auto m = Mod::get();
    sim::jumpVel = std::clamp(m->getSavedValue<float>("jumpVel", sim::jumpVel), 7.f, 16.f);
    sim::grav = std::clamp(m->getSavedValue<float>("grav", sim::grav), 0.5f, 1.5f);
    sim::portalMul = std::clamp(m->getSavedValue<float>("portalMul", sim::portalMul), 0.f, 1.3f);
    for (int k = sim::PadYellow; k < sim::KindCount; k++) {
        sim::launch[k] = std::clamp(
            m->getSavedValue<float>(fmt::format("launch{}", k), sim::launch[k]), -3.5f, 3.5f);
    }
}

// ===============================================================
// Draggable node
// ===============================================================
class DraggableNode : public CCNode, public CCTouchDelegate {
protected:
    std::string m_keyX, m_keyY;
    std::function<void()> m_onTap;
    std::function<bool()> m_canDrag;
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
        if (m_canDrag && !m_canDrag()) return false;

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
            *p = !t->isToggled();
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
// Layout Mode
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
// Ticker
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
            ret->scheduleUpdateWithPriority(100);
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
        int ticks = 0;
        int rebuilds = 0;
        bool needRebuild = false;
        bool geometryBuilt = false;

        float clickTimer = 0.f;
        float holdTimer = 0.f;
        bool holding = false;

        bool hitFlag = false;
        bool stepHookSeen = false;
        int stepsSinceHit = 1000;
        int deaths = 0;
        long totalSteps = 0;
        long deadSteps = 0;
        int labelTick = 0;

        DraggableNode* hud = nullptr;
        CCLabelBMFont* hudLabel = nullptr;
        extension::CCScale9Sprite* hudBg = nullptr;

        bool haveLast = false;
        float lastX = 0.f, lastY = 0.f;
        float vx = 5.193f;
        float vy = 0.f;
        int stepsThisTick = 0;
        float tpsEma = 240.f;

        bool arcActive = false;
        bool arcWasGround = true;
        float arcStartX = 0.f, arcStartY = 0.f, arcApex = 0.f;
        float arcPrevX = 0.f, arcPrevY = 0.f;
        float arcUp = 1.f;

        bool stepInit = false;
        float stepPrevX = 0.f, stepPrevY = 0.f, stepPrevAbs = 0.f;
        bool stepPrevGround = true, stepPrevFlip = false;

        CCDrawNode* traj = nullptr;
        int botRelease = 0;
        bool botHolding = false;
        bool havePlan = false;
        float planX = 0.f;
        int cooldown = 0;
        int retry = 0;
        int lockIdx = -1;
        CCLabelBMFont* status = nullptr;
        std::string lastStatus;
    };

    // FIX: geometric local-ground check (works for both gravity directions).
    bool isOnLocalGround(float x, float y, bool flip) {
        float up = flip ? -1.f : 1.f;
        float ly = y * up;
        float floorL = flip ? -sim::ceilY : sim::floorY;

        // Floor / ceiling boundary
        if (std::abs((ly - sim::HALF) - floorL) < 1.5f) return true;

        // Top face of a solid block
        bool onBlock = false;
        sim::forEachNear(x, [&](sim::Box const& b) {
            if (b.kind != sim::Solid) return;
            float by1 = flip ? -b.y0 : b.y1;    // block top in local frame
            if (x + sim::HALF > b.x0 && x - sim::HALF < b.x1 &&
                std::abs((ly - sim::HALF) - by1) < 1.5f) onBlock = true;
        });
        return onBlock;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto win = CCDirector::sharedDirector()->getWinSize();
        CCNode* ui = m_uiLayer ? static_cast<CCNode*>(m_uiLayer) : static_cast<CCNode*>(this);

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

        if (m_objectLayer) {
            auto node = CCDrawNode::create();
            m_objectLayer->addChild(node, 1000);
            m_fields->traj = node;
        }

        auto status = CCLabelBMFont::create("", "bigFont.fnt");
        status->setScale(0.35f);
        status->setOpacity(200);
        status->setPosition({win.width / 2.f, 14.f});
        status->setVisible(false);
        ui->addChild(status, 999);
        m_fields->status = status;

        if (auto ticker = Ticker::create([this](float dt) { this->tickAll(dt); })) {
            this->addChild(ticker);
        }

        return true;
    }

    void buildGeometry() {
        sim::boxes.clear();
        sim::wide.clear();
        sim::maxW = 30.f;
        sim::floorY = 90.f;
        sim::nSolid = sim::nHazard = sim::nPad = sim::nOrb = sim::nPortal = 0;
        if (!m_objects) return;

        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;

            int kind = -1;
            switch (obj->m_objectID) {
                case 35:   kind = sim::PadYellow; break;
                case 140:  kind = sim::PadPink;   break;
                case 1332: kind = sim::PadRed;    break;
                case 67:   kind = sim::PadBlue;   break;
                case 36:   kind = sim::OrbYellow; break;
                case 141:  kind = sim::OrbPink;   break;
                case 1333: kind = sim::OrbRed;    break;
                case 84:   kind = sim::OrbBlue;   break;
                case 1022: kind = sim::OrbGreen;  break;
                default:
                    if (obj->m_objectType == GameObjectType::Hazard) kind = sim::Hazard;
                    else if (obj->m_objectType == GameObjectType::Solid) kind = sim::Solid;
                    else if (obj->m_objectType == GameObjectType::InverseGravityPortal) kind = sim::PortalInverse;
                    else if (obj->m_objectType == GameObjectType::NormalGravityPortal) kind = sim::PortalNormal;
            }
            if (kind < 0) continue;

            auto pos = obj->getPosition();
            CCRect r;

            if (sim::isOrb(kind)) {
                r = CCRect(pos.x - 18.f, pos.y - 18.f, 36.f, 36.f);
            } else {
                r = obj->getObjectRect();
                float mx = r.origin.x + r.size.width * 0.5f;
                float my = r.origin.y + r.size.height * 0.5f;
                bool valid = r.size.width >= 1.f && r.size.height >= 1.f &&
                             std::abs(mx - pos.x) < 80.f && std::abs(my - pos.y) < 80.f;
                if (!valid) {
                    auto bb = obj->boundingBox();
                    if (kind == sim::Hazard) {
                        float cx = bb.origin.x + bb.size.width * 0.5f;
                        float cy = bb.origin.y + bb.size.height * 0.5f;
                        float w = bb.size.width * 0.55f, h = bb.size.height * 0.55f;
                        r = CCRect(cx - w * 0.5f, cy - h * 0.5f, w, h);
                    } else if (sim::isPortal(kind)) {
                        r = CCRect(pos.x - 10.f, bb.origin.y, 20.f, bb.size.height);
                    } else if (sim::isPad(kind)) {
                        r = CCRect(pos.x - 15.f, pos.y - 15.f, 30.f, 12.f);
                    } else {
                        r = bb;
                    }
                }
            }
            if (r.size.width <= 0.f || r.size.height <= 0.f) continue;

            sim::Box b{r.origin.x, r.origin.y, r.origin.x + r.size.width, r.origin.y + r.size.height, kind};
            bool isWide = r.size.width > 120.f && (kind == sim::Solid || kind == sim::Hazard);
            if (isWide) {
                sim::wide.push_back(b);
            } else {
                sim::boxes.push_back(b);
                sim::maxW = std::max(sim::maxW, r.size.width);
            }

            if (kind == sim::Solid) sim::nSolid++;
            else if (kind == sim::Hazard) sim::nHazard++;
            else if (sim::isPad(kind)) sim::nPad++;
            else if (sim::isPortal(kind)) sim::nPortal++;
            else sim::nOrb++;
        }
        std::sort(sim::boxes.begin(), sim::boxes.end(),
                  [](sim::Box const& a, sim::Box const& b) { return a.x0 < b.x0; });
    }

    void pressJump(bool down) {
        this->handleButton(down, 1, true);
        if (m_gameState.m_isDualMode && m_player2) {
            this->handleButton(down, 1, false);
        }
    }

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
        f->cooldown = 0;
        f->retry = 0;
        f->lockIdx = -1;
        f->deaths = 0;
        f->totalSteps = 0;
        f->deadSteps = 0;
        f->stepsSinceHit = 1000;
        f->hitFlag = false;
        f->arcActive = false;
        f->stepInit = false;
        f->haveLast = false;
        if (f->rebuilds < 2) {
            f->rebuilds++;
            f->needRebuild = true;
        }
        this->applyLayoutMode(g.layout);
    }

    void tickAll(float dt) {
        auto f = m_fields.self();
        f->ticks++;

        if ((!f->geometryBuilt && m_objects && m_objects->count() > 0) ||
            f->ticks == 20 || f->needRebuild) {
            f->geometryBuilt = true;
            f->needRebuild = false;
            this->buildGeometry();
        }

        this->updateHud();
        this->tickNoclipStats();
        this->measure(dt);

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

    void updateStatus() {
        auto f = m_fields.self();
        if (!f->status) return;

        bool show = g.bot || g.trajectory;
        f->status->setVisible(show);
        if (!show) return;

        std::string msg;
        if (!m_player1) msg = "no player";
        else if (!this->cubeMode()) msg = "Not in cube mode (bot + trajectory are cube only)";
        else if (sim::boxes.empty() && sim::wide.empty()) msg = "No level geometry found";
        else msg = fmt::format("S{} H{} P{} O{} G{} | jump {:.2f}/{:.3f} | {}",
                               sim::nSolid, sim::nHazard, sim::nPad, sim::nOrb, sim::nPortal,
                               sim::jumpVel, sim::grav, g.bot ? "bot" : "traj");

        if (msg != f->lastStatus) {
            f->lastStatus = msg;
            f->status->setString(msg.c_str());
        }
    }

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

        if (f->hudBg) f->hudBg->setColor(g.hudEdit ? ccColor3B{255, 220, 60} : ccColor3B{255, 255, 255});
    }

    void registerStep(bool fromStepHook) {
        auto f = m_fields.self();
        bool hit = f->hitFlag;
        f->hitFlag = false;

        if (!g.noclip) {
            f->stepsSinceHit = 1000;
            return;
        }

        int gap = fromStepHook ? 6 : 2;
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

    void onPhysicsStep() {
        auto f = m_fields.self();
        f->stepHookSeen = true;
        f->stepsThisTick++;
        this->registerStep(true);
        this->sampleArc();
        this->learnLaunch();
    }

    // FIX: use geometric local ground for stepPrevGround (works with flipped gravity).
    void learnLaunch() {
        auto f = m_fields.self();
        if (!m_player1 || sim::boxes.empty()) return;

        float x = m_player1->getPositionX();
        float y = m_player1->getPositionY();
        bool flip = m_player1->m_isUpsideDown;
        bool localGround = this->isOnLocalGround(x, y, flip);

        float tps = std::max(60.f, std::round(f->tpsEma / 60.f) * 60.f);
        float absV = (y - f->stepPrevY) / (60.f / tps);

        if (f->stepInit && this->cubeMode() && std::abs(absV - f->stepPrevAbs) > 3.f && std::abs(absV) < 40.f) {
            int launcher = -1, portal = -1;
            sim::forEachNear(f->stepPrevX, [&](sim::Box const& b) {
                if (!sim::overlap(f->stepPrevX, f->stepPrevY, sim::ORB_HALF + 6.f, b)) return;
                if (sim::isPad(b.kind) || sim::isOrb(b.kind)) { if (launcher < 0) launcher = b.kind; }
                else if (sim::isPortal(b.kind)) { if (portal < 0) portal = b.kind; }
            });

            auto m = Mod::get();
            if (launcher >= 0 && (sim::isPad(launcher) || !f->stepPrevGround)) {
                float mult = (absV * (flip ? -1.f : 1.f)) / sim::jumpVel;
                if (std::abs(mult) > 0.1f && std::abs(mult) < 3.5f) {
                    sim::launch[launcher] += (mult - sim::launch[launcher]) * 0.5f;
                    m->setSavedValue<float>(fmt::format("launch{}", launcher), sim::launch[launcher]);
                }
            } else if (portal >= 0 && flip != f->stepPrevFlip && std::abs(f->stepPrevAbs) > 4.f) {
                float r = absV / f->stepPrevAbs;
                if (r > 0.f && r < 1.3f) {
                    sim::portalMul += (r - sim::portalMul) * 0.5f;
                    m->setSavedValue<float>("portalMul", sim::portalMul);
                }
            }
        }

        f->stepInit = true;
        f->stepPrevX = x;
        f->stepPrevY = y;
        f->stepPrevAbs = absV;
        f->stepPrevGround = localGround;
        f->stepPrevFlip = flip;
    }

    void sampleArc() {
        auto f = m_fields.self();
        if (!m_player1) return;
        float x = m_player1->getPositionX();
        float y = m_player1->getPositionY();
        bool ground = m_player1->m_isOnGround;
        bool flip = m_player1->m_isUpsideDown;
        float up = flip ? -1.f : 1.f;

        if (!this->cubeMode()) {
            f->arcActive = false;
        } else if (!f->arcActive) {
            if (f->arcWasGround && !ground && (y - f->arcPrevY) * up > 0.01f) {
                f->arcActive = true;
                f->arcUp = up;
                f->arcStartX = f->arcPrevX;
                f->arcStartY = f->arcPrevY;
                f->arcApex = (y - f->arcStartY) * up;

                sim::forEachNear(f->arcPrevX, [&](sim::Box const& b) {
                    if ((sim::isPad(b.kind) || sim::isPortal(b.kind)) &&
                        sim::overlap(f->arcPrevX, f->arcPrevY, sim::HALF + 6.f, b)) {
                        f->arcActive = false;
                    }
                });
            }
        } else {
            f->arcApex = std::max(f->arcApex, (y - f->arcStartY) * f->arcUp);
            float dx = x - f->arcStartX;

            sim::S probe;
            probe.x = x; probe.y = y; probe.ground = ground;
            if (dx < 0.f || dx > f->vx * 70.f || up != f->arcUp || sim::findOrb(probe) >= 0) {
                f->arcActive = false;
            } else if (ground) {
                f->arcActive = false;
                float dy = y - f->arcStartY;
                if (std::abs(dy) < 0.5f && f->vx > 1.f) {
                    if (sim::calibrate(f->arcApex, dx / f->vx)) {
                        auto m = Mod::get();
                        m->setSavedValue<float>("jumpVel", sim::jumpVel);
                        m->setSavedValue<float>("grav", sim::grav);
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

        if (!f->stepHookSeen) {
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

    void measure(float dt) {
        if (!m_player1) return;
        auto f = m_fields.self();
        float x = m_player1->getPositionX();
        float y = m_player1->getPositionY();

        int steps = f->stepsThisTick;
        f->stepsThisTick = 0;

        float frames60;
        if (f->stepHookSeen) {
            if (dt > 0.0005f && dt < 0.1f) {
                f->tpsEma = f->tpsEma * 0.98f + (static_cast<float>(steps) / dt) * 0.02f;
            }
            float tps = std::max(60.f, std::round(f->tpsEma / 60.f) * 60.f);
            frames60 = static_cast<float>(steps) * (60.f / tps);
        } else {
            frames60 = dt * 60.f;
        }

        if (f->haveLast && frames60 > 0.f) {
            float dx = (x - f->lastX) / frames60;
            float dy = (y - f->lastY) / frames60;
            if (dx > 0.5f && dx < 20.f) f->vx = dx;
            f->vy = std::clamp(dy, -30.f, 30.f);
        }
        f->lastX = x;
        f->lastY = y;
        f->haveLast = true;
    }

    bool cubeMode() {
        auto p = m_player1;
        return p && !p->m_isDead && !p->m_isShip && !p->m_isBird && !p->m_isBall &&
               !p->m_isDart && !p->m_isRobot && !p->m_isSpider && !p->m_isSwing;
    }

    // FIX: gravity-agnostic ground detection. Uses geometric local-ground, falls
    // back to the game's own m_isOnGround. Learns ceilY / floorY from any ground
    // contact, so the sim stays accurate when flipped.
    sim::S currentSimState() {
        auto f = m_fields.self();
        sim::S s;
        bool flip = m_player1->m_isUpsideDown;
        float up = flip ? -1.f : 1.f;
        s.x = m_player1->getPositionX();
        s.y = m_player1->getPositionY();
        s.flip = flip;
        s.dead = false;
        s.lock = -1;

        // Geometric check first (works for both gravity directions),
        // then OR with the game's own flag as a safety net.
        s.ground = this->isOnLocalGround(s.x, s.y, flip) || m_player1->m_isOnGround;
        s.vy = s.ground ? 0.f : f->vy * up;

        if (f->lockIdx >= 0) {
            if (f->lockIdx < static_cast<int>(sim::boxes.size()) &&
                sim::overlap(s.x, s.y, sim::ORB_HALF + 4.f, sim::boxes[f->lockIdx])) {
                s.lock = f->lockIdx;
            } else {
                f->lockIdx = -1;
            }
        }
        if (!s.ground && s.vy > 2.f && s.lock < 0) {
            int found = -1;
            sim::forEachNear(s.x, [&](sim::Box const& b) {
                if (sim::isPad(b.kind) && sim::overlap(s.x, s.y, sim::ORB_HALF, b)) {
                    found = sim::indexOf(b);
                }
            });
            if (found >= 0) s.lock = found;
        }

        // Learn floor / ceiling from any grounded state where we're not on a block.
        if (s.ground) {
            bool onBlock = false;
            float ly = s.y * up;
            sim::forEachNear(s.x, [&](sim::Box const& b) {
                if (b.kind != sim::Solid) return;
                float by1 = flip ? -b.y0 : b.y1;
                if (s.x + sim::HALF > b.x0 && s.x - sim::HALF < b.x1 &&
                    std::abs((ly - sim::HALF) - by1) < 1.5f) onBlock = true;
            });
            if (!onBlock) {
                float worldEdge = flip ? s.y + sim::HALF : s.y - sim::HALF;
                if (!flip && worldEdge > 84.f && worldEdge < 96.f) sim::floorY = worldEdge;
                if (flip && worldEdge > 200.f && worldEdge < 900.f) sim::ceilY = worldEdge;
            }
        }
        return s;
    }

    void drawTrajectory() {
        auto f = m_fields.self();
        if (!f->traj) return;
        f->traj->clear();
        if (!g.trajectory || !this->cubeMode()) return;

        auto start = this->currentSimState();
        int H = sim::horizon(f->vx);

        auto drawPath = [&](bool press, ccColor4F col) {
            auto s = start;
            auto prev = CCPoint{s.x, s.y};
            for (int i = 0; i < H; i++) {
                sim::step(s, f->vx, press && i == 0);
                if (i % 3 == 2 || s.dead) {
                    CCPoint p{s.x, s.y};
                    f->traj->drawSegment(prev, p, 1.2f, col);
                    prev = p;
                }
                if (s.dead) {
                    f->traj->drawDot(prev, 4.f, ccc4f(1.f, 0.f, 0.f, 1.f));
                    break;
                }
            }
        };

        drawPath(false, ccc4f(1.f, 0.4f, 0.4f, 0.9f));
        if (sim::canAct(start)) drawPath(true, ccc4f(0.3f, 1.f, 0.4f, 0.9f));
    }

    // FIX: press with a caller-controlled cooldown so the desperation branch
    // can't spam multiple useless clicks before the real jump.
    void botPress(sim::S const& s, int cooldown = 3) {
        auto f = m_fields.self();
        if (!s.ground) {
            f->lockIdx = sim::findOrb(s);
        }
        f->havePlan = false;
        f->botHolding = true;
        f->botRelease = 2;
        f->cooldown = cooldown;
        this->pressJump(true);
    }

    // FIX: aim for the LATEST safe frame (last - 1, clamped to the window) so
    // the jump fires as late as possible. And do NOT press at first==0 unless
    // the window is actually about to close.
    void tickBot() {
        auto f = m_fields.self();

        if (f->botRelease > 0 && --f->botRelease == 0) {
            f->botHolding = false;
            this->pressJump(false);
        }
        if (f->cooldown > 0) f->cooldown--;

        if (!this->cubeMode() || (sim::boxes.empty() && sim::wide.empty())) return;
        if (f->botHolding) return;

        auto s = this->currentSimState();
        sim::Planner p;
        p.vx = f->vx;
        p.H = sim::horizon(p.vx);

        if (f->havePlan && f->planX < s.x - 2.f * p.vx) f->havePlan = false;

        bool act = sim::canAct(s);
        int d = sim::deathFrame(s, p.vx, p.H);

        if (f->havePlan) {
            if (d < 0) {
                f->havePlan = false;
            } else if (act && s.x + p.vx * 0.5f >= f->planX) {
                this->botPress(s);
            }
            return;
        }

        if (d < 0) return;
        if (f->cooldown > 0) return;
        if (f->retry > 0) { f->retry--; return; }

        auto scan = [&](int extra, int& first, int& last) {
            first = last = -1;
            int runFirst = -1;
            auto w = s;
            for (int k = 0; k <= d; k++) {
                if (sim::canAct(w) && (extra == 0 || (k & 1) == 0)) {
                    auto j = w;
                    sim::step(j, p.vx, true);
                    p.budget = extra == 0 ? 3000 : 6000;
                    bool ok = !j.dead && p.survive(j, 1, extra);
                    if (ok) {
                        if (runFirst < 0) runFirst = k;
                        first = runFirst;
                        last = k;
                    } else {
                        runFirst = -1;
                    }
                }
                sim::step(w, p.vx, false);
                if (w.dead) break;
            }
        };

        int first, last;
        scan(0, first, last);
        if (first < 0) scan(2, first, last);

        if (first < 0) {
            // No safe window: only press if we're grounded or next to an orb,
            // and use a long cooldown so we don't spam multiple "useless clicks".
            if (act && d <= 5) this->botPress(s, 10);
            else f->retry = 2;
            return;
        }

        // Aim near the END of the safe window (latest possible press), clamped
        // to [first, last] so narrow windows still get the correct frame.
        int aimFrame = std::max(first, last - 1);
        float aimX = s.x + p.vx * aimFrame;

        bool mustPress = (first == 0 && last <= 1);
        bool reachedAim = act && s.x + p.vx * 0.5f >= aimX;

        if (mustPress || reachedAim) {
            this->botPress(s);
            return;
        }
        f->havePlan = true;
        f->planX = aimX;
    }

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
// Per-physics-step hook
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

