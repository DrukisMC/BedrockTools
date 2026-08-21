#pragma once

#include "../Module.hpp"

class CameraOverhaulModule : public Module {
public:
    CameraOverhaulModule();
    ~CameraOverhaulModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void resetState();

    float m_verticalPitchIntensity   = 1.0f;
    float m_verticalVelocitySmoothing     = 1.0f;
    float m_forwardPitchIntensity    = 1.0f;
    float m_horizontalVelocitySmoothing   = 1.0f;
    float m_turningRollIntensity             = 1.0f;
    float m_turningRollAccumulation       = 1.0f;
    float m_turningRollSmoothing          = 1.0f;
    float m_strafingRollIntensity            = 1.0f;
    float m_swayIntensity                 = 1.0f;
    float m_swayFrequency                 = 1.0f;
    float m_swayFadeInDelay               = 3.0f;
    float m_swayFadeInLength              = 4.0f;
    float m_swayFadeOutLength             = 1.0f;
    float m_contextTransitionSmoothing    = 1.0f;

    bool m_enableSway   = true;
    bool m_enableRoll   = true;
    bool m_enablePitch  = true;

private:
    bool m_hooked = false;
};
