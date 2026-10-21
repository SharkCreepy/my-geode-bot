#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>

using namespace geode::prelude;

bool g_noclipEnabled = false;
bool g_autoClickerEnabled = false;
bool g_layoutModeEnabled = false;

// Updated for Geode v5: No more angle brackets <>
class MyBotMenu : public Popup { 
protected:
    // Updated for Geode v5: No arguments in init()
    bool init() override { 
        // We set the size here instead
        if (!Popup::init(280.f, 180.f)) return false;

        this->setTitle("Bot Menu");

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        // Updated for Geode v5: Add to m_mainLayer instead of 'this'
        this->m_mainLayer->addChild(menu); 

        auto createToggle = [&](const char* text, bool& value, const CCPoint& pos) {
            auto label = CCLabelBMFont::create(text, "bigFont.fnt");
            label->setScale(0.6f);
            
            auto spriteOn = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
            auto spriteOff = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
            
            auto toggleBtn = CCMenuItemToggler::create(
                spriteOff, spriteOn, this, menu_selector(MyBotMenu::onToggle)
            );
            toggleBtn->setPosition(pos);
            toggleBtn->toggle(value);
            toggleBtn->setUserData(&value); 
            menu->addChild(toggleBtn);

            label->setPosition(pos + CCPoint{30.f, 0.f});
            menu->addChild(label);
        };

        createToggle("Noclip", g_noclipEnabled, {60.f, 100.f});
        createToggle("Autoclicker", g_autoClickerEnabled, {60.f, 60.f});
        createToggle("Layout Mode", g_layoutModeEnabled, {60.f, 20.f});

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
        
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        menuBtn->setPosition({winSize.width - 30.f, winSize.height - 30.f});
        
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
