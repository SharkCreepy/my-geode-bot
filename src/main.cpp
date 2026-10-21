#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>

using namespace geode::prelude;

bool g_noclipEnabled = false;
bool g_autoClickerEnabled = false;
bool g_layoutModeEnabled = false;

class MyBotMenu : public Popup { 
protected:
    bool init() override { 
        // Set popup size to 280x180
        if (!Popup::init(280.f, 180.f)) return false;

        this->setTitle("Bot Menu");

        // Center the menu inside the popup
        auto menu = CCMenu::create();
        menu->setPosition({140.f, 90.f}); 
        this->m_mainLayer->addChild(menu); 

        // Helper to create perfectly aligned toggles
        auto createToggle = [&](const char* text, bool& value, float yPos) {
            auto label = CCLabelBMFont::create(text, "bigFont.fnt");
            label->setScale(0.6f);
            
            auto spriteOn = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
            auto spriteOff = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
            
            auto toggleBtn = CCMenuItemToggler::create(
                spriteOff, spriteOn, this, menu_selector(MyBotMenu::onToggle)
            );
            
            // Place toggle on the left side (-60), text on the right side (+20)
            toggleBtn->setPosition({-60.f, yPos});
            toggleBtn->toggle(value);
            toggleBtn->setUserData(&value); 
            menu->addChild(toggleBtn);

            label->setPosition({20.f, yPos});
            menu->addChild(label);
        };

        // Vertical spacing of 40 pixels between each option
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

class $modify(MyPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto menuBtnSprite = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        if (!menuBtnSprite) return true;
        menuBtnSprite->setScale(0.8f);
        
        auto menuBtn = CCMenuItemSpriteExtra::create(
            menuBtnSprite, nullptr, this, menu_selector(MyPlayLayer::onOpenMenu)
        );
        
        // Moved the button to the top-left corner so it doesn't clash with the Pause button
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        menuBtn->setPosition({30.f, winSize.height - 30.f});
        
        auto buttonMenu = CCMenu::create(menuBtn, nullptr);
        buttonMenu->setPosition({0, 0});
        this->addChild(buttonMenu, 100);

        return true;
    }

    void onOpenMenu(CCObject* sender) {
        MyBotMenu::create()->show();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_noclipEnabled) {
            return; // Stops you from dying
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void update(float dt) {
        PlayLayer::update(dt);

        if (g_autoClickerEnabled && m_player1) {
            m_player1->pushButton(PlayerButton::Jump);
            m_player1->releaseButton(PlayerButton::Jump);
        }
    }
};
