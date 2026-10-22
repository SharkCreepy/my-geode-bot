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

// Draw what the bot "sees" (cyan = solid, red = hazard, yellow = slope, purple = orb/pad/portal)
// whenever Trajectory is on. Set to false once everything looks right.
constexpr bool SHOW_SIM_BOXES = true;
// Which corner holds the right angle on an unrotated, unflipped slope:
// 0 = bottom-left, 1 = bottom-right, 2 = top-left, 3 = top-right.
// If the yellow debug triangles look mirrored, change this.
constexpr int SLOPE_BASE_CORNER = 1;
// Calibration of jump/grav from play. Off by default: it drifted the sim's jump arc
// too low on a real level, which made the bot think it couldn't clear obstacles.
constexpr bool ENABLE_CALIBRATION = false;
// Upper limit for the learned hazard margin (in level units).
constexpr float MAX_LEARNED_MARGIN = 2.f;
// Replay the best earlier attempt's presses. Off: it made every retry copy the same path.
constexpr bool ENABLE_REPLAY = false;

struct State {
    bool layout = false;
    bool trajectory = false;
    bool bot = false;
    bool cpsCounter = false;
    bool botInput = false;      // true while the bot presses, so its clicks are not counted
    float clock = 0.f;          // seconds, advanced every tick
    std::vector<float> clicks;  // times of the player's recent clicks

    void load() {
        auto m = Mod::get();
        layout     = m->getSavedValue<bool>("layout", false);
        trajectory = m->getSavedValue<bool>("trajectory", false);
        bot        = m->getSavedValue<bool>("bot", false);
        cpsCounter = m->getSavedValue<bool>("cpsCounter", false);
    }

    void save() {
        auto m = Mod::get();
        m->setSavedValue<bool>("layout", layout);
        m->setSavedValue<bool>("trajectory", trajectory);
        m->setSavedValue<bool>("bot", bot);
        m->setSavedValue<bool>("cpsCounter", cpsCounter);
    }
};

inline State g;

$on_mod(Loaded) {
    g.load();
}


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

    struct Box { float x0, y0, x1, y1; int kind; GameObject* src = nullptr; int corner = 0; float px = 0.f, py = 0.f; };

    inline std::vector<Box> boxes;
    inline std::vector<Box> wide;
    inline float maxW = 30.f;
    inline float floorY = 90.f;
    inline float ceilY = 600.f; // Give a little extra headroom
    inline int nSolid = 0, nHazard = 0, nPad = 0, nOrb = 0, nPortal = 0, nSlope = 0;

    constexpr float HALF = 15.f;
    constexpr float HAZ_HALF = 11.f; // Slightly forgiving spike hitbox
    constexpr float ORB_HALF = 12.f;
    constexpr float TERMINAL = -15.f;
    constexpr int SUB = 2;

    inline float jumpVel = 11.180032f;
    inline float grav = 0.958199f;
    inline float portalMul = 0.5f;
    inline float hazMargin = 0.f; // extra hazard clearance, used by the bot when planning

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

    inline bool overlap(float x, float y, float h, Box const& b) {
        return x + h > b.x0 && x - h < b.x1 && y + h > b.y0 && y - h < b.y1;
    }

    // Right-triangle slope: b is the bounding box, b.corner (bit0 = right, bit1 = top) is the right-angle corner.
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
            // ORB PRIORITY: Check for orbs first, even if on the ground
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
                    s.dead = true; // hit the underside of a block
                }
            });
            if (s.dead) break;

            s.x += vx * dt;
            forEachNear(s.x, [&](Box const& b) {
                if (s.dead) return;
                float u = s.flip ? -1.f : 1.f;
                switch (b.kind) {
                    case Hazard:
                        if (overlap(s.x, s.y, HAZ_HALF + hazMargin, b)) s.dead = true;
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

namespace waveSim {
    // ---- TUNABLE ----------------------------------------------------------
    constexpr float WAVE_HALF        = 8.f;   // collision half-size vs solid blocks and slopes
    constexpr float WAVE_HAZ_HALF    = 6.f;   // hazard (spike) half-size
    constexpr float WAVE_SPEED_MUL   = 1.0f;  // vertical speed relative to vx (45 degrees)
    constexpr int   WAVE_SUBSTEPS    = 2;
    // Extra room kept from the hard floor/ceiling bounds (sim::floorY / sim::ceilY).
    // Raise it if the wave clips edges, lower it if the bot hugs them too much.
    constexpr float WAVE_EDGE_MARGIN = 0.f;
    // How far ahead to plan, in frames. Longer = sees tight gaps earlier.
    constexpr int   MAXH             = 128;
    // Each frame survived is worth this many points. Clearance (max 40) only breaks ties,
    // so a path that dies earlier always loses to one that survives longer.
    constexpr float DEPTH_WEIGHT     = 10.f;
    // Score gap needed before the bot switches input (stops flicker).
    constexpr float SWITCH_MARGIN    = 4.f;
    constexpr float CLEAR_CAP        = 40.f;  // clearance beyond this is treated as "plenty"
    constexpr float CLEAR_X_RANGE    = 24.f;  // only obstacles this close in x count for clearance
    constexpr int   NODES            = (2 * MAXH + 1) * 2;
    // -----------------------------------------------------------------------

    struct S {
        float x = 0.f, y = 0.f;
        bool hold = false;
        bool flip = false;
        bool dead = false;
    };

    // Result per first input: 0 = release, 1 = hold.
    struct Option {
        bool alive = false;  // survives at least one frame
        int depth = 0;       // frames survived (max over paths), up to MAXH
        float clear = 0.f;   // best clearance seen on a path reaching that depth
        float score() const { return DEPTH_WEIGHT * depth + clear; }
    };

    inline int dirOf(bool hold, bool flip) { return (hold != flip) ? +1 : -1; }

    inline void step(S& s, float vx, bool hold) {
        if (s.dead) return;
        s.hold = hold;
        int dir = dirOf(hold, s.flip);
        float dt = 1.f / WAVE_SUBSTEPS;
        for (int i = 0; i < WAVE_SUBSTEPS && !s.dead; i++) {
            s.x += vx * dt;
            s.y += dir * vx * WAVE_SPEED_MUL * dt;
            if (s.y - WAVE_HALF < sim::floorY + WAVE_EDGE_MARGIN ||
                s.y + WAVE_HALF > sim::ceilY - WAVE_EDGE_MARGIN) {
                s.dead = true;
                return;
            }
            sim::forEachNear(s.x, [&](sim::Box const& b) {
                if (s.dead) return;
                switch (b.kind) {
                    case sim::Hazard:
                        if (sim::overlap(s.x, s.y, WAVE_HAZ_HALF, b)) s.dead = true;
                        break;
                    case sim::Solid:
                        if (sim::overlap(s.x, s.y, WAVE_HALF, b)) s.dead = true;
                        break;
                    case sim::Slope:
                        if (sim::slopeOverlap(s.x, s.y, WAVE_HALF, b)) s.dead = true;
                        break;
                    default: break;
                }
            });
        }
    }

    // Vertical gap between the wave and the nearest solid/hazard/slope that is
    // close in x. 0 = touching, CLEAR_CAP = nothing near. Higher is safer.
    inline float clearance(S const& s) {
        float best = CLEAR_CAP;
        // The floor and ceiling count as obstacles, so the bot prefers the middle of the corridor
        best = std::min(best, (s.y - WAVE_HALF) - sim::floorY);
        best = std::min(best, sim::ceilY - (s.y + WAVE_HALF));
        sim::forEachNear(s.x, [&](sim::Box const& b) {
            if (b.kind != sim::Solid && b.kind != sim::Hazard && b.kind != sim::Slope) return;
            if (s.x + CLEAR_X_RANGE < b.x0 || s.x - CLEAR_X_RANGE > b.x1) return;
            float gap;
            if (b.y1 <= s.y)      gap = (s.y - WAVE_HALF) - b.y1;   // box below
            else if (b.y0 >= s.y) gap = b.y0 - (s.y + WAVE_HALF);   // box above
            else                  gap = 0.f;                        // overlapping vertically
            best = std::min(best, std::max(gap, 0.f));
        });
        return std::clamp(best, 0.f, CLEAR_CAP);
    }

    struct Node {
        S s;
        int n = 0;
        uint8_t first = 0;     // bit h set = reached via first input h
        float minClear = 0.f;  // best (max) minimum clearance along the path so far
        bool valid = false;
    };

    // Plans H frames ahead. Fills out[0] (release first) and out[1] (hold first).
    // Each option records how far its best path survives and how much room it keeps.
    inline void plan(S start, float vx, int H, Option out[2]) {
        H = std::clamp(H, 1, MAXH);
        out[0] = Option{};
        out[1] = Option{};

        static Node bufA[NODES], bufB[NODES];
        Node* cur = bufA;
        Node* nxt = bufB;
        for (int i = 0; i < NODES; i++) cur[i] = Node{};

        int c0 = MAXH * 2 + (start.hold ? 1 : 0);
        cur[c0].s = start;
        cur[c0].n = 0;
        cur[c0].minClear = CLEAR_CAP;
        cur[c0].valid = true;

        for (int t = 0; t < H; t++) {
            for (int i = 0; i < NODES; i++) nxt[i].valid = false;
            bool any = false;

            for (int i = 0; i < NODES; i++) {
                Node const& nd = cur[i];
                if (!nd.valid) continue;
                for (int h = 0; h < 2; h++) {
                    S s = nd.s;
                    step(s, vx, h == 1);
                    if (s.dead) continue;

                    int n = nd.n + dirOf(h == 1, s.flip);
                    int key = (n + MAXH) * 2 + h;
                    if (key < 0 || key >= NODES) continue;

                    float c = std::min(nd.minClear, clearance(s));
                    uint8_t first = (t == 0) ? static_cast<uint8_t>(1 << h) : nd.first;

                    Node& d = nxt[key];
                    if (!d.valid) {
                        d.s = s; d.n = n; d.first = first; d.minClear = c; d.valid = true;
                    } else {
                        d.first |= first;
                        d.minClear = std::max(d.minClear, c);
                    }
                    any = true;

                    // Record survival depth per first input
                    for (int k = 0; k < 2; k++) {
                        if (!(first & (1 << k))) continue;
                        Option& o = out[k];
                        int depth = t + 1;
                        if (depth > o.depth || (depth == o.depth && c > o.clear)) {
                            o.depth = depth;
                            o.clear = c;
                        }
                        o.alive = true;
                    }
                }
            }

            if (!any) return; // everything dies this frame; out already holds the best depths
            std::swap(cur, nxt);
        }
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

    bool init() {
        if (!Popup::init(340.f, 290.f)) return false;
        this->setTitle("Bot Menu");

        auto menu = CCMenu::create();
        menu->setPosition({170.f, 145.f});
        m_mainLayer->addChild(menu);

        constexpr float L = -150.f, R = 15.f;
        this->addToggle(menu, "Layout Mode",   g.layout,     L, 100.f);
        this->addToggle(menu, "CPS Counter",   g.cpsCounter, R, 100.f);
        this->addToggle(menu, "Trajectory",    g.trajectory, L, 72.f);
        this->addToggle(menu, "Auto Bot",      g.bot,        R, 72.f);

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
class $modify(ClickPlayer, PlayerObject) {
    bool pushButton(PlayerButton btn) {
        if (!g.botInput && btn == PlayerButton::Jump) {
            auto pl = PlayLayer::get();
            if (pl && pl->m_player1 == this) g.clicks.push_back(g.clock);
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
// Corner of the slope's bounding box that holds the right angle (bit0 = right, bit1 = top).
// -1 = rotated by something that isn't a multiple of 90 degrees (skipped).
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

// Marker types:
//   0 = death the sim predicted (red X)
//   1 = bot had no safe plan / was forced to guess (orange X)
//   2 = death the sim did NOT predict, likely a physics or hitbox error (magenta X)
struct Marker { float x, y; int type; };

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        bool prevLayout = false;
        int ticks = 0;
        int rebuilds = 0;
        bool needRebuild = false;
        bool geometryBuilt = false;
        float lastRebuildX = -1e9f;

        bool stepHookSeen = false;
        CCLabelBMFont* cpsLabel = nullptr;
        CCLabelBMFont* warnLabel = nullptr;
        std::vector<float> deathXs; // x of every death this session, kept across retries
        int predDeathIn = -1;

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
        float planFirstX = 0.f;
        int planIdle = 0;
        int cooldown = 0;
        int planAge = 0;
        std::vector<Marker> markers;
        CCDrawNode* markerNode = nullptr;
        bool lastPredDeath = false;

        // Replay: press/toggle x positions of this attempt and the best attempt so far
        std::vector<float> curPresses, bestPresses;
        float maxX = -1e9f, bestX = -1e9f;
        size_t replayIdx = 0;
        bool replayActive = false;

        // Learned hazard margin: grows each time the sim misses a death
        float learnedMargin = 0.f;

        // Calibration of jump velocity and gravity from real play
        bool calibHave = false;
        bool calibJumpPending = false;
        float calibPrevVy = 0.f;
        float calibGrav = 0.958199f;
        float calibJump = 11.180032f;
        int retry = 0;
        int lockIdx = -1;
        int waveDir = 1;
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
            if (b.kind != sim::Solid) return;
            float by1 = flip ? -b.y0 : b.y1;
            if (x + sim::HALF > b.x0 && x - sim::HALF < b.x1 &&
                std::abs((ly - sim::HALF) - by1) < 1.5f) onBlock = true;
        });
        return onBlock;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        // Drop geometry left over from the previous level
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

        if (m_objectLayer) {
            auto node = CCDrawNode::create();
            m_objectLayer->addChild(node, 1000);
            m_fields->traj = node;
        }
        if (m_objectLayer) {
            auto mn = CCDrawNode::create();
            m_objectLayer->addChild(mn, 1001);
            m_fields->markerNode = mn;
        }

        auto cps = CCLabelBMFont::create("CPS: 0", "bigFont.fnt");
        cps->setScale(0.5f);
        cps->setPosition({win.width / 2.f, win.height - 80.f});
        cps->setVisible(g.cpsCounter);
        ui->addChild(cps, 999);
        m_fields->cpsLabel = cps;

        auto warn = CCLabelBMFont::create("", "bigFont.fnt");
        warn->setScale(0.45f);
        warn->setColor(ccColor3B{255, 60, 60});
        warn->setPosition({win.width / 2.f, win.height - 110.f});
        warn->setVisible(false);
        ui->addChild(warn, 999);
        m_fields->warnLabel = warn;

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

    // True if any solid/hazard the sim knows about has moved since the last rebuild
    // (moving platforms, moving spikes, etc).
    bool geometryMoved() {
        auto check = [](sim::Box const& b) {
            if (!b.src) return false;
            auto p = b.src->getPosition();
            return std::abs(p.x - b.px) > 0.5f || std::abs(p.y - b.py) > 0.5f;
        };
        for (auto const& b : sim::boxes) if (check(b)) return true;
        for (auto const& b : sim::wide) if (check(b)) return true;
        return false;
    }

    void buildGeometry() {
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
                // Centered shrink so upside-down / rotated spikes work.
                // Big hazards (saws, large blocks) keep their real size.
                if (r.size.width <= 30.f && r.size.height <= 30.f) {
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

            sim::Box b{r.origin.x, r.origin.y, r.origin.x + r.size.width,
                       r.origin.y + r.size.height, kind, obj};
            b.px = pos.x;
            b.py = pos.y;
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
        g.botInput = true;
        this->handleButton(down, 1, true);
        if (m_gameState.m_isDualMode && m_player2) {
            this->handleButton(down, 1, false);
        }
        g.botInput = false;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (player == m_player1) {
            m_fields->deathXs.push_back(player->getPositionX());
            int t = m_fields->lastPredDeath ? 0 : 2;
            this->addMarker(player->getPositionX(), player->getPositionY(), t);
            if (t == 2) m_fields->learnedMargin = std::min(m_fields->learnedMargin + 0.5f, MAX_LEARNED_MARGIN);
            if (m_fields->maxX > m_fields->bestX) {
                m_fields->bestX = m_fields->maxX;
                m_fields->bestPresses = m_fields->curPresses;
            }
        }
        m_fields->haveLast = false;
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        if (f->botHolding) this->pressJump(false);
        f->botHolding = false;
        f->botRelease = 0;
        f->havePlan = false;
        f->planIdle = 0;
        f->cooldown = 0;
        f->retry = 0;
        f->planAge = 0;
        f->markers.clear();
        f->lastPredDeath = false;
        f->curPresses.clear();
        f->maxX = -1e9f;
        f->replayIdx = 0;
        f->replayActive = ENABLE_REPLAY && !f->bestPresses.empty();
        f->calibHave = false;
        f->calibJumpPending = false;
        f->lockIdx = -1;
        g.clicks.clear();
        g.clock = 0.f;
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
            (f->geometryBuilt && (movedFar || timedOut || this->geometryMoved()))) {
            f->geometryBuilt = true;
            f->needRebuild = false;
            f->lastRebuildX = px;
            this->buildGeometry();
            f->lockIdx = -1;
            f->trajDirty = true;
        }

        g.clock += dt;
        this->measure(dt);
        this->updateCps();
        f->predDeathIn = -1;
        if (m_player1 && this->cubeMode()) {
            f->predDeathIn = sim::deathFrame(this->currentSimState(), f->vx, sim::horizon(f->vx));
        }
        this->updateWarning();
        if (m_player1 && !m_player1->m_isDead) f->maxX = std::max(f->maxX, m_player1->getPositionX());
        this->calibrate();

        if (!g.bot && f->botHolding) { f->botHolding = false; f->botRelease = 0; this->pressJump(false); }

        if (g.bot) this->tickBot();

        if (!g.bot && m_player1) {
            if (this->cubeMode()) {
                f->lastPredDeath = sim::deathFrame(this->currentSimState(), f->vx,
                                                   sim::horizon(f->vx)) >= 0;
            } else if (this->waveMode()) {
                waveSim::S ws;
                ws.x = m_player1->getPositionX();
                ws.y = m_player1->getPositionY();
                ws.flip = m_player1->m_isUpsideDown;
                ws.hold = f->botHolding;
                waveSim::Option o[2];
                waveSim::plan(ws, f->vx, std::min(sim::horizon(f->vx), waveSim::MAXH), o);
                f->lastPredDeath = !o[0].alive && !o[1].alive;
            }
        }
        this->drawTrajectory();
        this->drawMarkers();
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
        else if (m_player1->m_isDead) msg = "Dead, retrying";
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

    void onPhysicsStep() {
        auto f = m_fields.self();
        f->stepHookSeen = true;
        f->stepsThisTick++;
    }

    // Shows a red warning when the sim predicts a hit within about half a second
    void updateWarning() {
        auto f = m_fields.self();
        if (!f->warnLabel) return;
        bool show = (g.bot || g.trajectory) && m_player1 && !m_player1->m_isDead &&
                    f->predDeathIn >= 0 && f->predDeathIn <= 30;
        f->warnLabel->setVisible(show);
        if (show) {
            f->warnLabel->setString(fmt::format("WARNING: hit in {} frames", f->predDeathIn).c_str());
        }
    }

    void updateCps() {
        auto f = m_fields.self();
        while (!g.clicks.empty() && g.clicks.front() < g.clock - 1.f) {
            g.clicks.erase(g.clicks.begin());
        }
        if (!f->cpsLabel) return;
        f->cpsLabel->setVisible(g.cpsCounter);
        if (g.cpsCounter) {
            f->cpsLabel->setString(fmt::format("CPS: {}", g.clicks.size()).c_str());
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

    // True if a jump at x would land us in the region where an earlier attempt died
    bool nearPastDeath(float x) {
        for (float dx : m_fields.self()->deathXs) {
            if (x >= dx - 100.f && x <= dx + 5.f) return true;
        }
        return false;
    }

    void addMarker(float x, float y, int type) {
        auto f = m_fields.self();
        for (auto const& m : f->markers) {
            if (m.type == type && std::abs(m.x - x) < 12.f && std::abs(m.y - y) < 12.f) return;
        }
        if (f->markers.size() < 500) f->markers.push_back({x, y, type});
    }

    // Measures the real jump velocity and gravity from play and feeds them into the sim.
    // Gravity comes from free-fall frames (vy drops by about grav per frame).
    // Jump velocity comes from the first frame after a ground press (vy + grav ~ jumpVel).
    void calibrate() {
        auto f = m_fields.self();
        if (!ENABLE_CALIBRATION) {
            sim::jumpVel = 11.180032f;
            sim::grav = 0.958199f;
            return;
        }
        if (!m_player1 || !this->cubeMode() || m_player1->m_isUpsideDown) {
            f->calibHave = false;
            f->calibJumpPending = false;
            return;
        }
        bool ground = m_player1->m_isOnGround;
        float vy = f->vy;
        if (f->calibHave) {
            if (f->calibJumpPending) {
                float jv = vy + f->calibGrav;
                if (jv > 8.f && jv < 14.f) f->calibJump += 0.05f * (jv - f->calibJump);
                f->calibJumpPending = false;
            } else if (!ground) {
                float dv = vy - f->calibPrevVy;
                // Only plain free-fall frames: orb/pad launches show up as large positive dv
                if (dv < -0.5f && dv > -1.5f) f->calibGrav += 0.02f * (-dv - f->calibGrav);
            }
            f->calibJump = std::clamp(f->calibJump, 9.5f, 13.f);
            f->calibGrav = std::clamp(f->calibGrav, 0.6f, 1.3f);
            sim::jumpVel = f->calibJump;
            sim::grav = f->calibGrav;
        }
        f->calibPrevVy = vy;
        f->calibHave = true;
    }

    void drawMarkers() {
        auto f = m_fields.self();
        if (!f->markerNode) return;
        f->markerNode->clear();
        for (auto const& m : f->markers) {
            ccColor4F c = m.type == 0 ? ccc4f(1.f, 0.15f, 0.15f, 1.f)
                        : m.type == 1 ? ccc4f(1.f, 0.6f, 0.1f, 1.f)
                                      : ccc4f(1.f, 0.2f, 1.f, 1.f);
            float r = 8.f;
            f->markerNode->drawSegment({m.x - r, m.y - r}, {m.x + r, m.y + r}, 2.f, c);
            f->markerNode->drawSegment({m.x - r, m.y + r}, {m.x + r, m.y - r}, 2.f, c);
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

    void botPress(sim::S const& s, int cooldown = 1) {
        auto f = m_fields.self();
        if (!s.ground) {
            f->lockIdx = sim::findOrb(s);
        } else {
            f->lockIdx = -1;
        }
        f->havePlan = false;
        f->botHolding = true;
        f->botRelease = 2;
        f->cooldown = cooldown;
        f->trajDirty = true;
        f->curPresses.push_back(s.x);
        f->calibJumpPending = s.ground;
        this->pressJump(true);
    }

    // Hold state as the wave is actually being driven. Uses the bot's own
    // record when the bot is on, otherwise infers it from vertical velocity.
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

        int H = std::min(sim::horizon(f->vx), waveSim::MAXH);

        auto path = [&](bool toggleNow, ccColor4F col) {
            waveSim::S w = s;
            bool h = toggleNow ? !s.hold : s.hold;
            auto prev = CCPoint{w.x, w.y};
            CCPoint end = prev;
            bool died = false;

            for (int i = 0; i < H; i++) {
                waveSim::step(w, f->vx, (i == 0) ? h : s.hold);
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

        int H = std::min(waveSim::MAXH, std::max(sim::horizon(f->vx), 96));
        waveSim::Option opt[2];
        waveSim::plan(s, f->vx, H, opt);

        f->lastPredDeath = !opt[0].alive && !opt[1].alive;
        if (f->lastPredDeath) {
            this->addMarker(s.x, s.y, 1);
            return; // nothing survives, keep current input
        }

        // Replay the best previous attempt's toggles while they still line up
        if (f->replayActive) {
            if (f->replayIdx >= f->bestPresses.size()) {
                f->replayActive = false;
            } else {
                float rx = f->bestPresses[f->replayIdx];
                if (s.x > rx + 4.f * f->vx) {
                    f->replayActive = false;
                } else {
                    if (s.x >= rx) {
                        f->botHolding = !f->botHolding;
                        f->curPresses.push_back(s.x);
                        this->pressJump(f->botHolding);
                        f->replayIdx++;
                    }
                    return;
                }
            }
        }

        bool cur = f->botHolding;
        int curIdx = cur ? 1 : 0;
        int otherIdx = 1 - curIdx;

        // Keep the current input unless the other one is clearly better,
        // or the current one dies while the other survives.
        bool want = cur;
        if (!opt[curIdx].alive && opt[otherIdx].alive) {
            want = !cur;
        } else if (opt[otherIdx].alive &&
                   opt[otherIdx].score() > opt[curIdx].score() + waveSim::SWITCH_MARGIN) {
            want = !cur;
        }

        if (want != cur) {
            f->botHolding = want;
            f->curPresses.push_back(s.x);
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
            // leftover hold (e.g. just left wave mode) - never get stuck holding
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
        f->lastPredDeath = d >= 0;

        // Follow the best previous attempt's presses while they still line up with the sim
        if (f->replayActive) {
            if (f->replayIdx >= f->bestPresses.size()) {
                f->replayActive = false;
            } else {
                float rx = f->bestPresses[f->replayIdx];
                if (s.x > rx + 4.f * vx || (d >= 0 && d <= 2)) {
                    f->replayActive = false; // drifted or about to die: planner takes over
                } else {
                    if (act && s.x + vx * 0.5f >= rx) {
                        this->botPress(s);
                        f->replayIdx++;
                    }
                    return;
                }
            }
        }
        if (d < 0 && !f->havePlan) return; // nothing to avoid, and no plan to keep

        // Commit to the plan that is drawn on screen. Only drop it if the window was missed,
        // so the bot actually follows the blue path instead of replanning past the press point.
        if (f->havePlan) {
            if (s.x > f->planDeadlineX + vx * 2.f) {
                f->havePlan = false; // missed the window, replan below
            } else {
                // On the ground press at the planned spot. In the air (orbs), press on the first
                // frame the orb is reachable inside the planned window.
                bool inWindow = s.ground ? (s.x + vx * 0.5f >= f->planX)
                                         : (s.x + vx * 0.5f >= f->planFirstX);
                if (act && inWindow) this->botPress(s);
                return;
            }
        }
        if (f->cooldown > 0) return;
        // Nothing close enough to hit: don't plan or press yet. Only react to a real threat.
        if (d > 40) return;

        // Find the widest window of frames where a press survives.
        auto findWindow = [&](float margin, float& aimX, float& lastX) -> bool {
            sim::hazMargin = margin;
            struct Reset { ~Reset() { sim::hazMargin = 0.f; } } reset;

            int dm = sim::deathFrame(s, vx, H);
            if (dm < 0) return false;

            sim::Planner p;
            p.vx = vx;
            p.H = H;

            for (int extra : {0, 2}) { // 0 = one press is enough, 2 = allow follow-up presses
                int bestFirst = -1, bestLast = -1, runFirst = -1;
                auto w = s;
                for (int k = 0; k <= dm; k++) {
                    if (!sim::canAct(w) || this->nearPastDeath(s.x + vx * k)) {
                        runFirst = -1;
                    } else if (extra == 0 || (k & 1) == 0) {
                        auto j = w;
                        sim::step(j, vx, true);
                        p.budget = extra == 0 ? 3000 : 5000;
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
                    m_fields.self()->planFirstX = s.x + vx * bestFirst;
                    aimX = s.x + vx * (bestFirst + (bestLast - bestFirst) * 0.5f);
                    lastX = s.x + vx * bestLast;
                    return true;
                }
            }
            return false;
        };

        float aimX = 0.f, lastX = 0.f;
        // First try with extra clearance around hazards, then fall back to exact hitboxes
        bool ok = findWindow(f->learnedMargin + 3.f, aimX, lastX)
               || findWindow(f->learnedMargin, aimX, lastX)
               || findWindow(0.f, aimX, lastX);

        if (!ok) {
            // Far-away deaths are not urgent: keep waiting instead of jumping early
            if (d > 30) return;
            // Pads launch the cube by themselves, so don't add a jump on top of one
            bool onPad = false;
            sim::forEachNear(s.x, [&](sim::Box const& b) {
                if (sim::isPad(b.kind) && sim::overlap(s.x, s.y, sim::ORB_HALF, b)) onPad = true;
            });
            if (onPad) return;

            this->addMarker(s.x, s.y, 1);
            // No jump clears everything in the look-ahead. Instead of waiting until death is
            // imminent, take the press frame that survives longest (better than doing nothing).
            int bestK = -1, bestDepth = d;
            auto w = s;
            for (int k = 0; k <= d; k++) {
                if (sim::canAct(w) && !this->nearPastDeath(s.x + vx * k)) {
                    auto j = w;
                    sim::step(j, vx, true);
                    int depth = j.dead ? 0 : sim::deathFrame(j, vx, H);
                    if (depth < 0) depth = H; // survives the whole horizon
                    if (depth > bestDepth || (bestK >= 0 && depth == bestDepth)) { bestDepth = depth; bestK = k; }
                }
                sim::step(w, vx, false);
                if (w.dead) break;
            }
            if (bestK < 0) {
                if (act && d <= 6) this->botPress(s, 6); // nothing helps, last-ditch jump
                return;
            }
            aimX = s.x + vx * bestK;
            lastX = aimX;
            f->planFirstX = aimX;
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
