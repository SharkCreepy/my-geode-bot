#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/binding/GameObject.hpp>

using namespace geode::prelude;

// --- Global Toggles ---
bool g_noclipEnabled = false;
bool g_autoClickerEnabled = false;
bool g_layoutModeEnabled = false;

// --- Custom Draggable Button (Uses CCTargetedTouchDelegate) ---
class DraggableButton : public CCNode, public cocos2d::CCTargetedTouchDelegate {
protected:
    bool m_isDragging = false;
    CCPoint m_startPos;
    CCPoint m_startTouch;
    CCMenuItemSpriteExtra* m_button;

public:
    bool init(CCNode* normalSprite, CCObject* target, SEL_MenuHandler selector) {
        if (!CCNode::init()) return false;

        // Create the visual button
        m_button = CCMenuItemSpriteExtra::create(normalSprite, nullptr, target, selector);
        m_button->setPosition({0, 0});
        this->addChild(m_button);

        // Register this node to handle touches
        CCTouchDispatcher::get()->addTargetedDelegate(this, 0, true);

        return true;
    }

    void onExit() {
        // Clean up the touch delegate when the button is removed
        CCTouchDispatcher::get()->removeDelegate(this);
        CCNode::onExit();
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        auto location = touch->getLocation();
        auto local = this->convertToNodeSpace(location);
        auto size = this->getContentSize();
        if (local.x >= 0 && local.x <= size.width && local.y >= 0 && local.y <= size.height) {
            m_startPos = this->getPosition();
            m_startTouch = location;
            m_isDragging = false;
            return true;
        }
        return false;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        auto location = touch->getLocation();
        auto diff = location - m_startTouch;
        if (std::abs(diff.x) > 5.f || std::abs(diff.y) > 5.f) {
            m_isDragging = true;
        }
        if (m_isDragging) {
            auto newPos = m_startPos + diff;
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            newPos.x = std::clamp(newPos.x, 25.f, winSize.width - 25.f);
            newPos.y = std::clamp(newPos.y, 25.f, winSize.height - 25.f);
            this->setPosition(newPos);
        }
    }

    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        if (m_isDragging) {
            m_isDragging = false;
        } else {
            m_button->activate(); // Trigger the click if not dragged
        }
    }

    void ccTouchCancelled(CCTouch* touch, CCEvent* event) override {
        m_isDragging = false;
    }

    static DraggableButton* create(CCNode* normalSprite, CCObject* target, SEL_MenuHandler selector) {
        auto ret = new DraggableButton();
        if (ret && ret->init(normalSprite, target, selector)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// --- The Menu Popup ---
class MyBotMenu : public Popup { 
protected:
    bool init() override { 
        if (!Popup::init(280.f, 180.f)) return false;
        this->setTitle("Bot Menu");

        auto menu = CCMenu::create();
        menu->setPosition({140.f, 90.f}); 
        this->m_mainLayer->addChild(menu); 

        auto createToggle = [&](const char* text, bool& value, float yPos) {
            auto label = CCLabelBMFont::create(text, "bigFont.fnt");
            label->setScale(0.6f);
            
            auto spriteOn = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
            auto spriteOff = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
            
            auto toggleBtn = CCMenuItemToggler::create(
                spriteOff, spriteOn, this, menu_selector(MyBotMenu::onToggle)
            );
            
            toggleBtn->setPosition({-60.f, yPos});
            toggleBtn->toggle(value);
            toggleBtn->setUserData(&value); 
            menu->addChild(toggleBtn);

            label->setPosition({20.f, yPos});
            menu->addChild(label);
        };

        createToggle("Noclip", g_noclipEnabled, 40.f);
        createToggle("Autoclicker", g_autoClickerEnabled, 0.f);
        createToggle("Layout Mode", g_layoutModeEnabled, -40.f);

        return true;
    }

    void onToggle(CCObject* sender) {
        auto toggle = static_cast<CCMenuItemToggler*>(sender);
        if (toggle) {
            bool* valuePtr = static_cast<bool*>(toggle->getUserData());
            if (valuePtr) {
                *valuePtr = !toggle->isToggled();
            }
        }
    }

public:
    static MyBotMenu* create() {
        auto ret = new MyBotMenu();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// --- Hook into the Gameplay Layer ---
class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        bool m_prevLayoutMode = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto menuBtnSprite = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        if (!menuBtnSprite) return true;
        
        menuBtnSprite->setScale(0.9f);
        auto bg = cocos2d::extension::CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({45, 45});
        bg->setOpacity(100);
        bg->addChild(menuBtnSprite);
        menuBtnSprite->setPosition(bg->getContentSize() / 2);

        auto menuBtn = DraggableButton::create(bg, this, menu_selector(MyPlayLayer::onOpenMenu));
        
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        menuBtn->setPosition({40.f, winSize.height - 40.f});
        
        // Z-order 9999 ensures it renders above all other UI elements
        this->addChild(menuBtn, 9999); 

        return true;
    }

    void onOpenMenu(CCObject* sender) {
        MyBotMenu::create()->show();
    }

    // --- Noclip Logic ---
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_noclipEnabled) {
            return; 
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void update(float dt) {
        PlayLayer::update(dt);

        // --- Autoclicker Logic ---
        if (g_autoClickerEnabled && m_player1) {
            m_player1->pushButton(PlayerButton::Jump);
            m_player1->releaseButton(PlayerButton::Jump);
        }

        // --- Layout Mode Logic ---
        if (g_layoutModeEnabled != m_fields->m_prevLayoutMode) {
            m_fields->m_prevLayoutMode = g_layoutModeEnabled;
            applyLayoutMode(g_layoutModeEnabled);
        }
    }

    void applyLayoutMode(bool enable) {
        // 1. Hide/Show Decoration Objects
        if (m_objects) {
            for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
                if (obj->m_objectType == GameObjectType::Decoration) {
                    obj->setVisible(!enable);
                }
            }
        }

        // 2. Change Background Color
        if (m_background) {
            if (enable) {
                m_background->setColor({40, 62, 255}); 
            } else {
                m_background->setColor({255, 255, 255}); 
            }
        }
    }
};
