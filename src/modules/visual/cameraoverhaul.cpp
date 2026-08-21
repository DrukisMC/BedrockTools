/*
 * CameraOverhaul port for BedrockTools.
 *
 * The camera math in this file is derived from Minecraft-CameraOverhaul
 * by Mirsario & Contributors, licensed under GPL-3.0.
 * https://github.com/Mirsario/Minecraft-CameraOverhaul
 *
 * BedrockTools is likewise GPL-3.0, so the derivation is license compatible.
 */

#include "cameraoverhaul.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>

namespace {

CameraOverhaulModule* g_cameraMod = nullptr;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;

/*
 * Gains chosen so the effect is visible but not nauseating, given that
 * velocity here is in blocks/second:
 *   forward  0.45 -> ~2.5 deg sprinting, ~13 deg in an elytra dive
 *   vertical 0.30 -> ~6 deg at terminal fall velocity
 */
constexpr float kForwardPitchGain  = 0.45f;
constexpr float kVerticalPitchGain = 0.30f;

struct Quat {
    float x, y, z, w;
};

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float dampStep(float smoothing, float dt) {
    if (smoothing <= 0.001f) return 1.0f;
    return 1.0f - std::exp(-(16.0f / smoothing) * dt);
}

float easeInOutCubic(float t) {
    if (t < 0.5f) return 4.0f * t * t * t;
    float f = -2.0f * t + 2.0f;
    return 1.0f - (f * f * f) * 0.5f;
}

float signf(float v) { return v < 0.0f ? -1.0f : 1.0f; }

/*
 * Deterministic value noise used for camera sway and shakes.
 * SimplexNoise in the original is only sampled along one axis per channel,
 * so a smoothed 1D gradient noise is perceptually equivalent here.
 */
float hashNoise(int i, int seed) {
    std::uint32_t h = static_cast<std::uint32_t>(i) * 374761393u
                    + static_cast<std::uint32_t>(seed) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return (static_cast<float>(h & 0xFFFFFFu) / 8388607.5f) - 1.0f;
}

float noise1D(float x, int seed) {
    float fx = std::floor(x);
    int i = static_cast<int>(fx);
    float f = x - fx;
    float u = f * f * (3.0f - 2.0f * f);
    float a = hashNoise(i, seed);
    float b = hashNoise(i + 1, seed);
    return a + (b - a) * u;
}

/*
 * The camera bias is applied by post-multiplying the engine quaternion with a
 * small local rotation. This never converts to Euler angles, so it cannot hit
 * gimbal lock at the poles and does not depend on the engine's Euler
 * convention, which is not observable from the disassembly.
 */
Quat quatMul(const Quat& a, const Quat& b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

Quat axisAngle(float ax, float ay, float az, float angle) {
    float h = angle * 0.5f;
    float s = std::sin(h);
    return Quat{ax * s, ay * s, az * s, std::cos(h)};
}

Quat quatNormalize(const Quat& q) {
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-8f)) return Quat{0.0f, 0.0f, 0.0f, 1.0f};
    return Quat{q.x / n, q.y / n, q.z / n, q.w / n};
}

struct CameraState {
    float prevYaw = 0.0f;
    bool  hasPrevYaw = false;

    float rollTarget = 0.0f;
    float roll = 0.0f;

    float pitchVertical = 0.0f;
    float pitchForward = 0.0f;

    float idleTime = 0.0f;
    float swayFade = 0.0f;

    bedrocktools::sdk::Vec3 lastPos{};
    bedrocktools::sdk::Vec3 velocity{};
    bool  hasLastPos = false;

    double lastTickTime = 0.0;

    void reset() {
        *this = CameraState{};
    }
};

CameraState g_state;

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

using CameraBlendTickFn = void (*)(void* component, void* blendState, float factor);
CameraBlendTickFn _cameraBlendTick_orig = nullptr;

/*
 * CameraBlendSystem::_tick writes the camera transform into the
 * CameraComponent pointed to by the first argument:
 *   +0x28  quaternion (x, y, z, w)
 *   +0x38  Vec3 position
 * We let the original run first, then bias the resulting rotation.
 */
constexpr std::uintptr_t kRotationOffset = 0x28;

void _cameraBlendTick_hook(void* component, void* blendState, float factor) {
    if (_cameraBlendTick_orig) {
        _cameraBlendTick_orig(component, blendState, factor);
    }

    if (!g_cameraMod || !g_cameraMod->enabled) return;
    if (!component) return;

    auto* quat = reinterpret_cast<Quat*>(reinterpret_cast<std::uintptr_t>(component) + kRotationOffset);

    float len = std::sqrt(quat->x * quat->x + quat->y * quat->y +
                          quat->z * quat->z + quat->w * quat->w);
    if (!(len > 0.5f && len < 1.5f)) return;

    float pitchBias = 0.0f;
    float yawBias   = 0.0f;
    float rollBias  = 0.0f;

    if (g_cameraMod->m_enablePitch) {
        pitchBias += (g_state.pitchVertical + g_state.pitchForward) * kDeg2Rad;
    }
    if (g_cameraMod->m_enableRoll) {
        rollBias += g_state.roll * kDeg2Rad;
    }

    if (g_cameraMod->m_enableSway && g_cameraMod->m_swayIntensity > 0.0f && g_state.swayFade > 0.0f) {
        float t = static_cast<float>(nowSeconds());
        float freq = g_cameraMod->m_swayFrequency;
        float amp = g_cameraMod->m_swayIntensity * g_state.swayFade;
        float f = amp * amp * amp;
        pitchBias += noise1D(t * freq, 420) * f * 0.35f * kDeg2Rad;
        yawBias   += noise1D(t * freq, 1337) * f * 0.35f * kDeg2Rad;
        rollBias  += noise1D(t * freq, 6969) * f * 0.50f * kDeg2Rad;
    }

    if (std::fabs(pitchBias) < 1e-6f &&
        std::fabs(yawBias)   < 1e-6f &&
        std::fabs(rollBias)  < 1e-6f) {
        return;
    }

    if (!std::isfinite(pitchBias) || !std::isfinite(yawBias) || !std::isfinite(rollBias)) {
        return;
    }

    pitchBias = clampf(pitchBias, -0.7f, 0.7f);
    yawBias   = clampf(yawBias,   -0.7f, 0.7f);
    rollBias  = clampf(rollBias,  -0.7f, 0.7f);

    Quat delta = axisAngle(1.0f, 0.0f, 0.0f, pitchBias);
    if (yawBias != 0.0f)  delta = quatMul(delta, axisAngle(0.0f, 1.0f, 0.0f, yawBias));
    if (rollBias != 0.0f) delta = quatMul(delta, axisAngle(0.0f, 0.0f, 1.0f, rollBias));

    *quat = quatNormalize(quatMul(*quat, delta));
}

}

CameraOverhaulModule::CameraOverhaulModule()
    : Module("CameraOverhaul", "Cinematic camera tilt, roll and sway based on movement") {
    g_cameraMod = this;
}

CameraOverhaulModule::~CameraOverhaulModule() {
    g_cameraMod = nullptr;
}

void CameraOverhaulModule::resetState() {
    g_state.reset();
}

void CameraOverhaulModule::onInit() {
    if (!m_hooked) {
        std::uintptr_t addr = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::CameraBlendSystemTick);
        if (addr) {
            if (bedrocktools::hooks::install(reinterpret_cast<void*>(addr),
                                             reinterpret_cast<void*>(_cameraBlendTick_hook),
                                             reinterpret_cast<void**>(&_cameraBlendTick_orig))) {
                m_hooked = true;
            }
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](bedrocktools::events::LocalPlayerTickEvent& e) {
            if (!g_cameraMod || !g_cameraMod->enabled) return;
            auto* player = e.player;
            if (!player) return;

            double now = nowSeconds();
            float dt = 0.05f;
            if (g_state.lastTickTime > 0.0) {
                dt = static_cast<float>(now - g_state.lastTickTime);
            }
            g_state.lastTickTime = now;
            dt = clampf(dt, 0.001f, 0.25f);

            bedrocktools::sdk::Vec3 pos = player->position();
            if (!g_state.hasLastPos) {
                g_state.lastPos = pos;
                g_state.hasLastPos = true;
            }
            bedrocktools::sdk::Vec3 vel{
                (pos.x - g_state.lastPos.x) / dt,
                (pos.y - g_state.lastPos.y) / dt,
                (pos.z - g_state.lastPos.z) / dt
            };
            g_state.lastPos = pos;

            float blend = clampf(dt * 12.0f, 0.0f, 1.0f);
            g_state.velocity.x += (vel.x - g_state.velocity.x) * blend;
            g_state.velocity.y += (vel.y - g_state.velocity.y) * blend;
            g_state.velocity.z += (vel.z - g_state.velocity.z) * blend;

            bedrocktools::sdk::Vec2 rot = player->rotation();
            float yaw = rot.y;
            float pitch = rot.x;
            if (!g_state.hasPrevYaw) {
                g_state.prevYaw = yaw;
                g_state.hasPrevYaw = true;
            }

            float yawDelta = g_state.prevYaw - yaw;
            while (yawDelta > 180.0f) yawDelta -= 360.0f;
            while (yawDelta < -180.0f) yawDelta += 360.0f;
            g_state.prevYaw = yaw;

            float yawRad = yaw * kDeg2Rad;
            float sinY = std::sin(-yawRad);
            float cosY = std::cos(-yawRad);
            float fwdZ = g_state.velocity.z * cosY - g_state.velocity.x * sinY;
            float fwdX = g_state.velocity.z * sinY + g_state.velocity.x * cosY;

            const float kAccum = 0.0048f * g_cameraMod->m_turningRollAccumulation;
            const float kIntensity = 1.25f * g_cameraMod->m_turningRollIntensity;
            const float kSmooth = 0.0825f * g_cameraMod->m_turningRollSmoothing;

            float pitchScale = std::cos(pitch * kDeg2Rad);
            g_state.rollTarget = clampf(g_state.rollTarget + yawDelta * kAccum * pitchScale, -1.0f, 1.0f);
            g_state.rollTarget -= g_state.rollTarget * dampStep(kSmooth * 100.0f, dt);

            float rollFromTurn = clampf(easeInOutCubic(std::fabs(g_state.rollTarget)), 0.0f, 1.0f)
                               * kIntensity * signf(g_state.rollTarget);
            float rollFromStrafe = -fwdX * g_cameraMod->m_strafingRollIntensity;

            float rollGoal = rollFromTurn + rollFromStrafe;
            g_state.roll += (rollGoal - g_state.roll)
                          * dampStep(g_cameraMod->m_contextTransitionSmoothing, dt);

            float vTarget = g_state.velocity.y;
            if (std::fabs(vTarget) < 0.4f) vTarget = 0.0f;
            vTarget *= kVerticalPitchGain * g_cameraMod->m_verticalPitchIntensity;
            g_state.pitchVertical += (vTarget - g_state.pitchVertical)
                                   * dampStep(g_cameraMod->m_verticalVelocitySmoothing, dt);

            float fTarget = fwdZ * kForwardPitchGain * g_cameraMod->m_forwardPitchIntensity;
            g_state.pitchForward += (fTarget - g_state.pitchForward)
                                  * dampStep(g_cameraMod->m_horizontalVelocitySmoothing, dt);

            float speed = std::sqrt(g_state.velocity.x * g_state.velocity.x +
                                    g_state.velocity.z * g_state.velocity.z);
            if (speed < 0.05f) {
                g_state.idleTime += dt;
            } else {
                g_state.idleTime = 0.0f;
            }

            float fadeTarget = 0.0f;
            if (g_state.idleTime > g_cameraMod->m_swayFadeInDelay) {
                float over = g_state.idleTime - g_cameraMod->m_swayFadeInDelay;
                float len = g_cameraMod->m_swayFadeInLength;
                fadeTarget = len <= 0.0f ? 1.0f : clampf(over / len, 0.0f, 1.0f);
            }
            float fadeRate = fadeTarget > g_state.swayFade
                           ? g_cameraMod->m_swayFadeInLength
                           : g_cameraMod->m_swayFadeOutLength;
            float fadeStep = fadeRate <= 0.0f ? 1.0f : clampf(dt / fadeRate, 0.0f, 1.0f);
            g_state.swayFade += (fadeTarget - g_state.swayFade) * fadeStep;
        });
}

void CameraOverhaulModule::onEnable() {
    Module::onEnable();
    resetState();
}

void CameraOverhaulModule::onDisable() {
    Module::onDisable();
    resetState();
}

void CameraOverhaulModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_verticalPitchIntensity"))  m_verticalPitchIntensity  = j["m_verticalPitchIntensity"].get<float>();
    if (j.contains("m_verticalVelocitySmoothing"))    m_verticalVelocitySmoothing    = j["m_verticalVelocitySmoothing"].get<float>();
    if (j.contains("m_forwardPitchIntensity"))   m_forwardPitchIntensity   = j["m_forwardPitchIntensity"].get<float>();
    if (j.contains("m_horizontalVelocitySmoothing"))  m_horizontalVelocitySmoothing  = j["m_horizontalVelocitySmoothing"].get<float>();
    if (j.contains("m_turningRollIntensity"))            m_turningRollIntensity            = j["m_turningRollIntensity"].get<float>();
    if (j.contains("m_turningRollAccumulation"))      m_turningRollAccumulation      = j["m_turningRollAccumulation"].get<float>();
    if (j.contains("m_turningRollSmoothing"))         m_turningRollSmoothing         = j["m_turningRollSmoothing"].get<float>();
    if (j.contains("m_strafingRollIntensity"))           m_strafingRollIntensity           = j["m_strafingRollIntensity"].get<float>();
    if (j.contains("m_swayIntensity"))                m_swayIntensity                = j["m_swayIntensity"].get<float>();
    if (j.contains("m_swayFrequency"))                m_swayFrequency                = j["m_swayFrequency"].get<float>();
    if (j.contains("m_swayFadeInDelay"))              m_swayFadeInDelay              = j["m_swayFadeInDelay"].get<float>();
    if (j.contains("m_swayFadeInLength"))             m_swayFadeInLength             = j["m_swayFadeInLength"].get<float>();
    if (j.contains("m_swayFadeOutLength"))            m_swayFadeOutLength            = j["m_swayFadeOutLength"].get<float>();
    if (j.contains("m_contextTransitionSmoothing"))   m_contextTransitionSmoothing   = j["m_contextTransitionSmoothing"].get<float>();
    if (j.contains("m_enableSway"))                   m_enableSway                   = j["m_enableSway"].get<bool>();
    if (j.contains("m_enableRoll"))                   m_enableRoll                   = j["m_enableRoll"].get<bool>();
    if (j.contains("m_enablePitch"))                  m_enablePitch                  = j["m_enablePitch"].get<bool>();
}

void CameraOverhaulModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_verticalPitchIntensity"]  = m_verticalPitchIntensity;
    j["m_verticalVelocitySmoothing"]    = m_verticalVelocitySmoothing;
    j["m_forwardPitchIntensity"]   = m_forwardPitchIntensity;
    j["m_horizontalVelocitySmoothing"]  = m_horizontalVelocitySmoothing;
    j["m_turningRollIntensity"]            = m_turningRollIntensity;
    j["m_turningRollAccumulation"]      = m_turningRollAccumulation;
    j["m_turningRollSmoothing"]         = m_turningRollSmoothing;
    j["m_strafingRollIntensity"]           = m_strafingRollIntensity;
    j["m_swayIntensity"]                = m_swayIntensity;
    j["m_swayFrequency"]                = m_swayFrequency;
    j["m_swayFadeInDelay"]              = m_swayFadeInDelay;
    j["m_swayFadeInLength"]             = m_swayFadeInLength;
    j["m_swayFadeOutLength"]            = m_swayFadeOutLength;
    j["m_contextTransitionSmoothing"]   = m_contextTransitionSmoothing;
    j["m_enableSway"]                   = m_enableSway;
    j["m_enableRoll"]                   = m_enableRoll;
    j["m_enablePitch"]                  = m_enablePitch;
}
