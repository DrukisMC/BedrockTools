#include "bubblechat.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include "core/memory/Hooks.hpp"
#include <pl/memory/Vtable.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>

static BubbleChatModule* g_bubbleChatMod = nullptr;

static void (*originalHandleText)(void*, void*, void*) = nullptr;

// NameTagRenderer::render — vtable slot 17.
// Signature: void render(void* self, void* uiRenderContext, void* uiControl, void* uiAnchor)
static void (*s_nameTagRenderOrig)(void*, void*, void*, void*) = nullptr;

static void nameTagRenderHook(void* self, void* uiCtx, void* uiControl, void* uiAnchor) {
    if (s_nameTagRenderOrig) s_nameTagRenderOrig(self, uiCtx, uiControl, uiAnchor);
    if (!g_bubbleChatMod || !g_bubbleChatMod->enabled || !g_bubbleChatMod->m_separateBubble) return;
    g_bubbleChatMod->renderSeparateBubble(self, uiCtx, uiControl, uiAnchor);
}

struct DistanceSortedActor { void* mActor; float mDistance; float _pad; };
struct ActorVec { DistanceSortedActor* begin; DistanceSortedActor* end; DistanceSortedActor* cap; };

using ActorFetchFn = ActorVec (*)(void*, void*, int);
using ActorIsPlayerFn = bool (*)(void*);
using ActorSetNameTagFn = void (*)(void*, std::string*);

static ActorFetchFn s_actorFetchNearby = nullptr;
static ActorIsPlayerFn s_actorIsPlayer = nullptr;
static ActorSetNameTagFn s_actorSetNameTag = nullptr;

static std::string trimStr(const std::string& t) {
    size_t b = t.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = t.find_last_not_of(" \t");
    return t.substr(b, e - b + 1);
}

static std::string stripColors(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.length(); ++i) {
        if ((unsigned char)in[i] == 0xC2 && i + 2 < in.length() && (unsigned char)in[i+1] == 0xA7) { i += 2; continue; }
        out += in[i];
    }
    return out;
}

static std::string normalizeName(const std::string& in) {
    std::string s = stripColors(in);
    std::string out;
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (u >= 'A' && u <= 'Z') u += 32;
        if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) out += (char)u;
    }
    return out;
}

static std::string removeBrackets(const std::string& s) {
    std::string out;
    int depth = 0;
    for (char c : s) {
        if (c == '[') { depth++; continue; }
        if (c == ']') { if (depth > 0) depth--; continue; }
        if (depth == 0) out += c;
    }
    return out;
}

static std::string wrapText(const std::string& s, size_t maxLen) {
    std::string out;
    size_t lineLen = 0;
    size_t i = 0, n = s.length();
    while (i < n) {
        while (i < n && s[i] == ' ') ++i;
        size_t j = i;
        while (j < n && s[j] != ' ') ++j;
        if (j == i) break;
        std::string word = s.substr(i, j - i);
        i = j;
        if (lineLen == 0) {
            while (word.length() > maxLen) { out += word.substr(0, maxLen); out += '\n'; word = word.substr(maxLen); }
            out += word; lineLen = word.length();
        } else if (lineLen + 1 + word.length() <= maxLen) {
            out += ' '; out += word; lineLen += 1 + word.length();
        } else {
            out += '\n';
            while (word.length() > maxLen) { out += word.substr(0, maxLen); out += '\n'; word = word.substr(maxLen); }
            out += word; lineLen = word.length();
        }
    }
    return out;
}

static bool parseRawChat(const std::string& raw, std::string& author, std::string& msg) {
    std::string s = stripColors(raw);

    const std::string sep = "\xC2\xBB"; // »
    size_t pos = s.find(sep);
    if (pos != std::string::npos) {
        author = trimStr(removeBrackets(s.substr(0, pos)));
        msg = trimStr(s.substr(pos + sep.length()));
        return !author.empty() && !msg.empty();
    }

    size_t start = s.find('<');
    size_t end = s.find('>');
    if (start != std::string::npos && end != std::string::npos && end > start) {
        author = trimStr(s.substr(start + 1, end - start - 1));
        msg = trimStr(s.substr(end + 1));
        return !author.empty() && !msg.empty();
    }

    size_t colon = s.find(": ");
    if (colon != std::string::npos && colon <= 32) {
        author = trimStr(removeBrackets(s.substr(0, colon)));
        msg = trimStr(s.substr(colon + 2));
        return !author.empty() && !msg.empty();
    }

    return false;
}

static void handleTextHook(void* handler, void* source, void* packet) {
    if (originalHandleText) originalHandleText(handler, source, packet);

    if (!g_bubbleChatMod || !g_bubbleChatMod->enabled || !packet) return;

    try {
        uintptr_t payload = (uintptr_t)packet + bedrocktools::sdk::offsets::Packet::Size;
        uint8_t type = *reinterpret_cast<uint8_t*>(payload + bedrocktools::sdk::offsets::TextPacketPayload::AuthorAndMessage::mType);

        std::string author, message;

        if (type == 1) {
            author = stripColors(*reinterpret_cast<std::string*>(payload + bedrocktools::sdk::offsets::TextPacketPayload::AuthorAndMessage::mAuthor));
            message = *reinterpret_cast<std::string*>(payload + bedrocktools::sdk::offsets::TextPacketPayload::AuthorAndMessage::mMessage);
        } else if (type == 2) {
            std::string key = *reinterpret_cast<std::string*>(payload + 0x78);
            std::string* pbegin = *reinterpret_cast<std::string**>(payload + 0x90);
            std::string* pend   = *reinterpret_cast<std::string**>(payload + 0x98);
            if (pbegin && pend && pend >= pbegin && (pend - pbegin) <= 16 && (uintptr_t)pbegin > 0x1000) {
                size_t cnt = (size_t)(pend - pbegin);
                if (cnt >= 2 && key.find("chat.type") != std::string::npos) {
                    author = stripColors(pbegin[0]);
                    message = stripColors(pbegin[1]);
                } else if (cnt >= 1) {
                    message = stripColors(pbegin[0]);
                }
            }
        } else if (type <= 9) {
            message = *reinterpret_cast<std::string*>(payload + bedrocktools::sdk::offsets::TextPacketPayload::MessageOnly::mMessage);
        } else {
            return;
        }

        if (author.empty()) {
            std::string pa, pm;
            if (parseRawChat(message, pa, pm)) {
                author = pa;
                message = pm;
            }
        }

        if (!author.empty() && !message.empty()) {
            if (message.rfind(".bc", 0) == 0) {
                std::string arg = trimStr(message.substr(3));
                if (!arg.empty() && arg[0] >= '0' && arg[0] <= '9') {
                    int v = atoi(arg.c_str());
                    if (v >= 1 && v <= 15) { g_bubbleChatMod->setDuration(v); g_bubbleChatMod->addBubble(author, "Tempo da bolha: " + std::to_string(v) + "s"); }
                } else if (arg == "cima" || arg == "above") {
                    g_bubbleChatMod->setMsgAbove(true); g_bubbleChatMod->addBubble(author, "Msg acima do nome");
                } else if (arg == "baixo" || arg == "below") {
                    g_bubbleChatMod->setMsgAbove(false); g_bubbleChatMod->addBubble(author, "Msg abaixo do nome");
                }
                return;
            }
            g_bubbleChatMod->addBubble(author, message);
        }
    } catch (...) {}
}

static void tickCallback(void* player) {
    if (!g_bubbleChatMod || !player) return;

    if (g_bubbleChatMod->m_localPlayerPtr != player) {
        g_bubbleChatMod->resetState();
    }

    g_bubbleChatMod->m_localPlayerPtr = player;
    g_bubbleChatMod->applyBubbles(g_bubbleChatMod->enabled);
}

BubbleChatModule::BubbleChatModule() : Module("Bubble Chat", "Bolhas de chat acima da cabeca (estilo Roblox)") {
    g_bubbleChatMod = this;
    m_lastFrame = std::chrono::steady_clock::now();
}

BubbleChatModule::~BubbleChatModule() {
    removeNametagPatch();
    if (g_bubbleChatMod == this) g_bubbleChatMod = nullptr;
}

void BubbleChatModule::setDuration(int secs) {
    if (secs < 1) secs = 1;
    if (secs > 15) secs = 15;
    m_duration = secs;
}

void BubbleChatModule::setMsgAbove(bool above) {
    m_msgAboveName = above;
}

void BubbleChatModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("duration")) { int d = j.value("duration", 5); if (d < 1) d = 1; if (d > 15) d = 15; m_duration = d; }
    if (j.contains("msgAbove")) m_msgAboveName = j.value("msgAbove", false);
    if (j.contains("separateBubble")) m_separateBubble = j.value("separateBubble", false);
}

void BubbleChatModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["duration"] = m_duration;
    j["msgAbove"] = m_msgAboveName;
    j["separateBubble"] = m_separateBubble;
}

void BubbleChatModule::addBubble(const std::string& author, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& dq = m_bubbles[author];
    dq.push_back({message, (float)m_duration});
    while (dq.size() > 3) dq.pop_front();
}

void BubbleChatModule::onEnable() { ensureNametagPatch(); }

void BubbleChatModule::onDisable() {
    removeNametagPatch();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bubbles.clear();
}

void BubbleChatModule::resetState() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bubbles.clear();
    m_overrides.clear();
}

void BubbleChatModule::ensureNametagPatch() {
    if (!m_nametagPatchTarget) return;
    uint32_t cur = 0;
    memcpy(&cur, m_nametagPatchTarget, 4);
    const uint32_t nop = 0xD503201F;
    if (cur == nop) return;
    if (!m_hasOrigBytes) { memcpy(m_nametagOrigBytes, &cur, 4); m_hasOrigBytes = true; }
    bedrocktools::sdk::patchMemory(m_nametagPatchTarget, &nop, 4);
    if (!m_nametagPatchedByUs) { m_nametagPatchedByUs = true; }
}

void BubbleChatModule::removeNametagPatch() {
    if (!m_nametagPatchedByUs || !m_nametagPatchTarget) return;
    bedrocktools::sdk::patchMemory(m_nametagPatchTarget, m_nametagOrigBytes, 4);
    m_nametagPatchedByUs = false;
    m_hasOrigBytes = false;
}

void BubbleChatModule::onFrame() {
    ensureNametagPatch();
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - m_lastFrame).count();
    m_lastFrame = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_bubbles.begin(); it != m_bubbles.end(); ) {
        auto& dq = it->second;
        for (auto bi = dq.begin(); bi != dq.end(); ) {
            bi->timer -= dt;
            if (bi->timer <= 0.0f) bi = dq.erase(bi);
            else ++bi;
        }
        if (dq.empty()) it = m_bubbles.erase(it);
        else ++it;
    }
}

void BubbleChatModule::onInit() {
    uintptr_t textAddr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ClientNetworkHandlerHandleText);
    if (textAddr) bedrocktools::hooks::install((void*)textAddr, (void*)handleTextHook, (void**)&originalHandleText);

    uintptr_t ntAddr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::Nametag);
    if (ntAddr) m_nametagPatchTarget = (void*)(ntAddr + bedrocktools::sdk::offsets::NameTag::mExtractNameTagsPatchOffset);

    if (auto afn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted)) s_actorFetchNearby = (ActorFetchFn)afn;
    if (auto aip = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer)) s_actorIsPlayer = (ActorIsPlayerFn)aip;
    if (auto snt = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSetNameTag)) s_actorSetNameTag = (ActorSetNameTagFn)snt;

    // Hook NameTagRenderer::render (vtable slot 17) for the separate-bubble mode.
    if (!s_nameTagRenderOrig) {
        const auto target = pl::memory::resolveVtableFunction("15NameTagRenderer", 17, "libminecraftpe.so");
        if (target) {
            bedrocktools::hooks::install(reinterpret_cast<void*>(target),
                                         reinterpret_cast<void*>(nameTagRenderHook),
                                         reinterpret_cast<void**>(&s_nameTagRenderOrig));
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](const bedrocktools::events::LocalPlayerTickEvent& event) {
        tickCallback(event.player);
    });
}

void BubbleChatModule::applyBubbles(bool apply) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!apply) m_bubbles.clear();

    // In separate-bubble mode the message is drawn by the NameTagRenderer hook,
    // not by overriding the nametag text.
    if (m_separateBubble) return;

    if (m_bubbles.empty() && m_overrides.empty()) return;
    if (!m_localPlayerPtr || !s_actorFetchNearby || !s_actorSetNameTag) return;

    uintptr_t levelPtr = *(uintptr_t*)((uintptr_t)m_localPlayerPtr + bedrocktools::sdk::offsets::Actor::mLevel);
    if (!levelPtr) return;
    uintptr_t dimPtr = *(uintptr_t*)((uintptr_t)m_localPlayerPtr + bedrocktools::sdk::offsets::Actor::mDimension);
    if (!dimPtr) return;

    std::vector<void*> actors;
    actors.push_back(m_localPlayerPtr);

    bedrocktools::sdk::Vec3 extent = {30.0f, 30.0f, 30.0f};
    ActorVec av = s_actorFetchNearby(m_localPlayerPtr, &extent, 1);
    if (av.begin && av.end) {
        for (DistanceSortedActor* it = av.begin; it < av.end; ++it) {
            if (it->mActor && it->mActor != m_localPlayerPtr) actors.push_back(it->mActor);
        }
    }

    std::vector<std::pair<std::string, std::string>> normKeys;
    for (auto& kv : m_bubbles) normKeys.push_back({normalizeName(kv.first), kv.first});

    for (void* actor : actors) {
        if (s_actorIsPlayer && !s_actorIsPlayer(actor)) continue;

        uintptr_t addr = (uintptr_t)actor;
        std::string* pName = (std::string*)(addr + bedrocktools::sdk::offsets::Player::mName);
        std::string* pTag  = (std::string*)(addr + bedrocktools::sdk::offsets::Actor::mFilteredNameTag);

        std::string n1 = (pName && !pName->empty()) ? *pName : "";
        std::string n2 = (pTag  && !pTag->empty())  ? *pTag  : "";

        std::string nn1 = normalizeName(n1);
        std::string nn2 = normalizeName(n2);

        auto ovIt = m_overrides.find(actor);
        std::string original = (ovIt != m_overrides.end()) ? ovIt->second.original : "";

        auto bIt = m_bubbles.end();
        if (!original.empty()) bIt = m_bubbles.find(original);
        if (bIt == m_bubbles.end()) { bIt = m_bubbles.find(n1); if (bIt != m_bubbles.end()) original = n1; }
        if (bIt == m_bubbles.end()) { bIt = m_bubbles.find(n2); if (bIt != m_bubbles.end()) original = n2; }
        if (bIt == m_bubbles.end()) {
            for (auto& nk : normKeys) {
                if (nk.first.length() < 3) continue;
                if (nk.first == nn1 || nk.first == nn2 ||
                    nn2.find(nk.first) != std::string::npos ||
                    nn1.find(nk.first) != std::string::npos) {
                    original = nk.second;
                    bIt = m_bubbles.find(original);
                    break;
                }
            }
        }

        if (bIt != m_bubbles.end()) {
            std::string joined;
            for (const auto& b : bIt->second) {
                if (!joined.empty()) joined += "\n";
                joined += wrapText("\xE2\x80\x94 " + b.message, 28);
            }
            std::string appliedStr;
            if (m_msgAboveName) appliedStr = joined + "\n" + original;
            else appliedStr = original + "\n" + joined;

            if (n2 != appliedStr) {
                s_actorSetNameTag(actor, &appliedStr);
            }
            m_overrides[actor] = {original, appliedStr};
        } else if (ovIt != m_overrides.end()) {
            if (n2 != ovIt->second.original) {
                s_actorSetNameTag(actor, &ovIt->second.original);
            }
            m_overrides.erase(ovIt);
        }
    }
}

// Separate-bubble renderer. Called from the NameTagRenderer::render hook
// (vtable slot 17) AFTER the original nametag has been drawn.
//
// The render object layout (this build):
//   [this+0x08] float scale
//   [this+0x28] std::string  -> text that gets drawn (player name)
//   [this+0x40..0x5c] mce::Color
// The anchor object:
//   [anchor+0x10] Vec2 position (screen anchor)
//   [anchor+0x40] Vec2 size (nametag width/height)
void BubbleChatModule::renderSeparateBubble(void* self, void* uiCtx, void* uiControl, void* uiAnchor) {
    if (!self || !uiAnchor || !s_nameTagRenderOrig) return;

    try {
        std::string* pName = reinterpret_cast<std::string*>(reinterpret_cast<std::uintptr_t>(self) + 0x28);
        if (!pName || pName->empty()) return;

        const std::string playerName = stripColors(*pName);
        if (playerName.empty()) return;

        // Find which author key matches this player's nametag.
        std::string foundKey;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_bubbles.find(playerName);
            if (it != m_bubbles.end()) {
                foundKey = playerName;
            } else {
                const std::string norm = normalizeName(playerName);
                for (const auto& kv : m_bubbles) {
                    if (normalizeName(kv.first) == norm) { foundKey = kv.first; break; }
                }
            }
        }
        if (foundKey.empty()) return;

        // Build the stacked bubble text.
        std::string joined;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_bubbles.find(foundKey);
            if (it == m_bubbles.end() || it->second.empty()) return;
            for (const auto& b : it->second) {
                if (!joined.empty()) joined += "\n";
                joined += wrapText(b.message, 28);
            }
        }
        if (joined.empty()) return;

        float* pos = reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(uiAnchor) + 0x10);
        float* size = reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(uiAnchor) + 0x40);
        const float height = (size[1] > 1.0f) ? size[1] : 20.0f;
        const float gap = 4.0f;

        const std::string original = *pName;
        const float origY = pos[1];

        // Swap the text and lift the anchor, then re-render to draw the bubble
        // above the nametag as its own panel + text.
        *pName = joined;
        pos[1] = origY - (height + gap);

        s_nameTagRenderOrig(self, uiCtx, uiControl, uiAnchor);

        pos[1] = origY;
        *pName = original;
    } catch (...) {
        // Never let a rendering edge case crash the game.
    }
}
