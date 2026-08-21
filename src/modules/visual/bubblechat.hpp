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
    void setMsgAbove(bool above);
    void applyBubbles(bool apply);
    void resetState();
    void ensureNametagPatch();
    void removeNametagPatch();

    // Separate-bubble mode: draws the message in its own panel ABOVE the
    // nametag (does not touch the nametag text). Hooked on
    // NameTagRenderer::render (vtable slot 17).
    void renderSeparateBubble(void* self, void* uiCtx, void* uiControl, void* uiAnchor);

    struct Bubble { std::string message; float timer; };
    std::unordered_map<std::string, std::deque<Bubble>> m_bubbles;
    std::mutex m_mutex;
    int m_duration = 5;
    bool m_msgAboveName = false;
    bool m_separateBubble = false;

private:
    struct Override { std::string original; std::string applied; };
    std::unordered_map<void*, Override> m_overrides;
    std::chrono::steady_clock::time_point m_lastFrame;

    void* m_nametagPatchTarget = nullptr;
    uint8_t m_nametagOrigBytes[4] = {0, 0, 0, 0};
    bool m_hasOrigBytes = false;
    bool m_nametagPatchedByUs = false;
};
