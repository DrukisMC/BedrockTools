#include "bubblechat.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace {

// GuiData::displayChatMessage(
//     const Player* player, ChatEvent::Name name,
//     const std::string& senderName, const std::string& message,
//     const std::string& xuid, const std::string& platformId);
using DisplayChatMessageFn = void (*)(void*, void*, int, const std::string&, const std::string&, const std::string&, const std::string&);
using ActorGetNameTagFn = std::string (*)(void*);
using ActorSetNameTagFn = void (*)(void*, const std::string&);
using LevelDtorFn = void (*)(void*);

BubbleChatModule* g_bubbleChat = nullptr;
DisplayChatMessageFn g_displayChatMessageOriginal = nullptr;
ActorGetNameTagFn g_getNameTag = nullptr;
ActorSetNameTagFn g_setNameTag = nullptr;
LevelDtorFn g_levelDtorOriginal = nullptr;

std::string truncateMessage(const std::string& message, int maxLength) {
    if (maxLength <= 0 || static_cast<int>(message.size()) <= maxLength) return message;
    return message.substr(0, static_cast<std::size_t>(maxLength)) + "...";
}

void displayChatMessageHook(void* self, void* player, int name, const std::string& senderName,
                            const std::string& message, const std::string& xuid,
                            const std::string& platformId) {
    if (g_displayChatMessageOriginal) {
        g_displayChatMessageOriginal(self, player, name, senderName, message, xuid, platformId);
    }
    if (g_bubbleChat && g_bubbleChat->enabled) {
        g_bubbleChat->handleChatMessage(player, senderName, message);
    }
}

void levelDtorHook(void* self) {
    // Safety net: when a level is destroyed the actor pointers we hold become
    // dangling, so drop every bubble before that happens.
    if (g_bubbleChat) g_bubbleChat->clearBubbles();
    if (g_levelDtorOriginal) g_levelDtorOriginal(self);
}

} // namespace

BubbleChatModule::BubbleChatModule()
    : Module("Bubble Chat", "Shows chat messages in a bubble above the sender's head, like a nametag.") {
    g_bubbleChat = this;
}

BubbleChatModule::~BubbleChatModule() {
    if (g_bubbleChat == this) g_bubbleChat = nullptr;
}

void BubbleChatModule::onInit() {
    const auto displayChat = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::GuiDataDisplayChatMessage
    );
    if (displayChat && !g_displayChatMessageOriginal) {
        bedrocktools::hooks::install(
            reinterpret_cast<void*>(displayChat),
            reinterpret_cast<void*>(displayChatMessageHook),
            reinterpret_cast<void**>(&g_displayChatMessageOriginal)
        );
    }

    g_getNameTag = reinterpret_cast<ActorGetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag)
    );
    g_setNameTag = reinterpret_cast<ActorSetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSetNameTag)
    );

    const auto levelDtor = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::LevelDtor
    );
    if (levelDtor && !g_levelDtorOriginal) {
        bedrocktools::hooks::install(
            reinterpret_cast<void*>(levelDtor),
            reinterpret_cast<void*>(levelDtorHook),
            reinterpret_cast<void**>(&g_levelDtorOriginal)
        );
    }
}

void BubbleChatModule::onEnable() {}

void BubbleChatModule::onDisable() {
    restoreAll();
}

void BubbleChatModule::handleChatMessage(void* player, const std::string& senderName, const std::string& message) {
    if (!player || !g_getNameTag || !g_setNameTag) return;
    if (message.empty()) return;

    if (!m_showSelf) {
        auto* client = bedrocktools::sdk::ClientInstance::current();
        if (client && client->localPlayer() == player) return;
    }

    const std::string text = truncateMessage(message, m_maxLength);
    const auto now = std::chrono::steady_clock::now();
    const auto lifetime = std::chrono::milliseconds(static_cast<long long>(m_duration * 1000.0f));

    auto it = m_bubbles.find(player);
    if (it == m_bubbles.end()) {
        Bubble bubble;
        bubble.originalNameTag = g_getNameTag(player);
        bubble.text = text;
        bubble.expires = now + lifetime;
        m_bubbles.emplace(player, std::move(bubble));
    } else {
        // Keep the originally captured nametag; just refresh the text + expiry.
        it->second.text = text;
        it->second.expires = now + lifetime;
    }

    std::string display;
    if (m_showName && !senderName.empty()) {
        display = std::string("\xC2\xA7") + "a" + senderName + "\xC2\xA7" + "r: " + text;
    } else {
        display = text;
    }
    g_setNameTag(player, display);
}

void BubbleChatModule::expireBubbles() {
    if (m_bubbles.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_bubbles.begin(); it != m_bubbles.end();) {
        if (now >= it->second.expires) {
            if (g_setNameTag) g_setNameTag(it->first, it->second.originalNameTag);
            it = m_bubbles.erase(it);
        } else {
            ++it;
        }
    }
}

void BubbleChatModule::restoreAll() {
    if (g_setNameTag) {
        for (const auto& [player, bubble] : m_bubbles) {
            g_setNameTag(player, bubble.originalNameTag);
        }
    }
    m_bubbles.clear();
}

void BubbleChatModule::clearBubbles() {
    restoreAll();
}

void BubbleChatModule::onFrame() {
    if (!enabled) return;
    const auto now = std::chrono::steady_clock::now();
    if (m_lastCheck == std::chrono::steady_clock::time_point{} ||
        now - m_lastCheck >= std::chrono::milliseconds(100)) {
        m_lastCheck = now;
        expireBubbles();
    }
}

void BubbleChatModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_duration")) m_duration = std::clamp(j["m_duration"].get<float>(), 0.5f, 60.0f);
    if (j.contains("m_showSelf")) m_showSelf = j["m_showSelf"].get<bool>();
    if (j.contains("m_showName")) m_showName = j["m_showName"].get<bool>();
    if (j.contains("m_maxLength")) m_maxLength = std::clamp(j["m_maxLength"].get<int>(), 4, 256);
}

void BubbleChatModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_duration"] = m_duration;
    j["m_showSelf"] = m_showSelf;
    j["m_showName"] = m_showName;
    j["m_maxLength"] = m_maxLength;
}
