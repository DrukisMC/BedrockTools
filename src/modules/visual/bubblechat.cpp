#include "bubblechat.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include "core/memory/Hooks.hpp"
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>

#define BT_LOG(...) __android_log_print(ANDROID_LOG_INFO, "BubbleChat", __VA_ARGS__)

static BubbleChatModule* g_bubbleChatMod = nullptr;

static void (*originalHandleText)(void*, void*, void*) = nullptr;

// BaseActorRenderer::renderText — the REAL world nametag render (tessellator
// panel + text). Signature (best effort): (x0=ctx, x1=screenCtx, x2=data,
// x3=text, x4=color). Found via signature, verified unique.
static void (*s_renderTextOrig)(void*, void*, void*, void*, void*) = nullptr;
static int s_renderTextLogBudget = 40;  // rate-limit diagnostic logs

static void writeDiag(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "BubbleChat", "%s", buf);

    // Also dump to a file the user can open directly (no logcat needed).
    FILE* fp = fopen("/storage/emulated/0/Android/media/org.levimc.launcher/kurumi/bubblechat_diag.log", "a");
    if (fp) {
        fprintf(fp, "%s\n", buf);
        fclose(fp);
    }
}

static void renderTextHook(void* x0, void* x1, void* x2, void* x3, void* x4) {
    if (s_renderTextLogBudget > 0) {
        --s_renderTextLogBudget;
        writeDiag("renderText fired: x0=%p x1=%p x2=%p x3=%p x4=%p",
                  x0, x1, x2, x3, x4);

        // Try to read a string from x2 and x3 (could be std::string or raw ptr).
        auto tryStr = [](void* p) -> const char* {
            if (!p || (uintptr_t)p < 0x1000) return nullptr;
            // Try: p is a pointer to a C-string.
            return reinterpret_cast<const char*>(p);
        };
        // x3 is most likely the text (from draw-text: x2=strPtr came from arg3 path)
        for (int slot = 2; slot <= 3; ++slot) {
            void* arg = (slot == 2) ? x2 : x3;
            if (!arg || (uintptr_t)arg < 0x1000) continue;
            // Read as std::string-like: pointer at +0 (heap) or inline at +1
            char tmp[96];
            memset(tmp, 0, sizeof(tmp));
            // heap layout: [ptr][size][cap]
            const char* dataPtr = *reinterpret_cast<const char* const*>(arg);
            if (dataPtr && (uintptr_t)dataPtr > 0x1000) {
                strncpy(tmp, dataPtr, sizeof(tmp) - 1);
                writeDiag("  arg x%d -> heap string: '%s'", slot, tmp);
            }
            // inline layout (SSO): bytes right after the size field
            bool printable = true;
            size_t n = 0;
            const unsigned char* b = reinterpret_cast<const unsigned char*>(arg);
            for (size_t i = 0; i < 48; ++i) {
                if (b[i] == 0) { n = i; break; }
                if (b[i] < 0x20 || b[i] > 0x7e) { printable = false; break; }
            }
            if (printable && n > 0) {
                strncpy(tmp, reinterpret_cast<const char*>(arg), n);
                writeDiag("  arg x%d -> inline string: '%s'", slot, tmp);
            }
        }
        // x4 = color (4 floats).
        if (x4 && (uintptr_t)x4 > 0x1000) {
            const float* c = reinterpret_cast<const float*>(x4);
            writeDiag("  color @x4: %.2f %.2f %.2f %.2f", c[0], c[1], c[2], c[3]);
        }
    }

    if (s_renderTextOrig) s_renderTextOrig(x0, x1, x2, x3, x4);
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

void BubbleChatModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("duration")) { int d = j.value("duration", 5); if (d < 1) d = 1; if (d > 15) d = 15; m_duration = d; }
    if (j.contains("separateBubble")) m_separateBubble = j.value("separateBubble", false);
}

void BubbleChatModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["duration"] = m_duration;
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
    m_overrides.clear();
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

    // Diagnostic hook on the REAL world nametag render (BaseActorRenderer::renderText).
    if (!s_renderTextOrig) {
        const auto sig = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BaseActorRenderText);
        if (sig) {
            bedrocktools::hooks::install(reinterpret_cast<void*>(sig),
                                         reinterpret_cast<void*>(renderTextHook),
                                         reinterpret_cast<void**>(&s_renderTextOrig));
            writeDiag("renderText hook INSTALLED at 0x%llx (orig=%p)",
                      (unsigned long long)sig, (void*)s_renderTextOrig);
        } else {
            writeDiag("renderText hook FAILED: signature not resolved");
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](const bedrocktools::events::LocalPlayerTickEvent& event) {
        tickCallback(event.player);
    });
}

void BubbleChatModule::applyBubbles(bool apply) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!apply) m_bubbles.clear();

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
            // Default mode: the nametag becomes the message (stacked, newlines).
            std::string joined;
            for (const auto& b : bIt->second) {
                if (!joined.empty()) joined += "\n";
                joined += wrapText(stripColors(b.message), 28);
            }
            std::string appliedStr = joined;

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
