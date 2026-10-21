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
#include <unordered_map>
#include <deque>

using namespace geode::prelude;

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

class $modify(SpeedScheduler, CCScheduler) {
    void update(float dt) {
        if (g.speedOn && PlayLayer::get()) dt *= g.speed;
        CCScheduler::update(dt);
    }
};

// ===============================================================
// Cube / Ball / Ship physics simulator
// ===============================================================
namespace sim {
    enum Kind : int {
        Solid = 0, Hazard, Slope,
        PortalNormal, PortalInverse,
        PortalSpeed05, PortalSpeed1, PortalSpeed2, PortalSpeed3, PortalSpeed4,
        PadYellow, PadPink, PadRed, PadBlue,
        OrbYellow, OrbPink, OrbRed, OrbBlue, OrbGreen,
        KindCount
    };

    // Mode: 0 = Cube, 1 = Ship, 2 = Ball
    struct Box { float x0, y0, x1, y1; int kind; GameObject* src = nullptr; };

    inline std::vector<Box> boxes;
    inline std::vector<Box> wide;
    inline float maxW = 30.f;
    inline float floorY = 90.f;
    inline float ceilY = 600.f;
    inline int nSolid = 0, nHazard = 0, nPad = 0, nOrb = 0, nPortal = 0;

    constexpr float HALF = 15.f;
    constexpr float HAZ_HALF = 11.f; 
    constexpr float ORB_HALF = 12.f; 
    constexpr float TERMINAL = -15.f;
    constexpr int SUB = 2;

    inline float jumpVel = 11.180032f;
    inline float grav = 0.958199f;
    inline float portalMul = 0.5f;
    inline float speedMul = 1.0f;

    inline float launch[KindCount] = {
        0.f, 0.f, 0.f, 0.f,
        0.f, 0.f, 0.f, 0.f, 0.f,
        1.40f, 0.93f, 1.79f, -0.9f,
        1.00f, 0.68f, 1.30f, -1.1f, -1.3f
    };

    struct S {
        float x = 0.f, y = 0.f, vy = 0.f;
        bool ground = false;
        bool dead = false;
        bool flip = false;
        int mode = 0; // 0=Cube, 1=Ship, 2=Ball
        int lock = -1;
    };

    inline bool isPad(int k) { return k >= PadYellow && k <= PadBlue; }
    inline bool isOrb(int k) { return k >= OrbYellow && k <= OrbGreen; }
    inline bool isPortal(int k) { return k >= PortalNormal && k <= PortalSpeed4; }
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
        return !s.dead && (s.ground || s.mode == 1 || findOrb(s) >= 0); // Ship can always act
    }

    inline void activate(S& s, Box const& b, int idx) {
        if (flipsGravity(b.kind)) s.flip = !s.flip;
        if (isOrb(b.kind)) s.vy = jumpVel * launch[b.kind];
        s.ground = false;
        s.lock = idx;
    }

    inline void step(S& s, float vx, bool press) {
        if (s.dead) return;

        // Apply current speed multiplier
        float actual_vx = vx * speedMul;

        if (press) {
            if (s.mode == 0) { // Cube
                int o = findOrb(s);
                if (o >= 0) activate(s, boxes[o], o);
                else if (s.ground) { s.vy = jumpVel; s.ground = false; }
            } else if (s.mode == 2) { // Ball
                s.flip = !s.flip;
                s.vy = 0.f;
            }
        }

        constexpr float dt = 1.f / SUB;
        for (int i = 0; i < SUB && !s.dead; i++) {
            float up = s.flip ? -1.f : 1.f;
            float ly = s.y * up;
            float floorL = s.flip ? -ceilY : floorY;
            float prevBottom = ly - HALF;
            float prevTop = ly + HALF;

            // Mode-specific vertical physics
            if (s.mode == 1) { // Ship
                s.vy += (press ? 0.6f : -0.6f) * dt;
                s.vy = std::clamp(s.vy, -8.f, 8.f);
            } else if (s.mode == 2) { // Ball
                s.vy = std::max(s.vy - (grav * 1.2f) * dt, TERMINAL);
            } else { // Cube
                s.vy = std::max(s.vy - grav * dt, TERMINAL);
            }

            ly += s.vy * dt;
            s.ground = false;

            if (ly - HALF <= floorL) {
                ly = floorL + HALF;
                if (s.vy < 0.f) s.vy = 0.f;
                s.ground = true;
            }
            s.y = ly * up;

            forEachNear(s.x, [&](Box const& b) {
                if (s.dead) return;
                if (b.kind == Solid || b.kind == Slope) {
                    if (!overlap(s.x, s.y, HALF, b)) return;
                    float by0 = s.flip ? -b.y1 : b.y0;
                    float by1 = s.flip ? -b.y0 : b.y1;

                    // Basic Slope handling (pushes player up if walking on it)
                    if (b.kind == Slope) {
                        float slopeY = b.y0 + (s.x - b.x0);
                        if (s.y - HALF < slopeY) {
                            s.y = (slopeY + HALF) * up;
                            s.vy = 0.f;
                            s.ground = true;
                        }
                        return;
                    }

                    if (s.vy <= 0.f && prevBottom >= by1 - 1.f) {
                        ly = by1 + HALF;
                        s.y = ly * up;
                        s.vy = 0.f;
                        s.ground = true;
                    } else if (s.vy > 0.f && prevTop <= by0 + 1.f) {
                        ly = by0 - HALF;
                        s.y = ly * up;
                        s.vy = 0.f;
                    }
                }
            });
            if (s.dead) break;

            s.x += actual_vx * dt;
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
                    case PortalNormal: case PortalInverse:
                    case PortalSpeed05: case PortalSpeed1: case PortalSpeed2: case PortalSpeed3: case PortalSpeed4: {
                        if (!overlap(s.x, s.y, ORB_HALF, b)) return;
                        if (b.kind == PortalNormal || b.kind == PortalInverse) {
                            bool target = (b.kind == PortalInverse);
                            if (s.flip != target) {
                                float absV = s.vy * u;
                                s.flip = target;
                                s.vy = absV * portalMul * (s.flip ? -1.f : 1.f);
                                s.ground = false;
                            }
                        } else {
                            if (b.kind == PortalSpeed05) speedMul = 0.5f;
                            else if (b.kind == PortalSpeed1) speedMul = 1.0f;
                            else if (b.kind == PortalSpeed2) speedMul = 1.3f;
                            else if (b.kind == PortalSpeed3) speedMul = 1.6f;
                            else if (b.kind == PortalSpeed4) speedMul = 1.9f;
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
                if (s.lock >= static_cast<int>(boxes.size()) || !overlap(s.x, s.y, ORB_HALF + 4.f, boxes[s.lock])) {
                    s.lock = -1;
                }
            }
        }
    }

    inline int horizon(float vx) {
        return std::clamp(static_cast<int>((550.f * speedMul) / std::max(vx, 1.f)), 55, 120);
    }

    // BFS Planner (Breadth-First Search)
    struct Planner {
        int H = 90;
        float vx = 5.19f;
        int budget = 0;

        struct Node {
            S state;
            int frame;
            int presses;
            bool firstAction;
        };

        bool survive(S start, int initialFrame, bool& shouldPressFirst) {
            std::deque<Node> q;
            q.push_back({start, initialFrame, 0, false});

            while (!q.empty()) {
                if (--budget <= 0) return false;
                Node n = q.front();
                q.pop_front();

                if (n.frame >= H) {
                    shouldPressFirst = n.firstAction;
                    return true;
                }

                S coast = n.state;
                step(coast, vx, false);
                if (!coast.dead) {
                    q.push_back({coast, n.frame + 1, n.presses, n.firstAction});
                }

                if (n.presses < 3 && canAct(n.state)) {
                    S press = n.state;
                    step(press, vx, true);
                    if (!press.dead) {
                        bool isFirst = (n.frame == initialFrame) ? true : n.firstAction;
                        q.push_back({press, n.frame + 1, n.presses + 1, isFirst});
                    }
                }
            }
            return false;
        }
    };
}

// ===============================================================
// Wave physics
// ===============================================================
namespace waveSim {
    constexpr float HALF = sim::HALF;
    constexpr float HAZ_HALF = sim::HAZ_HALF;

    struct S {
        float x = 0.f, y = 0.f;
        int dir = +1;
        bool dead = false;
    };

    inline void step(S& s, float vx, bool click) {
        if (s.dead) return;
        if (click) s.dir = -s.dir;

        constexpr int SUB = 2;
        constexpr float dt = 1.f / SUB;
        float actual_vx = vx * sim::speedMul;

        for (int i = 0; i < SUB && !s.dead; i++) {
            s.x += actual_vx * dt;
            s.y += s.dir * actual_vx * dt;

            if (s.y - HALF < sim::floorY) { s.dead = true; return; }
            if (s.y + HALF > sim::ceilY)  { s.dead = true; return; }

            sim::forEachNear(s.x, [&](sim::Box const& b) {
                if (s.dead) return;
                if (b.kind == sim::Hazard) {
                    if (sim::overlap(s.x, s.y, HAZ_HALF, b)) s.dead = true;
                } else if (b.kind == sim::Solid) {
                    if (sim::overlap(s.x, s.y, HALF, b)) s.dead = true;
                }
            });
        }
    }
}

$on_mod(Loaded) {
    sim::jumpVel = 11.180032f;
    sim::grav = 0.958199f;
    sim::portalMul = 0.5f;
    for (int k = sim::PadYellow; k < sim::KindCount; k++) {
        if (sim::isOrb(k)) sim::launch[k] = (k == sim::OrbYellow) ? 1.00f :
                                            (k == sim::OrbPink)   ? 0.68f :
                                            (k == sim::OrbRed)    ? 1.30f :
                                            (k == sim::OrbBlue)   ? -1.1f : -1.3f;
    }
}

// ===============================================================
// Draggable node & Menu (Unchanged from previous)
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
        this->setPosition({std::clamp(x, 20.f, win.width - 20.f), std::clamp(y, 20.f, win.height - 20.f)});
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
        if (!isUsable() || (m_canDrag && !m_canDrag())) return false;
        auto local = this->convertToNodeSpace(touch->getLocation());
        auto size = this->getContentSize();
        if (local.x < 0 || local.y < 0 || local.x > size.width || local.y > size.height) return false;
        m_touching = true; m_dragging = false;
        m_nodeStart = this->getPosition();
        m_touchStart = this->getParent() ? this->getParent()->convertToNodeSpace(touch->getLocation()) : touch->getLocation();
        if (m_scaleTarget) m_scaleTarget->setScale(0.92f);
        return true;
    }
    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (!m_touching) return;
        auto parent = this->getParent();
        auto now = parent ? parent->convertToNodeSpace(touch->getLocation()) : touch->getLocation();
        auto diff = now - m_touchStart;
        if (!m_dragging && (std::abs(diff.x) > 6.f || std::abs(diff.y) > 6.f)) m_dragging = true;
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
        } else if (m_onTap) m_onTap();
    }
    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_touching = false; m_dragging = false;
        if (m_scaleTarget) m_scaleTarget->setScale(1.f);
    }
};

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
        auto resetBtn = CCMenuItemExt::createSpriteExtra(resetSpr, [](CCMenuItemSpriteExtra*) { g.hudResetRequested = true; });
        resetBtn->setPosition({R + 55.f, -12.f});
        menu->addChild(resetBtn);

        m_speedLabel = this->addStepper(menu, -45.f, [this](int dir) {
            g.speed = std::clamp(std::round((g.speed + dir * 0.1f) * 10.f) / 10.f, 0.1f, 5.f);
            g.save(); m_speedLabel->setString(fmt::format("Speed: {:.1f}x", g.speed).c_str());
        });
        m_speedLabel->setString(fmt::format("Speed: {:.1f}x", g.speed).c_str());

        m_cpsLabel = this->addStepper(menu, -75.f, [this](int dir) {
            g.cps = std::clamp(std::round(g.cps + dir), 1.f, 30.f);
            g.save(); m_cpsLabel->setString(fmt::format("Click CPS: {:.0f}", g.cps).c_str());
        });
        m_cpsLabel->setString(fmt::format("Click CPS: {:.0f}", g.cps).c_str());

        m_hudLabel = this->addStepper(menu, -105.f, [this](int dir) {
            g.hudScale = std::clamp(std::round((g.hudScale + dir * 0.1f) * 10.f) / 10.f, 0.5f, 2.5f);
            g.save(); m_hudLabel->setString(fmt::format("HUD Size: {:.1f}x", g.hudScale).c_str());
        });
        m_hudLabel->setString(fmt::format("HUD Size: {:.1f}x", g.hudScale).c_str());
        return true;
    }

    void addToggle(CCMenu* menu, char const* text, bool& ref, float x, float y) {
        auto on = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto off = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        bool* p = &ref;
        auto toggler = CCMenuItemExt::createToggler(on, off, [p](CCMenuItemToggler* t) { *p = !t->isToggled(); g.save(); });
        toggler->setScale(0.6f); toggler->toggle(ref); toggler->setPosition({x, y}); menu->addChild(toggler);
        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(0.42f); label->setAnchorPoint({0.f, 0.5f}); label->setPosition({x + 20.f, y}); menu->addChild(label);
    }

    CCLabelBMFont* addStepper(CCMenu* menu, float y, std::function<void(int)> change) {
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(0.5f); label->setPosition({0.f, y}); menu->addChild(label);
        auto leftSpr = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
        leftSpr->setFlipX(true); leftSpr->setScale(0.6f);
        auto left = CCMenuItemExt::createSpriteExtra(leftSpr, [change](CCMenuItemSpriteExtra*) { change(-1); });
        left->setPosition({-105.f, y}); menu->addChild(left);
        auto rightSpr = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
        rightSpr->setScale(0.6f);
        auto right = CCMenuItemExt::createSpriteExtra(rightSpr, [change](CCMenuItemSpriteExtra*) { change(1); });
        right->setPosition({105.f, y}); menu->addChild(right);
        return label;
    }

public:
    static BotMenu* create() {
        auto ret = new BotMenu();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
};

class $modify(LayoutGameObject, GameObject) {
    void setVisible(bool v) {
        if (g.layout && m_objectType == GameObjectType::Decoration) v = false;
        GameObject::setVisible(v);
    }
};

class $modify(JumpPlayer, PlayerObject) {
    bool pushButton(PlayerButton btn) {
        if (g.jump && btn == PlayerButton::Jump && PlayLayer::get()) m_isOnGround = true;
        return PlayerObject::pushButton(btn);
    }
};

class Ticker : public CCNode {
protected:
    std::function<void(float)> m_fn;
public:
    static Ticker* create(std::function<void(float)> fn) {
        auto ret = new Ticker();
        if (ret->init()) { ret->m_fn = std::move(fn); ret->autorelease(); ret->scheduleUpdateWithPriority(100); return ret; }
        delete ret; return nullptr;
    }
    void update(float dt) override { if (m_fn) m_fn(dt); }
};

// ===============================================================
// PlayLayer hook
// ===============================================================
class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        bool prevLayout = false;
        int ticks = 0, rebuilds = 0;
        bool needRebuild = false, geometryBuilt = false;
        float lastRebuildX = -1e9f;

        float clickTimer = 0.f, holdTimer = 0.f;
        bool holding = false;

        bool hitFlag = false, stepHookSeen = false;
        int stepsSinceHit = 1000, deaths = 0;
        long totalSteps = 0, deadSteps = 0;
        int labelTick = 0;

        DraggableNode* hud = nullptr;
        CCLabelBMFont* hudLabel = nullptr;
        extension::CCScale9Sprite* hudBg = nullptr;

        bool haveLast = false;
        float lastX = 0.f, lastY = 0.f, vx = 5.193f, vy = 0.f;
        int stepsThisTick = 0;
        float tpsEma = 240.f;

        bool stepInit = false;
        float stepPrevX = 0.f, stepPrevY = 0.f, stepPrevAbs = 0.f;

        CCDrawNode* traj = nullptr;
        bool  trajDirty = true;
        int   trajCounter = 0;
        bool  trajPrevGround = true, trajPrevFlip = false;
        float trajPrevVY = 0.f;
        int   trajPrevWaveDir = 1;
        bool  trajPrevWasWave = false;

        int botRelease = 0;
        bool botHolding = false, havePlan = false;
        float planX = 0.f, planDeadlineX = 0.f;
        int planIdle = 0, cooldown = 0, retry = 0;
        int lockIdx = -1, waveDir = 1;
        CCLabelBMFont* status = nullptr;
        std::string lastStatus;
    };

    bool isOnLocalGround(float x, float y, bool flip) {
        float up = flip ? -1.f : 1.f;
        float ly = y * up;
        float floorL = flip ? -sim::ceilY : sim::floorY;
        if (std::abs((ly - sim::HALF) - floorL) < 1.5f) return true;
        bool onBlock = false;
        sim::forEachNear(x, [&](sim::Box const& b) {
            if (b.kind != sim::Solid && b.kind != sim::Slope) return;
            float by1 = flip ? -b.y0 : b.y1;
            if (x + sim::HALF > b.x0 && x - sim::HALF < b.x1 && std::abs((ly - sim::HALF) - by1) < 1.5f) onBlock = true;
        });
        return onBlock;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        auto win = CCDirector::sharedDirector()->getWinSize();
        CCNode* ui = m_uiLayer ? static_cast<CCNode*>(m_uiLayer) : static_cast<CCNode*>(this);

        auto bg = extension::CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({45.f, 45.f}); bg->setOpacity(110);
        if (auto icon = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png")) {
            icon->setScale(0.9f); icon->setPosition(bg->getContentSize() / 2); bg->addChild(icon);
        }
        if (auto btn = DraggableNode::create({45.f, 45.f}, "btn-x", "btn-y", nullptr, [] { BotMenu::create()->show(); })) {
            bg->setPosition({22.5f, 22.5f}); btn->addChild(bg); btn->setVisualScaleTarget(bg);
            btn->restorePosition({40.f, win.height - 40.f}); ui->addChild(btn, 1000);
        }

        if (auto hud = DraggableNode::create({170.f, 26.f}, "hud-x", "hud-y", [] { return g.hudEdit; }, nullptr)) {
            auto hbg = extension::CCScale9Sprite::create("square02_small.png");
            hbg->setContentSize({170.f, 26.f}); hbg->setOpacity(120); hbg->setPosition({85.f, 13.f}); hud->addChild(hbg);
            auto label = CCLabelBMFont::create("Deaths: 0   Acc: 100.0%", "bigFont.fnt");
            label->setScale(0.38f); label->setPosition({85.f, 13.f}); hud->addChild(label);
            hud->restorePosition({win.width / 2.f, win.height - 48.f}); hud->setScale(g.hudScale); hud->setVisible(false);
            ui->addChild(hud, 999); m_fields->hud = hud; m_fields->hudLabel = label; m_fields->hudBg = hbg;
        }

        if (m_objectLayer) {
            auto node = CCDrawNode::create();
            m_objectLayer->addChild(node, 1000);
            m_fields->traj = node;
        }

        auto status = CCLabelBMFont::create("", "bigFont.fnt");
        status->setScale(0.35f); status->setOpacity(200); status->setPosition({win.width / 2.f, 14.f}); status->setVisible(false);
        ui->addChild(status, 999); m_fields->status = status;

        if (auto ticker = Ticker::create([this](float dt) { this->tickAll(dt); })) this->addChild(ticker);
        return true;
    }

    void buildGeometry() {
        std::unordered_map<GameObject*, std::pair<float,float>> oldPos;
        for (auto const& b : sim::boxes) if (b.src) oldPos[b.src] = {b.x0, b.y0};

        sim::boxes.clear(); sim::wide.clear(); sim::maxW = 30.f; sim::floorY = 90.f;
        sim::nSolid = sim::nHazard = sim::nPad = sim::nOrb = sim::nPortal = 0;
        if (!m_objects) return;

        float px = m_player1 ? m_player1->getPositionX() : 0.f;
        float winX0 = m_player1 ? px - 500.f  : -1e9f;
        float winX1 = m_player1 ? px + 1600.f :  1e9f;

        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            auto pos = obj->getPosition();
            if (pos.x < winX0 || pos.x > winX1) continue;

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
                case 200:  kind = sim::PortalSpeed05; break;
                case 201:  kind = sim::PortalSpeed1;  break;
                case 202:  kind = sim::PortalSpeed2;  break;
                case 203:  kind = sim::PortalSpeed3;  break;
                case 1334: kind = sim::PortalSpeed4;  break;
                default:
                    if (obj->m_objectType == GameObjectType::Hazard) kind = sim::Hazard;
                    else if (obj->m_objectType == GameObjectType::Solid) kind = sim::Solid;
                    else if (obj->m_objectType == GameObjectType::Slope) kind = sim::Slope;
                    else if (obj->m_objectType == GameObjectType::InverseGravityPortal) kind = sim::PortalInverse;
                    else if (obj->m_objectType == GameObjectType::NormalGravityPortal) kind = sim::PortalNormal;
            }
            if (kind < 0) continue;

            CCRect r;
            if (sim::isOrb(kind)) {
                r = obj->getObjectRect(); r.origin.x += 2.f; r.origin.y += 2.f; r.size.width -= 4.f; r.size.height -= 4.f;
            } else if (sim::isPad(kind)) {
                r = obj->getObjectRect(); r.origin.y -= 2.f; r.size.height += 4.f;
            } else if (kind == sim::Hazard) {
                r = obj->getObjectRect(); float cx = r.origin.x + r.size.width * 0.5f;
                r.origin.x = cx - 7.5f; r.size.width = 15.f; r.size.height = 15.f;
            } else {
                r = obj->getObjectRect();
                float mx = r.origin.x + r.size.width * 0.5f, my = r.origin.y + r.size.height * 0.5f;
                bool valid = r.size.width >= 1.f && r.size.height >= 1.f && std::abs(mx - pos.x) < 80.f && std::abs(my - pos.y) < 80.f;
                if (!valid) {
                    auto bb = obj->boundingBox();
                    if (sim::isPortal(kind)) r = CCRect(pos.x - 10.f, bb.origin.y, 20.f, bb.size.height);
                    else r = bb;
                }
            }
            if (r.size.width <= 0.f || r.size.height <= 0.f) continue;

            bool isDynamic = false;
            auto oit = oldPos.find(obj);
            if (oit != oldPos.end() && (std::abs(oit->second.first - r.origin.x) > 0.5f || std::abs(oit->second.second - r.origin.y) > 0.5f)) isDynamic = true;
            if (isDynamic && (kind == sim::Solid || kind == sim::Hazard || kind == sim::Slope)) continue;

            sim::Box b{r.origin.x, r.origin.y, r.origin.x + r.size.width, r.origin.y + r.size.height, kind, obj};
            bool isWide = r.size.width > 120.f && (kind == sim::Solid || kind == sim::Hazard || kind == sim::Slope);
            if (isWide) sim::wide.push_back(b);
            else { sim::boxes.push_back(b); sim::maxW = std::max(sim::maxW, r.size.width); }

            if (kind == sim::Solid) sim::nSolid++;
            else if (kind == sim::Hazard) sim::nHazard++;
            else if (sim::isPad(kind)) sim::nPad++;
            else if (sim::isPortal(kind)) sim::nPortal++;
            else sim::nOrb++;
        }
        std::sort(sim::boxes.begin(), sim::boxes.end(), [](sim::Box const& a, sim::Box const& b) { return a.x0 < b.x0; });
    }

    void pressJump(bool down) {
        this->handleButton(down, 1, true);
        if (m_gameState.m_isDualMode && m_player2) this->handleButton(down, 1, false);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g.noclip) { m_fields->hitFlag = true; return; }
        m_fields->haveLast = false; m_fields->vy = 0.f;
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        if (f->holding || f->botHolding) this->pressJump(false);
        f->clickTimer = 0.f; f->holdTimer = 0.f; f->holding = false; f->botHolding = false;
        f->botRelease = 0; f->havePlan = false; f->planIdle = 0; f->cooldown = 0; f->retry = 0; f->lockIdx = -1;
        f->deaths = 0; f->totalSteps = 0; f->deadSteps = 0; f->stepsSinceHit = 1000; f->hitFlag = false;
        f->stepInit = false; f->haveLast = false; f->trajDirty = true; f->trajCounter = 0;
        f->trajPrevGround = true; f->trajPrevFlip = false; f->trajPrevVY = 0.f; f->trajPrevWaveDir = 1;
        f->trajPrevWasWave = false; f->lastRebuildX = -1e9f;
        sim::speedMul = 1.0f; // Reset speed
        if (f->rebuilds < 2) { f->rebuilds++; f->needRebuild = true; }
        this->applyLayoutMode(g.layout);
    }

    void tickAll(float dt) {
        auto f = m_fields.self();
        f->ticks++;
        float px = m_player1 ? m_player1->getPositionX() : 0.f;
        bool movedFar = std::abs(px - f->lastRebuildX) > 200.f;

        if ((!f->geometryBuilt && m_objects && m_objects->count() > 0) || f->ticks == 20 || f->needRebuild || (f->geometryBuilt && (movedFar || f->ticks % 120 == 0))) {
            f->geometryBuilt = true; f->needRebuild = false; f->lastRebuildX = px;
            this->buildGeometry(); f->lockIdx = -1; f->trajDirty = true;
        }

        this->updateHud(); this->tickNoclipStats(); this->measure(dt);

        if (!g.clicker && f->holding) { f->holding = false; this->pressJump(false); }
        if (!g.bot && f->botHolding) { f->botHolding = false; f->botRelease = 0; this->pressJump(false); }

        if (g.bot) this->tickBot();
        else if (g.clicker) this->tickAutoclicker(dt);

        this->drawTrajectory(); this->updateStatus();

        if (g.layout != f->prevLayout) { f->prevLayout = g.layout; this->applyLayoutMode(g.layout); }
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
        else if (this->waveMode()) msg = "Wave mode";
        else if (sim::boxes.empty() && sim::wide.empty()) msg = "No geometry found";
        else msg = fmt::format("S{} H{} P{} O{} | jump {:.2f} | spd {:.1f}x | {}", sim::nSolid, sim::nHazard, sim::nPad, sim::nOrb, sim::jumpVel, sim::speedMul, g.bot ? "bot" : "traj");

        if (msg != f->lastStatus) { f->lastStatus = msg; f->status->setString(msg.c_str()); }
    }

    void updateHud() {
        auto f = m_fields.self();
        if (!f->hud) return;
        auto win = CCDirector::sharedDirector()->getWinSize();
        if (g.hudResetRequested) { g.hudResetRequested = false; g.hudScale = DEFAULT_HUD_SCALE; g.save(); f->hud->resetPosition({win.width / 2.f, win.height - 48.f}); }
        f->hud->setScale(g.hudScale); f->hud->setVisible(g.counter || g.hudEdit);
        if (f->hudBg) f->hudBg->setColor(g.hudEdit ? ccColor3B{255, 220, 60} : ccColor3B{255, 255, 255});
    }

    void registerStep(bool fromStepHook) {
        auto f = m_fields.self();
        bool hit = f->hitFlag; f->hitFlag = false;
        if (!g.noclip) { f->stepsSinceHit = 1000; return; }
        int gap = fromStepHook ? 6 : 2; f->totalSteps++;
        if (hit) {
            if (f->stepsSinceHit <= gap) f->deadSteps += f->stepsSinceHit;
            else f->deaths++;
            f->deadSteps++; f->stepsSinceHit = 0;
        } else if (f->stepsSinceHit < 1000) f->stepsSinceHit++;
    }

    void onPhysicsStep() {
        auto f = m_fields.self();
        f->stepHookSeen = true; f->stepsThisTick++;
        this->registerStep(true);
    }

    void tickNoclipStats() {
        auto f = m_fields.self();
        if (!f->stepHookSeen) this->registerStep(false);
        if (f->hudLabel && ++f->labelTick >= 6) {
            f->labelTick = 0;
            float acc = f->totalSteps > 0 ? 100.f * (1.f - static_cast<float>(f->deadSteps) / static_cast<float>(f->totalSteps)) : 100.f;
            f->hudLabel->setString(fmt::format("Deaths: {}   Acc: {:.1f}%", f->deaths, acc).c_str());
            float w = f->hudLabel->getContentSize().width;
            f->hudLabel->setScale(std::min(0.38f, 160.f / std::max(w, 1.f)));
        }
    }

    void tickAutoclicker(float dt) {
        if (!m_player1 || m_player1->m_isDead) return;
        auto f = m_fields.self();
        float interval = 1.f / g.cps, hold = std::min(0.04f, interval * 0.5f);
        if (f->holding) {
            f->holdTimer -= dt;
            if (f->holdTimer <= 0.f) { f->holding = false; this->pressJump(false); }
            return;
        }
        f->clickTimer += dt;
        if (f->clickTimer >= interval) { f->clickTimer = 0.f; f->holding = true; f->holdTimer = hold; this->pressJump(true); }
    }

    void measure(float dt) {
        if (!m_player1) return;
        auto f = m_fields.self();
        float x = m_player1->getPositionX(), y = m_player1->getPositionY();
        int steps = f->stepsThisTick; f->stepsThisTick = 0;
        float frames60 = f->stepHookSeen ? static_cast<float>(steps) * (60.f / std::max(60.f, std::round(f->tpsEma / 60.f) * 60.f)) : dt * 60.f;

        if (f->haveLast && frames60 > 0.f) {
            float dx = (x - f->lastX) / frames60, dy = (y - f->lastY) / frames60;
            if (dx > 0.5f && dx < 30.f) f->vx = dx / std::max(0.1f, sim::speedMul);
            f->vy = std::clamp(dy, -30.f, 30.f);
        }
        f->lastX = x; f->lastY = y; f->haveLast = true;
    }

    bool cubeMode() { auto p = m_player1; return p && !p->m_isDead && !p->m_isShip && !p->m_isBird && !p->m_isBall && !p->m_isDart && !p->m_isRobot && !p->m_isSpider && !p->m_isSwing; }
    bool waveMode() { auto p = m_player1; return p && !p->m_isDead && p->m_isDart; }
    bool shipMode() { auto p = m_player1; return p && !p->m_isDead && p->m_isShip; }
    bool ballMode() { auto p = m_player1; return p && !p->m_isDead && p->m_isBall; }

    sim::S currentSimState() {
        auto f = m_fields.self();
        sim::S s;
        bool flip = m_player1->m_isUpsideDown;
        float up = flip ? -1.f : 1.f;
        s.x = m_player1->getPositionX(); s.y = m_player1->getPositionY();
        s.flip = flip; s.dead = false; s.lock = -1;
        
        if (this->shipMode()) s.mode = 1;
        else if (this->ballMode()) s.mode = 2;
        else s.mode = 0;

        s.ground = (this->isOnLocalGround(s.x, s.y, flip) || m_player1->m_isOnGround) && std::abs(m_player1->m_yVelocity) < 2.0f;
        s.vy = s.ground ? 0.f : f->vy * up;

        if (f->lockIdx >= 0) {
            if (f->lockIdx < static_cast<int>(sim::boxes.size()) && sim::overlap(s.x, s.y, sim::ORB_HALF + 4.f, sim::boxes[f->lockIdx])) s.lock = f->lockIdx;
            else f->lockIdx = -1;
        }
        return s;
    }

    void drawTrajectory() {
        auto f = m_fields.self();
        if (!f->traj) return;
        if (!g.trajectory) { f->traj->clear(); return; }
        if (!this->waveMode() && !this->cubeMode() && !this->shipMode() && !this->ballMode()) { f->traj->clear(); return; }

        f->trajCounter++;
        if (!f->trajDirty && f->trajCounter < 2) return;
        f->trajCounter = 0; f->trajDirty = false; f->traj->clear();

        if (this->waveMode()) {
            auto s = currentSimState(); waveSim::S ws; ws.x = s.x; ws.y = s.y; ws.dir = f->vy >= 0 ? 1 : -1;
            int H = sim::horizon(f->vx);
            auto path = [&](bool click, ccColor4F col) {
                waveSim::S w = ws; auto prev = CCPoint{w.x, w.y}; CCPoint end = prev; bool died = false;
                for (int i = 0; i < H; i++) {
                    waveSim::step(w, f->vx, click && i == 0);
                    CCPoint p{w.x, w.y};
                    if ((i % 2) == 0 || w.dead) { f->traj->drawSegment(prev, p, 1.4f, col); prev = p; }
                    end = p; if (w.dead) { died = true; break; }
                }
                f->traj->drawDot(end, 5.f, died ? ccc4f(1.f, 0.2f, 0.2f, 1.f) : ccc4f(col.r, col.g, col.b, 0.85f));
            };
            path(false, ccc4f(1.f, 0.4f, 0.4f, 0.9f)); path(true, ccc4f(0.3f, 1.f, 0.4f, 0.9f));
        } else {
            auto start = currentSimState();
            int H = sim::horizon(f->vx);
            auto drawPath = [&](bool pressNow, int pressFrame, ccColor4F col) {
                auto s = start; auto prev = CCPoint{s.x, s.y}; CCPoint end = prev; bool died = false;
                for (int i = 0; i < H; i++) {
                    bool fire = pressNow ? (i == 0) : (i == pressFrame);
                    sim::step(s, f->vx, fire);
                    CCPoint p{s.x, s.y};
                    if ((i % 2) == 0 || s.dead) { f->traj->drawSegment(prev, p, 1.4f, col); prev = p; }
                    end = p; if (s.dead) { died = true; break; }
                }
                f->traj->drawDot(end, 5.f, died ? ccc4f(1.f, 0.2f, 0.2f, 1.f) : ccc4f(col.r, col.g, col.b, 0.85f));
            };
            drawPath(false, -1, ccc4f(1.f, 0.4f, 0.4f, 0.9f));
            if (sim::canAct(start)) drawPath(true, -1, ccc4f(0.3f, 1.f, 0.4f, 0.9f));
            if (g.bot && f->havePlan) {
                int planFrame = static_cast<int>(std::round((f->planX - start.x) / f->vx));
                if (planFrame >= 0) {
                    planFrame = std::min(planFrame, H - 1);
                    drawPath(false, planFrame, ccc4f(0.35f, 0.65f, 1.f, 0.9f));
                    f->traj->drawDot(CCPoint{start.x + f->vx * planFrame, start.y}, 6.f, ccc4f(0.35f, 0.65f, 1.f, 1.f));
                }
            }
        }
    }

    void botPress(sim::S const& s, int cooldown = 3) {
        auto f = m_fields.self();
        if (!s.ground) f->lockIdx = sim::findOrb(s);
        else f->lockIdx = -1;
        f->havePlan = false; f->botHolding = true;
        f->botRelease = 3; // HOLD FOR 3 FRAMES
        f->cooldown = cooldown; f->trajDirty = true;
        this->pressJump(true);
    }

    void tickBot() {
        auto f = m_fields.self();
        if (f->botRelease > 0 && --f->botRelease == 0) { f->botHolding = false; this->pressJump(false); }
        if (f->cooldown > 0) f->cooldown--;
        if (f->botHolding) return;
        if (sim::boxes.empty() && sim::wide.empty()) return;

        if (this->waveMode()) {
            // Wave bot logic remains the same
            float vyHint = m_player1->m_yVelocity;
            if (std::abs(vyHint) < 0.5f) vyHint = static_cast<float>(f->waveDir);
            f->waveDir = (vyHint >= 0.f) ? +1 : -1;
            waveSim::S s; s.x = m_player1->getPositionX(); s.y = m_player1->getPositionY(); s.dir = f->waveDir;
            
            // Quick lookahead
            waveSim::S probe = s; bool dies = false;
            for (int i = 0; i < 30; i++) { waveSim::step(probe, f->vx, false); if (probe.dead) { dies = true; break; } }
            if (!dies) return;
            probe = s; waveSim::step(probe, f->vx, true); dies = false;
            for (int i = 1; i < 30; i++) { waveSim::step(probe, f->vx, false); if (probe.dead) { dies = true; break; } }
            if (!dies) { f->botHolding = true; f->botRelease = 1; this->pressJump(true); return; }
            return;
        }

        auto s = this->currentSimState();
        if (!s.ground && !this->shipMode() && sim::findOrb(s) < 0) { f->havePlan = false; f->trajDirty = true; return; }

        sim::Planner p; p.vx = f->vx; p.H = sim::horizon(p.vx);
        int d = -1;
        { auto probe = s; for (int i = 0; i < p.H; i++) { sim::step(probe, p.vx, false); if (probe.dead) { d = i; break; } } }

        if (f->havePlan) {
            if (d < 0) { if (++f->planIdle > 2) { f->havePlan = false; f->trajDirty = true; } return; }
            f->planIdle = 0;
            if (sim::canAct(s) && s.x + p.vx * 0.5f >= f->planX) { this->botPress(s); return; }
            return;
        }
        if (d < 0) return;
        if (f->cooldown > 0) return;

        // BFS Pathfinding
        bool shouldPress = false;
        p.budget = 5000;
        if (p.survive(s, 0, shouldPress) && shouldPress) {
            this->botPress(s);
        }
    }

    void applyLayoutMode(bool enable) {
        if (m_objects) {
            for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
                if (obj && obj->m_objectType == GameObjectType::Decoration) obj->setVisible(!enable);
            }
        }
        if (!enable) {
            if (m_background) m_background->setColor({255, 255, 255});
            if (m_groundLayer) { m_groundLayer->updateGround01Color({255, 255, 255}); m_groundLayer->updateGround02Color({255, 255, 255}); }
        }
    }

    void forceFlatColors() {
        if (m_background) m_background->setColor({40, 62, 255});
        if (m_groundLayer) { m_groundLayer->updateGround01Color({25, 40, 170}); m_groundLayer->updateGround02Color({25, 40, 170}); }
    }
};

class $modify(StepPlayer, PlayerObject) {
    void update(float dt) {
        if (auto pl = PlayLayer::get()) {
            if (pl->m_player1 == this) static_cast<BotPlayLayer*>(pl)->onPhysicsStep();
        }
        PlayerObject::update(dt);
    }
};
