#include "bubblechat.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/Level.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include "core/memory/Hooks.hpp"
#include <EGL/egl.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <list>
#include <algorithm>

static BubbleChatModule* g_bubbleChatMod = nullptr;

static void (*originalHandleText)(void*, void*, void*) = nullptr;

struct DistanceSortedActor { void* mActor; float mDistance; float _pad; };
struct ActorVec { DistanceSortedActor* begin; DistanceSortedActor* end; DistanceSortedActor* cap; };

using ActorFetchFn = ActorVec (*)(void*, void*, int);
using ActorIsPlayerFn = bool (*)(void*);

static ActorFetchFn s_actorFetchNearby = nullptr;
static ActorIsPlayerFn s_actorIsPlayer = nullptr;

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

BubbleChatModule::BubbleChatModule() : Module("Bubble Chat", "Bolhas de chat acima da cabeca (estilo Roblox)") {
    g_bubbleChatMod = this;
    m_lastFrame = std::chrono::steady_clock::now();
}

BubbleChatModule::~BubbleChatModule() {
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
    if (j.contains("fov")) m_fov = std::clamp(j["fov"].get<float>(), 30.0f, 120.0f);
    if (j.contains("scale")) m_scale = std::clamp(j["scale"].get<float>(), 0.5f, 3.0f);
    if (j.contains("heightOffset")) m_heightOffset = std::clamp(j["heightOffset"].get<float>(), 0.5f, 5.0f);
    if (j.contains("showOwnBubbles")) m_showOwnBubbles = j["showOwnBubbles"].get<bool>();
}

void BubbleChatModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["duration"] = m_duration;
    j["fov"] = m_fov;
    j["scale"] = m_scale;
    j["heightOffset"] = m_heightOffset;
    j["showOwnBubbles"] = m_showOwnBubbles;
}

void BubbleChatModule::addBubble(const std::string& author, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& dq = m_bubbles[author];
    dq.push_back({message, (float)m_duration});
    while (dq.size() > 3) dq.pop_front();
}

void BubbleChatModule::onEnable() {}

void BubbleChatModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bubbles.clear();
}

static float calcTextWidth(const std::string& text, float size) {
    float width = 0;
    for (char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ' || c == '!') width += size * 0.32f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') width += size * 0.82f;
        else width += size * 0.58f;
    }
    return width;
}

// Project a world-space point to screen pixel coordinates.
// Returns false if the point is behind the camera.
static bool worldToScreen(const bedrocktools::sdk::Vec3& camPos, float yaw, float pitch,
                          const bedrocktools::sdk::Vec3& world, float fovV,
                          int screenW, int screenH, float& outX, float& outY) {
    const float PI = 3.14159265f;
    const float yawR = yaw * (PI / 180.0f);
    const float pitchR = pitch * (PI / 180.0f);

    const float cy = std::cos(yawR), sy = std::sin(yawR);
    const float cp = std::cos(pitchR), sp = std::sin(pitchR);

    // Camera basis (Minecraft convention: yaw clockwise from +Z, pitch down positive).
    bedrocktools::sdk::Vec3 fwd{-sy * cp, -sp, cy * cp};
    bedrocktools::sdk::Vec3 right{cy, 0.0f, sy};
    bedrocktools::sdk::Vec3 up{-sy * sp, cp, cy * sp};

    const float dx = world.x - camPos.x;
    const float dy = world.y - camPos.y;
    const float dz = world.z - camPos.z;

    const float zCam = dx * fwd.x + dy * fwd.y + dz * fwd.z;
    if (zCam < 0.1f) return false; // behind the camera

    const float xCam = dx * right.x + dy * right.y + dz * right.z;
    const float yCam = dx * up.x + dy * up.y + dz * up.z;

    const float f = (screenH * 0.5f) / std::tan((fovV * 0.5f) * (PI / 180.0f));
    outX = (screenW * 0.5f) + (xCam * f / zCam);
    outY = (screenH * 0.5f) - (yCam * f / zCam);
    return true;
}

void BubbleChatModule::onFrame() {
    if (!enabled) return;

    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - m_lastFrame).count();
    m_lastFrame = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;

    // Update timers.
    {
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

    auto* client = bedrocktools::sdk::ClientInstance::current();
    if (!client) return;

    auto* localPlayer = client->localPlayer();
    if (!localPlayer) return;
    m_localPlayerPtr = localPlayer;

    if (!s_actorFetchNearby || !s_actorIsPlayer) return;

    // Camera position (smooth camera) + rotation.
    bedrocktools::sdk::Vec3 camPos = localPlayer->position();
    camPos.y += 1.62f;
    auto* lr = client->levelRenderer();
    if (lr && lr->playerRenderer()) {
        const auto cp = lr->playerRenderer()->cameraPosition();
        camPos = cp;
    }
    const auto rot = localPlayer->rotation(); // {pitch, yaw}
    const float yaw = rot.y;
    const float pitch = rot.x;

    // Screen size.
    int screenW = 0, screenH = 0;
    EGLDisplay display = eglGetCurrentDisplay();
    EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE) {
        eglQuerySurface(display, surface, EGL_WIDTH, &screenW);
        eglQuerySurface(display, surface, EGL_HEIGHT, &screenH);
    }
    if (screenW <= 0 || screenH <= 0) return;

    std::vector<PLModMenu_DrawCommand> cmds;
    std::list<std::string> stringStore; // keep strings alive (std::list = stable addresses)

    // Gather nearby actors (players).
    std::vector<void*> actors;
    bedrocktools::sdk::Vec3 extent = {30.0f, 30.0f, 30.0f};
    ActorVec av = s_actorFetchNearby(m_localPlayerPtr, &extent, 1);
    if (av.begin && av.end) {
        for (DistanceSortedActor* it = av.begin; it < av.end; ++it) {
            if (it->mActor) actors.push_back(it->mActor);
        }
    }
    // Also consider the local player (for own bubbles in 3rd person).
    if (m_showOwnBubbles) actors.push_back(m_localPlayerPtr);

    for (void* actor : actors) {
        if (s_actorIsPlayer && !s_actorIsPlayer(actor)) continue;

        auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(actor);
        const std::string name = stripColors(player->name());
        if (name.empty()) continue;

        // Find this player's pending bubbles.
        std::string foundKey;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_bubbles.find(name);
            if (it != m_bubbles.end()) foundKey = name;
            else {
                const std::string norm = normalizeName(name);
                for (const auto& kv : m_bubbles) {
                    if (normalizeName(kv.first) == norm) { foundKey = kv.first; break; }
                }
            }
        }
        if (foundKey.empty()) continue;

        // Anchor above the head.
        bedrocktools::sdk::Vec3 anchor = player->position();
        anchor.y += m_heightOffset;

        float sx = 0.0f, sy = 0.0f;
        if (!worldToScreen(camPos, yaw, pitch, anchor, m_fov, screenW, screenH, sx, sy)) continue;
        if (sx < -200 || sx > screenW + 200 || sy < -200 || sy > screenH + 200) continue;

        // Build the stacked text and draw one bubble per message.
        std::vector<std::string> texts;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_bubbles.find(foundKey);
            if (it == m_bubbles.end()) continue;
            for (const auto& b : it->second) texts.push_back(stripColors(b.message));
        }
        if (texts.empty()) continue;

        const float fontSize = 16.0f * m_scale;
        const float padX = 8.0f * m_scale;
        const float padY = 5.0f * m_scale;
        const float lineGap = 2.0f * m_scale;

        // Draw bottom-up so older messages stack upward.
        float cursorY = sy;
        for (auto it = texts.rbegin(); it != texts.rend(); ++it) {
            const std::string& text = *it;
            const float textW = calcTextWidth(text, fontSize);
            const float boxW = textW + padX * 2.0f;
            const float boxH = fontSize + padY * 2.0f;

            // Background box (white).
            PLModMenu_DrawCommand bg = {};
            bg.type = PL_DRAW_RECT_FILLED;
            bg.x = sx - boxW * 0.5f;
            bg.y = cursorY - boxH;
            bg.w = boxW;
            bg.h = boxH;
            bg.color = 0xF0FFFFFF; // near-opaque white
            cmds.push_back(bg);

            // Text (black).
            stringStore.push_back(text);
            PLModMenu_DrawCommand txt = {};
            txt.type = PL_DRAW_TEXT;
            txt.x = sx - textW * 0.5f;
            txt.y = cursorY - boxH + padY;
            txt.w = 0.0f;
            txt.h = 0.0f;
            txt.color = 0xFF000000; // black
            txt.size = fontSize;
            txt.text = stringStore.back().c_str();
            cmds.push_back(txt);

            cursorY -= (boxH + lineGap);
        }
    }

    if (!cmds.empty()) submitDrawCommands(moduleId, cmds);
}

void BubbleChatModule::onInit() {
    uintptr_t textAddr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ClientNetworkHandlerHandleText);
    if (textAddr) bedrocktools::hooks::install((void*)textAddr, (void*)handleTextHook, (void**)&originalHandleText);

    if (auto afn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted)) s_actorFetchNearby = (ActorFetchFn)afn;
    if (auto aip = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer)) s_actorIsPlayer = (ActorIsPlayerFn)aip;
}
