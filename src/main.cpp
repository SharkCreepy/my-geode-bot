#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/ui/Popup.hpp>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>

using namespace geode::prelude;

#if defined(GEODE_IS_ANDROID) || defined(GEODE_IS_IOS)
constexpr float DEFAULT_HUD_SCALE = 1.3f;
#else
constexpr float DEFAULT_HUD_SCALE = 1.0f;
#endif

// Draw what the bot "sees" (cyan = solid, red = hazard, yellow = slope, purple = orb/pad/portal)
// whenever Trajectory is on. Set to false once everything looks right.
constexpr bool SHOW_SIM_BOXES = true;
// Which corner holds the right angle on an unrotated, unflipped slope:
// 0 = bottom-left, 1 = bottom-right, 2 = top-left, 3 = top-right.
// If the yellow debug triangles look mirrored, change this.
constexpr int SLOPE_BASE_CORNER = 1;

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
// Cube physics simulator
// ===============================================================
namespace sim {
    enum Kind : int {
        Solid = 0, Hazard,
        PortalNormal, PortalInverse,
        PadYellow, PadPink, PadRed, PadBlue,
        OrbYellow, OrbPink, OrbRed, OrbBlue, OrbGreen,
        Slope,
        KindCount
    };

    struct Box { float x0, y0, x1, y1; int kind; GameObject* src = nullptr; int corner = 0; bool circular = false; };

    inline std::vector<Box> boxes;
    inline std::vector<Box> wide;
    inline float maxW = 30.f;
    inline float floorY = 90.f;
    inline float ceilY = 600.f;
    inline int nSolid = 0, nHazard = 0, nPad = 0, nOrb = 0, nPortal = 0, nSlope = 0;

    constexpr float HALF = 15.f;
    constexpr float HAZ_HALF = 11.f;
    constexpr float ORB_HALF = 12.f;
    constexpr float TERMINAL = -15.f;
    constexpr int SUB = 2;

    inline float jumpVel = 11.180032f;
    inline float grav = 0.958199f;
    inline float portalMul = 0.5f;
    inline float hazMargin = 0.f;

    inline float launch[KindCount] = {
        0.f, 0.f, 0.f, 0.f,
        1.40f, 0.93f, 1.79f, -0.9f,
        1.00f, 0.68f, 1.30f, -1.1f, -1.3f
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

    inline bool isSawId(int id) {
        switch (id) {
            case 88: case 89: case 98:
            case 183: case 184: case 185: case 186:
            case 675: case 676: case 677: case 678: case 679: case 680:
            case 685: case 686:
            case 2586: case 3604: case 5005:
                return true;
            default: return false;
        }
    }

    inline bool overlap(float x, float y, float h, Box const& b) {
        return x + h > b.x0 && x - h < b.x1 && y + h > b.y0 && y - h < b.y1;
    }

    inline bool circleOverlapBox(float px, float py, float ph, float cx, float cy, float r) {
        float nx = std::clamp(cx, px - ph, px + ph);
        float ny = std::clamp(cy, py - ph, py + ph);
        float dx = cx - nx, dy = cy - ny;
        return dx * dx + dy * dy < r * r;
    }

    inline bool hitsHazard(float x, float y, float half_, float margin, Box const& b) {
        if (b.circular) {
            float cx = (b.x0 + b.x1) * 0.5f;
            float cy = (b.y0 + b.y1) * 0.5f;
            float r = std::min(b.x1 - b.x0, b.y1 - b.y0) * 0.42f + margin;
            return circleOverlapBox(x, y, half_, cx, cy, r);
        }
        return overlap(x, y, half_ + margin, b);
    }

    inline bool slopeOverlap(float x, float y, float h, Box const& b) {
        if (!overlap(x, y, h, b)) return false;
        float W = std::max(b.x1 - b.x0, 1.f), H = std::max(b.y1 - b.y0, 1.f);
        bool right = (b.corner & 1) != 0, top = (b.corner & 2) != 0;
        float u = std::max(right ? (b.x1 - (x + h)) : ((x - h) - b.x0), 0.f);
        float v = std::max(top   ? (b.y1 - (y + h)) : ((y - h) - b.y0), 0.f);
        return u / W + v / H < 1.f;
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
            int o = findOrb(s);
            if (o >= 0) {
                activate(s, boxes[o], o);
            } else if (s.ground) {
                s.vy = jumpVel;
                s.ground = false;
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
                    ly = by0 - HALF;
                    s.y = ly * up;
                    s.vy = 0.f;
                }
            });
            if (s.dead) break;

            s.x += vx * dt;
            forEachNear(s.x, [&](Box const& b) {
                if (s.dead) return;
                float u = s.flip ? -1.f : 1.f;
                switch (b.kind) {
                    case Hazard:
                        if (hitsHazard(s.x, s.y, HAZ_HALF, hazMargin, b)) s.dead = true;
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
                    case Slope: {
                        if (overlap(s.x, s.y, HALF, b)) {
                            float W = b.x1 - b.x0, Hh = b.y1 - b.y0;
                            if (W > 1.f && Hh > 1.f) {
                                float t = (s.x - b.x0) / W;
                                bool right = (b.corner & 1) != 0;
                                bool top   = (b.corner & 2) != 0;
                                float surfY = top
                                    ? (right ? (b.y1 - Hh * t) : (b.y0 + Hh * t))
                                    : (right ? (b.y0 + Hh * t) : (b.y1 - Hh * t));
                                
                                if (!s.flip) {
                                    if (s.y - HALF < surfY - 2.f) {
                                        s.dead = true; // Hit from below
                                    } else {
                                        s.y = surfY + HALF;
                                        if (s.vy < 0.f) s.vy = 0.f;
                                        s.ground = true;
                                    }
                                } else {
                                    if (s.y + HALF > surfY + 2.f) {
                                        s.dead = true;
                                    } else {
                                        s.y = surfY - HALF;
                                        if (s.vy > 0.f) s.vy = 0.f;
                                        s.ground = true;
                                    }
                                }
                            } else {
                                s.dead = true;
                            }
                        }
                        break;
                    }
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
        return std::clamp(static_cast<int>(550.f / std::max(vx, 1.f)), 55, 120);
    }

    inline int deathFrame(S s, float vx, int H) {
        for (int i = 0; i < H; i++) {
            step(s, vx, false);
            if (s.dead) return i;
        }
        return -1;
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

// ===============================================================
// Wave physics (hold = up, release = down; flipped gravity inverts)
// ===============================================================
namespace waveSim {
    constexpr float HALF = 4.f;       // Tighter hitbox
    constexpr float HAZ_HALF = 4.f;   // Tighter hazard check
    constexpr int SUB = 4;            // Finer sub-steps
    constexpr int MAXH = 80;          // More lookahead

    struct S {
        float x = 0.f, y = 0.f;
        bool hold = false;
        bool flip = false;
        bool dead = false;
    };

    inline int dirOf(bool hold, bool flip) { return (hold != flip) ? +1 : -1; }

    inline void step(S& s, float vx, bool hold) {
        if (s.dead) return;
        s.hold = hold;
        int dir = dirOf(hold, s.flip);

        constexpr float dt = 1.f / SUB;
        for (int i = 0; i < SUB && !s.dead; i++) {
            s.x += vx * dt;
            s.y += dir * vx * dt;

            if (s.y - HALF < sim::floorY || s.y + HALF > sim::ceilY) { s.dead = true; return; }

            sim::forEachNear(s.x, [&](sim::Box const& b) {
                if (s.dead) return;
                if (b.kind == sim::Hazard) {
                    if (sim::hitsHazard(s.x, s.y, HAZ_HALF, 0.f, b)) s.dead = true;
                } else if (b.kind == sim::Solid) {
                    if (sim::overlap(s.x, s.y, HALF, b)) s.dead = true;
                } else if (b.kind == sim::Slope) {
                    if (sim::slopeOverlap(s.x, s.y, HALF, b)) s.dead = true;
                }
            });
        }
    }

    struct Node {
        S s;
        int n = 0;          
        uint8_t first = 0;  
        bool valid = false;
    };

    inline int plan(S start, float vx, int H) {
        H = std::min(H, MAXH);
        constexpr int N = (2 * MAXH + 1) * 2;
        static Node bufA[N], bufB[N];
        Node* cur = bufA;
        Node* nxt = bufB;

        for (int i = 0; i < N; i++) { cur[i] = Node{}; }
        int c0 = MAXH * 2 + (start.hold ? 1 : 0);
        cur[c0].s = start;
        cur[c0].n = 0;
        cur[c0].valid = true;

        int lastMask = 0;
        for (int t = 0; t < H; t++) {
            for (int i = 0; i < N; i++) nxt[i].valid = false;
            int mask = 0;
            for (int i = 0; i < N; i++) {
                Node const& nd = cur[i];
                if (!nd.valid) continue;
                for (int h = 0; h < 2; h++) {
                    S s = nd.s;
                    step(s, vx, h == 1);
                    if (s.dead) continue;
                    int n = nd.n + dirOf(h == 1, s.flip);
                    int key = (n + MAXH) * 2 + h;
                    uint8_t first = (t == 0) ? static_cast<uint8_t>(1 << h) : nd.first;
                    Node& d = nxt[key];
                    if (!d.valid) {
                        d.s = s; d.n = n; d.first = first; d.valid = true;
                    } else {
                        d.first |= first;
                    }
                    mask |= first;
                }
            }
            if (mask == 0) return lastMask;
            lastMask = mask;
            std::swap(cur, nxt);
        }
        return lastMask;
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
static int slopeCorner(GameObject* obj) {
    float q90 = obj->getRotation() / 90.f;
    if (std::abs(q90 - std::round(q90)) > 0.03f) return -1;
    int cx = SLOPE_BASE_CORNER & 1, cy = (SLOPE_BASE_CORNER >> 1) & 1;
    if (obj->isFlipX()) cx = 1 - cx;
    if (obj->isFlipY()) cy = 1 - cy;
    int q = static_cast<int>(std::lround(q90)) % 4;
    if (q < 0) q += 4;
    for (int i = 0; i < q; i++) { int ncx = cy; int ncy = 1 - cx; cx = ncx; cy = ncy; }
    return cx | (cy << 1);
}

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        bool prevLayout = false;
        int ticks = 0;
        int rebuilds = 0;
        bool needRebuild = false;
        bool geometryBuilt = false;
        float lastRebuildX = -1e9f;

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

        bool stepInit = false;
        float stepPrevX = 0.f, stepPrevY = 0.f, stepPrevAbs = 0.f;
        bool stepPrevGround = true, stepPrevFlip = false;

        CCDrawNode* traj = nullptr;

        bool  trajDirty = true;
        int   trajCounter = 0;
        bool  trajPrevGround = true;
        bool  trajPrevFlip = false;
        float trajPrevVY = 0.f;
        int   trajPrevWaveDir = 1;
        bool  trajPrevWasWave = false;

        int botRelease = 0;
        bool botHolding = false;
        bool havePlan = false;
        float planX = 0.f;
        float planDeadlineX = 0.f;
        int planIdle = 0;
        int cooldown = 0;
        int planAge = 0;
        int retry = 0;
        int lockIdx = -1;
        int waveDir = 1;
        CCLabelBMFont* status = nullptr;
        std::string lastStatus;

        float lastDeathX = -1e9f;
        int   lastDeathJumpBit = 0;
        float lastPressX = -1e9f;
    };

    bool isOnLocalGround(float x, float y, bool flip) {
        float up = flip ? -1.f : 1.f;
        float ly = y * up;
        float floorL = flip ? -sim::ceilY : sim::floorY;

        if (std::abs((ly - sim::HALF) - floorL) < 2.5f) return true;

        bool onBlock = false;
        sim::forEachNear(x, [&](sim::Box const& b) {
            if (b.kind != sim::Solid) return;
            float by1 = flip ? -b.y0 : b.y1;
            if (x + sim::HALF > b.x0 && x - sim::HALF < b.x1 + 2.0f && 
                std::abs((ly - sim::HALF) - by1) < 3.0f) onBlock = true;
        });
        
        bool onSlope = false;
        if (!onBlock) {
            sim::forEachNear(x, [&](sim::Box const& b) {
                if (onSlope || b.kind != sim::Slope) return;
                if (x < b.x0 || x > b.x1) return;
                float W = b.x1 - b.x0, Hh = b.y1 - b.y0;
                if (W < 1.f || Hh < 1.f) return;
                float t = (x - b.x0) / W;
                bool right = (b.corner & 1) != 0;
                bool top   = (b.corner & 2) != 0;
                if (top) return;   
                float surfY = right ? (b.y0 + Hh * t) : (b.y1 - Hh * t);
                if (std::abs((ly - sim::HALF) - surfY) < 3.5f) onSlope = true;
            });
        }
        return onBlock || onSlope;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        sim::boxes.clear();
        sim::wide.clear();

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
        std::unordered_map<GameObject*, std::pair<float,float>> oldPos;
        for (auto const& b : sim::boxes) {
            if (b.src) oldPos[b.src] = {b.x0, b.y0};
        }

        sim::boxes.clear();
        sim::wide.clear();
        sim::maxW = 30.f;
        sim::floorY = 90.f;
        sim::nSolid = sim::nHazard = sim::nPad = sim::nOrb = sim::nPortal = sim::nSlope = 0;
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
                default:
                    if (obj->m_objectType == GameObjectType::Hazard) kind = sim::Hazard;
                    else if (obj->m_objectType == GameObjectType::Solid) kind = sim::Solid;
                    else if (obj->m_objectType == GameObjectType::InverseGravityPortal) kind = sim::PortalInverse;
                    else if (obj->m_objectType == GameObjectType::NormalGravityPortal) kind = sim::PortalNormal;
                    else if (obj->m_objectType == GameObjectType::Slope) kind = sim::Slope;
            }
            if (kind < 0) continue;

            CCRect r;

            if (sim::isOrb(kind)) {
                r = obj->getObjectRect();
                r.origin.x += 2.f; r.origin.y += 2.f; r.size.width -= 4.f; r.size.height -= 4.f;
            } else if (sim::isPad(kind)) {
                r = obj->getObjectRect();
                r.origin.y -= 2.f;
                r.size.height += 4.f;
            } else if (kind == sim::Hazard) {
                r = obj->getObjectRect();
                bool isSaw = sim::isSawId(obj->m_objectID) ||
                             (std::abs(r.size.width - r.size.height) < 3.f &&
                              r.size.width >= 25.f);
                if (!isSaw && r.size.width <= 30.f && r.size.height <= 30.f) {
                    float cx = r.origin.x + r.size.width * 0.5f;
                    float cy = r.origin.y + r.size.height * 0.5f;
                    float w = std::min(r.size.width, 15.f);
                    float h = std::min(r.size.height, 15.f);
                    r = CCRect(cx - w * 0.5f, cy - h * 0.5f, w, h);
                }
            } else {
                r = obj->getObjectRect();
                float mx = r.origin.x + r.size.width * 0.5f;
                float my = r.origin.y + r.size.height * 0.5f;
                bool valid = r.size.width >= 1.f && r.size.height >= 1.f &&
                             std::abs(mx - pos.x) < 80.f && std::abs(my - pos.y) < 80.f;
                if (!valid) {
                    auto bb = obj->boundingBox();
                    if (sim::isPortal(kind)) {
                        r = CCRect(pos.x - 10.f, bb.origin.y, 20.f, bb.size.height);
                    } else {
                        r = bb;
                    }
                }
            }
            if (r.size.width <= 0.f || r.size.height <= 0.f) continue;

            bool isDynamic = false;
            auto oit = oldPos.find(obj);
            if (oit != oldPos.end()) {
                if (std::abs(oit->second.first  - r.origin.x) > 2.0f ||
                    std::abs(oit->second.second - r.origin.y) > 2.0f) {
                    isDynamic = true;
                }
            }
            // Do NOT skip hazards. Only skip moving solids.
            if (isDynamic && kind == sim::Solid) {
                continue;
            }

            sim::Box b{r.origin.x, r.origin.y, r.origin.x + r.size.width,
                       r.origin.y + r.size.height, kind, obj};
            if (kind == sim::Hazard) {
                b.circular = sim::isSawId(obj->m_objectID) ||
                             (std::abs(r.size.width - r.size.height) < 3.f &&
                              r.size.width >= 25.f);
            }
            if (kind == sim::Slope) {
                int c = slopeCorner(obj);
                if (c < 0) continue;
                b.corner = c;
            }
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
            else if (kind == sim::Slope) sim::nSlope++;
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
        auto f = m_fields.self();
        if (player) {
            float px = player->getPositionX();
            f->lastDeathX = px;
            f->lastDeathJumpBit = (std::abs(f->lastPressX - px) < 60.f) ? 1 : 0;
        }
        if (g.noclip) {
            m_fields->hitFlag = true;
            return;
        }
        m_fields->haveLast = false;
        m_fields->vy = 0.f;
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
        f->planIdle = 0;
        f->cooldown = 0;
        f->retry = 0;
        f->planAge = 0;
        f->lockIdx = -1;
        f->deaths = 0;
        f->totalSteps = 0;
        f->deadSteps = 0;
        f->stepsSinceHit = 1000;
        f->hitFlag = false;
        f->stepInit = false;
        f->haveLast = false;

        f->trajDirty = true;
        f->trajCounter = 0;
        f->trajPrevGround = true;
        f->trajPrevFlip = false;
        f->trajPrevVY = 0.f;
        f->trajPrevWaveDir = 1;
        f->trajPrevWasWave = false;
        f->lastRebuildX = -1e9f;

        f->lastDeathX = -1e9f;
        f->lastDeathJumpBit = 0;
        f->lastPressX = -1e9f;

        if (f->rebuilds < 2) {
            f->rebuilds++;
            f->needRebuild = true;
        }
        this->applyLayoutMode(g.layout);
    }

    void tickAll(float dt) {
        auto f = m_fields.self();
        f->ticks++;

        float px = m_player1 ? m_player1->getPositionX() : 0.f;
        bool movedFar = std::abs(px - f->lastRebuildX) > 200.f;
        bool timedOut = (f->ticks % 120 == 0);

        if ((!f->geometryBuilt && m_objects && m_objects->count() > 0) ||
            f->ticks == 20 || f->needRebuild ||
            (f->geometryBuilt && (movedFar || timedOut))) {
            f->geometryBuilt = true;
            f->needRebuild = false;
            f->lastRebuildX = px;
            this->buildGeometry();
            f->lockIdx = -1;
            f->trajDirty = true;
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
        else if (this->waveMode()) msg = fmt::format("Wave | S{} H{} L{} | {} v3", sim::nSolid, sim::nHazard, sim::nSlope, g.bot ? "bot" : "traj");
        else if (!this->cubeMode()) msg = "Unsupported gamemode (cube + wave only)";
        else if (sim::boxes.empty() && sim::wide.empty()) msg = "No level geometry found";
        else msg = fmt::format("S{} H{} L{} P{} O{} G{} | jump {:.2f}/{:.3f} | {} v3",
                               sim::nSolid, sim::nHazard, sim::nSlope, sim::nPad, sim::nOrb, sim::nPortal,
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
    }

    void tickNoclipStats() {
        auto f = m_fields.self();

        if (!f->stepHookSeen) {
            this->registerStep(false);
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

    bool waveMode() {
        auto p = m_player1;
        return p && !p->m_isDead && p->m_isDart;
    }

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

        bool geomGround = this->isOnLocalGround(s.x, s.y, flip);
        bool isMoving = std::abs(m_player1->m_yVelocity) > 2.0f;
        s.ground = (geomGround || m_player1->m_isOnGround) && !isMoving;

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

        return s;
    }

    void drawTrajectory() {
        auto f = m_fields.self();
        if (!f->traj) return;
        if (!g.trajectory) { f->traj->clear(); return; }

        bool isWave = this->waveMode();
        bool isCube = this->cubeMode();
        if (!isWave && !isCube) { f->traj->clear(); return; }

        f->trajCounter++;

        bool stateChanged = false;
        if (isWave) {
            if (!f->trajPrevWasWave) stateChanged = true;
            f->trajPrevWasWave = true;

            float vyHint = m_player1->m_yVelocity;
            if (std::abs(vyHint) < 0.5f) vyHint = static_cast<float>(f->waveDir);
            int dir = (vyHint >= 0.f) ? +1 : -1;
            f->waveDir = dir;
            if (dir != f->trajPrevWaveDir) { stateChanged = true; f->trajPrevWaveDir = dir; }
        } else {
            if (f->trajPrevWasWave) stateChanged = true;
            f->trajPrevWasWave = false;

            bool ground = m_player1->m_isOnGround;
            bool flip = m_player1->m_isUpsideDown;
            if (ground != f->trajPrevGround) stateChanged = true;
            if (flip != f->trajPrevFlip) stateChanged = true;
            if (std::abs(f->vy - f->trajPrevVY) > 3.f) stateChanged = true;
            f->trajPrevGround = ground;
            f->trajPrevFlip = flip;
            f->trajPrevVY = f->vy;
        }

        if (!stateChanged && !f->trajDirty && f->trajCounter < 2) return;

        f->trajCounter = 0;
        f->trajDirty = false;
        f->traj->clear();
        if (SHOW_SIM_BOXES) this->drawSimBoxes();

        if (isWave) {
            this->drawWaveTrajectoryBody();
        } else {
            auto start = this->currentSimState();
            this->drawCubeTrajectoryBody(start);
        }
    }

    void drawSimBoxes() {
        auto f = m_fields.self();
        if (!m_player1) return;
        float px = m_player1->getPositionX();
        auto draw = [&](sim::Box const& b) {
            if (b.x1 < px - 120.f || b.x0 > px + 500.f) return;
            ccColor4F col = ccc4f(0.2f, 0.8f, 1.f, 0.8f);
            if (b.kind == sim::Hazard) col = ccc4f(1.f, 0.2f, 0.2f, 0.9f);
            else if (b.kind == sim::Slope) col = ccc4f(1.f, 1.f, 0.2f, 0.9f);
            else if (b.kind != sim::Solid) col = ccc4f(0.8f, 0.4f, 1.f, 0.8f);
            CCPoint v[4];
            unsigned n = 4;
            if (b.kind == sim::Slope) {
                bool right = (b.corner & 1) != 0, top = (b.corner & 2) != 0;
                float cx = right ? b.x1 : b.x0, cy = top ? b.y1 : b.y0;
                v[0] = CCPoint{cx, cy};
                v[1] = CCPoint{right ? b.x0 : b.x1, cy};
                v[2] = CCPoint{cx, top ? b.y0 : b.y1};
                n = 3;
            } else {
                v[0] = CCPoint{b.x0, b.y0};
                v[1] = CCPoint{b.x1, b.y0};
                v[2] = CCPoint{b.x1, b.y1};
                v[3] = CCPoint{b.x0, b.y1};
            }
            f->traj->drawPolygon(v, n, ccc4f(0.f, 0.f, 0.f, 0.f), 1.f, col);
        };
        for (auto const& b : sim::boxes) draw(b);
        for (auto const& b : sim::wide) draw(b);
    }

    void drawCubeTrajectoryBody(sim::S const& start) {
        auto f = m_fields.self();
        int H = sim::horizon(f->vx);

        auto drawPath = [&](bool pressNow, int pressFrame, ccColor4F col) {
            auto s = start;
            auto prev = CCPoint{s.x, s.y};
            CCPoint end = prev;
            bool died = false;

            for (int i = 0; i < H; i++) {
                bool fire = pressNow ? (i == 0) : (i == pressFrame);
                sim::step(s, f->vx, fire);
                CCPoint p{s.x, s.y};
                if ((i % 2) == 0 || s.dead) {
                    f->traj->drawSegment(prev, p, 1.4f, col);
                    prev = p;
                }
                end = p;
                if (s.dead) { died = true; break; }
            }
            f->traj->drawDot(end, 5.f,
                died ? ccc4f(1.f, 0.2f, 0.2f, 1.f)
                     : ccc4f(col.r, col.g, col.b, 0.85f));
        };

        drawPath(false, -1, ccc4f(1.f, 0.4f, 0.4f, 0.9f));
        if (sim::canAct(start)) drawPath(true, -1, ccc4f(0.3f, 1.f, 0.4f, 0.9f));

        if (g.bot && f->havePlan) {
            int planFrame = static_cast<int>(std::round((f->planX - start.x) / f->vx));
            if (planFrame >= 0) {
                planFrame = std::min(planFrame, H - 1);
                drawPath(false, planFrame, ccc4f(0.35f, 0.65f, 1.f, 0.9f));
                f->traj->drawDot(CCPoint{start.x + f->vx * planFrame, start.y},
                                 6.f, ccc4f(0.35f, 0.65f, 1.f, 1.f));
            }
        }
    }

    bool waveHoldNow() {
        auto f = m_fields.self();
        if (g.bot) return f->botHolding;
        bool flip = m_player1->m_isUpsideDown;
        if (std::abs(f->vy) > 1.f) return (f->vy > 0.f) != flip;
        return f->botHolding;
    }

    void drawWaveTrajectoryBody() {
        auto f = m_fields.self();

        waveSim::S s;
        s.x = m_player1->getPositionX();
        s.y = m_player1->getPositionY();
        s.flip = m_player1->m_isUpsideDown;
        s.hold = this->waveHoldNow();

        int H = sim::horizon(f->vx);

        auto path = [&](bool toggleNow, ccColor4F col) {
            waveSim::S w = s;
            bool h = toggleNow ? !s.hold : s.hold;
            auto prev = CCPoint{w.x, w.y};
            CCPoint end = prev;
            bool died = false;

            for (int i = 0; i < H; i++) {
                waveSim::step(w, f->vx, h);
                CCPoint p{w.x, w.y};
                if ((i % 2) == 0 || w.dead) {
                    f->traj->drawSegment(prev, p, 1.4f, col);
                    prev = p;
                }
                end = p;
                if (w.dead) { died = true; break; }
            }
            f->traj->drawDot(end, 5.f,
                died ? ccc4f(1.f, 0.2f, 0.2f, 1.f)
                     : ccc4f(col.r, col.g, col.b, 0.85f));
        };

        path(false, ccc4f(1.f, 0.4f, 0.4f, 0.9f));
        path(true,  ccc4f(0.3f, 1.f, 0.4f, 0.9f));
    }

    void botPress(sim::S const& s, int cooldown = 1) {
        auto f = m_fields.self();
        if (!s.ground) {
            f->lockIdx = sim::findOrb(s);
        } else {
            f->lockIdx = -1;
        }
        f->lastPressX = s.x;
        f->havePlan = false;
        f->botHolding = true;
        f->botRelease = 2;
        f->cooldown = cooldown;
        f->trajDirty = true;
        this->pressJump(true);
    }

    void tickWaveBot() {
        auto f = m_fields.self();
        f->botRelease = 0;
        f->havePlan = false;

        if (!this->waveMode()) return;
        if (sim::boxes.empty() && sim::wide.empty()) return;

        waveSim::S s;
        s.x = m_player1->getPositionX();
        s.y = m_player1->getPositionY();
        s.flip = m_player1->m_isUpsideDown;
        s.hold = f->botHolding;

        int H = std::min(sim::horizon(f->vx), waveSim::MAXH);
        int mask = waveSim::plan(s, f->vx, H);
        if (mask == 0) return; // no survivable line, keep doing what we're doing

        bool cur = f->botHolding;
        // keep the current input if it's safe, otherwise switch
        bool want = (mask & (cur ? 2 : 1)) ? cur : !cur;
        if (want != cur) {
            f->botHolding = want;
            this->pressJump(want);
        }
    }

    void tickBot() {
        if (this->waveMode()) { this->tickWaveBot(); return; }

        auto f = m_fields.self();

        if (f->botRelease > 0) {
            if (--f->botRelease == 0) {
                f->botHolding = false;
                this->pressJump(false);
            }
        } else if (f->botHolding) {
            f->botHolding = false;
            this->pressJump(false);
        }
        if (f->cooldown > 0) f->cooldown--;

        if (!this->cubeMode() || (sim::boxes.empty() && sim::wide.empty())) return;
        if (f->botHolding) return;

        float vx = f->vx;
        int H = sim::horizon(vx);
        auto s = this->currentSimState();
        bool act = sim::canAct(s);

        int d = sim::deathFrame(s, vx, H);
        if (d < 0) { f->havePlan = false; return; }

        if (f->havePlan) {
            if (++f->planAge >= 4 || s.x > f->planDeadlineX) f->havePlan = false;
        }
        if (f->havePlan) {
            if (act && (s.x + vx * 0.5f >= f->planX || d <= 1)) this->botPress(s);
            return;
        }
        if (f->cooldown > 0) return;

        auto findWindow = [&](float margin, float& aimX, float& lastX) -> bool {
            sim::hazMargin = margin;
            struct Reset { ~Reset() { sim::hazMargin = 0.f; } } reset;

            int dm = sim::deathFrame(s, vx, H);
            if (dm < 0) return false;

            sim::Planner p;
            p.vx = vx;
            p.H = H;

            for (int extra : {0, 2}) {
                int bestFirst = -1, bestLast = -1, runFirst = -1;
                auto w = s;
                for (int k = 0; k <= dm; k++) {
                    if (!sim::canAct(w)) {
                        runFirst = -1;
                    } else if (extra == 0 || (k & 1) == 0) {
                        auto j = w;
                        sim::step(j, vx, true);
                        p.budget = extra == 0 ? 10000 : 15000;
                        if (!j.dead && p.survive(j, 1, extra)) {
                            if (runFirst < 0) runFirst = k;
                            if (bestFirst < 0 || (k - runFirst) >= (bestLast - bestFirst)) {
                                bestFirst = runFirst;
                                bestLast = k;
                            }
                        } else {
                            runFirst = -1;
                        }
                    }
                    sim::step(w, vx, false);
                    if (w.dead) break;
                }
                if (bestFirst >= 0) {
                    aimX = s.x + vx * (bestFirst + (bestLast - bestFirst) * 0.5f);
                    lastX = s.x + vx * bestLast;
                    return true;
                }
            }
            return false;
        };

        float aimX = 0.f, lastX = 0.f;
        bool nearDeath = std::abs(s.x - f->lastDeathX) < 50.f;
        float margin = nearDeath ? 6.f : 3.f;
        
        bool ok = findWindow(margin, aimX, lastX);
        if (!ok) ok = findWindow(0.f, aimX, lastX);

        if (!ok) {
            if (act && d <= 3) this->botPress(s, 6);
            return;
        }

        if (nearDeath && f->lastDeathJumpBit == 1 && act && d > 2) {
            return;
        }

        if (act && s.x + vx * 0.5f >= aimX) {
            this->botPress(s);
            return;
        }

        f->havePlan = true;
        f->planX = aimX;
        f->planDeadlineX = lastX + vx * 0.5f;
        f->planIdle = 0;
        f->planAge = 0;
        f->trajDirty = true;
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
