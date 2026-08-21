#pragma once
#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <string>
#include <unordered_map>
#include <deque>
#include <vector>
#include <mutex>
#include <cstdint>
#include <chrono>

class BubbleChatModule : public Module {
public:
    BubbleChatModule();
    ~BubbleChatModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void* m_localPlayerPtr = nullptr;
    void addBubble(const std::string& author, const std::string& message);
    void setDuration(int secs);

    struct Bubble { std::string message; float timer; };
    std::unordered_map<std::string, std::deque<Bubble>> m_bubbles;
    std::mutex m_mutex;
    int m_duration = 5;

    // Rendering settings
    float m_fov = 70.0f;        // vertical FOV used for world->screen projection
    float m_scale = 1.0f;       // bubble size scale
    float m_heightOffset = 2.2f; // height above feet where the bubble anchors
    bool m_showOwnBubbles = true; // show bubbles for your own messages (3rd person)

private:
    std::chrono::steady_clock::time_point m_lastFrame;
};
