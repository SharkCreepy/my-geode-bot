#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/ui/Popup.hpp>

using namespace geode::prelude;

bool g_noclipEnabled = false;
bool g_autoClickerEnabled = false;
bool g_layoutModeEnabled = false;

// --- Custom Draggable Button Class ---
class DraggableButton : public CCMenuItemSpriteExtra {
protected:
    bool m_isDragging = false;
    CCPoint m_startPos;
    CCPoint m_startTouch;

public:
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        if (!CCMenuItemSpriteExtra::ccTouchBegan(touch, event)) return false;
        m_isDragging = false;
        m_startPos = this->getPosition();
        m_startTouch = touch->getLocation();
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        auto diff = touch->getLocation() - m_startTouch;
        
        // If we move more than 5 pixels, it's a drag, not a click
        if (std::abs(diff.x) > 5.f || std::abs(diff.y) > 5.f) {
            m_isDragging = true;
        }

        if (m_isDragging) {
            auto newPos = m_startPos + diff;
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            
            // Clamp to screen so you can't drag it off-screen
            newPos.x = std::clamp(newPos.x, 25.f, winSize.width - 25.f);
            newPos.y = std::clamp(newPos.y, 25.f, winSize.height - 25.f);
            
            this->setPosition(newPos);
        }
    }

    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        if (m_isDragging) {
            m_isDragging = false; // Stop dragging, do NOT trigger the click
        } else {
            CCMenuItemSpriteExtra::ccTouchEnded(touch, event);
        }
    }

    static DraggableButton* create(CCNode* normalSprite, CCNode* selectedSprite, CCObject* target, SEL_MenuHandler selector) {
        auto ret = new DraggableButton();
        if (ret && ret->init(normalSprite, selectedSprite, target, selector)) {
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
        
        // Make the button look a bit cooler with a scale and a subtle background
        menuBtnSprite->setScale(0.9f);
        auto bg = cocos2d::extension::CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({45, 45});
        bg->setOpacity(100);
        bg->addChild(menuBtnSprite);
        menuBtnSprite->setPosition(bg->getContentSize() / 2);

        // Use our custom DraggableButton!
        auto menuBtn = DraggableButton::create(bg, nullptr, this, menu_selector(MyPlayLayer::onOpenMenu));
        
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        menuBtn->setPosition({40.f, winSize.height - 40.f});
        
        auto buttonMenu = CCMenu::create(menuBtn, nullptr);
        buttonMenu->setPosition({0, 0});
        // Z-order 9999 ensures it renders ABOVE the pause button and everything else
        this->addChild(buttonMenu, 9999); 

        return true;
    }

    void onOpenMenu(CCObject* sender) {
        MyBotMenu::create()->show();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_noclipEnabled) {
            return; 
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void update(float dt) {
        PlayLayer::update(dt);

        // Autoclicker Logic
        if (g_autoClickerEnabled && m_player1) {
            m_player1->pushButton(PlayerButton::Jump);
            m_player1->releaseButton(PlayerButton::Jump);
        }

        // Layout Mode Logic (Only apply when the toggle changes to prevent lag)
        if (g_layoutModeEnabled != m_fields->m_prevLayoutMode) {
            m_fields->m_prevLayoutMode = g_layoutModeEnabled;
            applyLayoutMode(g_layoutModeEnabled);
        }
    }

    void applyLayoutMode(bool enable) {
        // 1. Hide/Show Decorations
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
                // Solid blue color for layout mode
                m_background->setColor({40, 62, 255}); 
            } else {
                // Reset to white (original colors)
                m_background->setColor({255, 255, 255}); 
            }
        }
    }
};

// --- Hook GameObject to hide new decorations that spawn AFTER you toggle it ---
class $modify(MyGameObject, GameObject) {
    void addToLayer() {
        GameObject::addToLayer();
        if (g_layoutModeEnabled && this->m_objectType == GameObjectType::Decoration) {
            this->setVisible(false);
        }
    }
};
