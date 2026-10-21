#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>

using namespace geode::prelude;

// --- Simple variables to hold the state of your cheats ---
bool g_noclipEnabled = false;
bool g_autoClickerEnabled = false;
bool g_layoutModeEnabled = false; // Placeholder for the layout feature

// --- The Popup Menu Class ---
class MyBotMenu : public Popup<> {
protected:
    bool setup() override {
        // Set the popup title and size
        this->setTitle("Bot Menu");
        this->setContentSize({280.f, 180.f});

        // Create a menu to hold our toggle buttons
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu);

        // Helper lambda to create a toggle button
        auto createToggle = [&](const char* text, bool& value, const CCPoint& pos) {
            auto label = CCLabelBMFont::create(text, "bigFont.fnt");
            label->setScale(0.6f);
            
            // Using a standard checkbox sprite from the game
            auto spriteOn = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
            auto spriteOff = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
            
            auto toggleBtn = CCMenuItemToggler::create(
                spriteOff, spriteOn, this, menu_selector(MyBotMenu::onToggle)
            );
            toggleBtn->setPosition(pos);
            toggleBtn->toggle(value); // Set initial state
            // We store a pointer to the bool in the button's user data
            toggleBtn->setUserData(&value); 
            menu->addChild(toggleBtn);

            label->setPosition(pos + CCPoint{30.f, 0.f});
            menu->addChild(label);
        };

        // Add the toggles
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
                *valuePtr = !toggle->isToggled(); // Update the global state
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
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        // Create the button to open our menu
        auto menuBtnSprite = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        menuBtnSprite->setScale(0.8f);
        auto menuBtn = CCMenuItemSpriteExtra::create(
            menuBtnSprite, nullptr, this, menu_selector(MyPlayLayer::onOpenMenu)
        );
        
        // Add it to the screen, positioned near the top-right
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        menuBtn->setPosition({winSize.width - 30.f, winSize.height - 30.f});
        
        auto buttonMenu = CCMenu::create(menuBtn, nullptr);
        buttonMenu->setPosition({0, 0});
        this->addChild(buttonMenu, 100); // High Z-order to appear on top

        return true;
    }

    void onOpenMenu(CCObject* sender) {
        MyBotMenu::create()->show();
    }

    // --- Hook to apply Noclip ---
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_noclipEnabled) {
            return; // Skip the original function, preventing death
        }
        PlayLayer::destroyPlayer(player, object);
    }

    // --- Hook for the Autoclicker logic ---
    void update(float dt) {
        PlayLayer::update(dt);

        if (g_autoClickerEnabled && m_player1) {
            // This is a very basic autoclicker that spams the jump button.
            // It doesn't check if the player is on the ground.
            m_player1->pushButton(PlayerButton::Jump);
            m_player1->releaseButton(PlayerButton::Jump);
        }
    }
};
