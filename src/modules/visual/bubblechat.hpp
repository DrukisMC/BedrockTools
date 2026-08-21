#pragma once

#include "../Module.hpp"

#include <chrono>
#include <string>
#include <unordered_map>

// Bubble Chat
// Shows a chat message in a bubble above the sender's head, like a nametag,
// but only while the message is fresh (it fades away after a few seconds).
class BubbleChatModule : public Module {
public:
    BubbleChatModule();
    ~BubbleChatModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the displayChatMessage hook.
    void handleChatMessage(void* player, const std::string& senderName, const std::string& message);
    // Restores every active nametag (used on level unload / disable).
    void clearBubbles();

    float m_duration = 4.0f;   // seconds the bubble stays visible
    bool  m_showSelf = true;   // show bubbles for your own messages too
    bool  m_showName = true;   // prefix the sender's name in the bubble
    int   m_maxLength = 48;    // truncate long messages (0 = no limit)

private:
    struct Bubble {
        std::string originalNameTag;                 // captured before we take over
        std::string text;                            // last message text
        std::chrono::steady_clock::time_point expires;
    };

    void expireBubbles();
    void restoreAll();

    std::unordered_map<void*, Bubble> m_bubbles;
    std::chrono::steady_clock::time_point m_lastCheck{};
};
