/*
 * Copyright (C) 2018 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "LightService"

#include "Light.h"

#include <android-base/logging.h>

namespace {

using android::hardware::light::V2_0::LightState;

static constexpr int RAMP_SIZE = 8;
static constexpr int RAMP_STEP_DURATION = 50;
static constexpr int BRIGHTNESS_RAMP[RAMP_SIZE] = {0, 12, 25, 37, 50, 72, 85, 100};
static constexpr int MAX_BRIGHTNESS = 255;

static uint32_t rgbToBrightness(const LightState& state) {
    uint32_t color = state.color & 0x00ffffff;
    return ((77 * ((color >> 16) & 0xff)) + (150 * ((color >> 8) & 0xff)) +
            (29 * (color & 0xff))) >> 8;
}

static bool isLit(const LightState& state) {
    return (state.color & 0x00ffffff);
}

static std::string getScaledDutyPcts(int brightness) {
    std::string buf, pad;

    for (auto i : BRIGHTNESS_RAMP) {
        buf += pad;
        buf += std::to_string(i * brightness / 255);
        pad = ",";
    }

    return buf;
}

}  // anonymous namespace

namespace android {
namespace hardware {
namespace light {
namespace V2_0 {
namespace implementation {

static const std::string kSysLedPath = "/sys/class/leds";

Led::Led(int index, const std::string& name) : mIndex(index), mName(name) {
    std::string path = kSysLedPath + "/" + name;

    mBrightness.open(path + "/brightness");
    mDutyPcts.open(path + "/duty_pcts");
    mStartIdx.open(path + "/start_idx");
    mPauseLo.open(path + "/pause_lo");
    mPauseHi.open(path + "/pause_hi");
    mRampStepMs.open(path + "/ramp_step_ms");
    mBlink.open(path + "/blink");
}

Led::operator bool() const {
    return mBrightness &&
           mDutyPcts &&
           mStartIdx &&
           mPauseLo &&
           mPauseHi &&
           mRampStepMs &&
           mBlink;
}

bool Led::operator!() const {
    return !mBrightness ||
           !mDutyPcts ||
           !mStartIdx ||
           !mPauseLo ||
           !mPauseHi ||
           !mRampStepMs ||
           !mBlink;
}

void Led::setBrightness(int value) {
    LOG(INFO) << "setBrightness: brightness=" << value;
    mBlink << 0 << std::endl;
    mBrightness << value << std::endl;
}

void Led::setBlink(int brightness, int onMs, int offMs) {
    int stepDuration = RAMP_STEP_DURATION;
    int pauseHi = onMs - (stepDuration * RAMP_SIZE * 2);
    if (stepDuration * RAMP_SIZE * 2 > onMs) {
        stepDuration = onMs / (RAMP_SIZE * 2);
        pauseHi = 0;
    }

    mStartIdx << (mIndex * RAMP_SIZE) << std::endl;
    mDutyPcts << getScaledDutyPcts(brightness) << std::endl;
    mPauseLo << offMs << std::endl;
    mPauseHi << pauseHi << std::endl;
    mRampStepMs << stepDuration << std::endl;
}

Light::Light(std::pair<std::ofstream, uint32_t>&& lcd_backlight,
             std::vector<std::ofstream>&& button_backlight,
             Led&& red_led, Led&& green_led, Led&& blue_led,
             std::ofstream&& rgb_blink)
    : mLcdBacklight(std::move(lcd_backlight)),
      mRedLed(std::move(red_led)),
      mGreenLed(std::move(green_led)),
      mBlueLed(std::move(blue_led)),
      mRgbBlink(std::move(rgb_blink)) {
    auto attnFn(std::bind(&Light::setAttentionLight, this, std::placeholders::_1));
    auto backlightFn(std::bind(&Light::setLcdBacklight, this, std::placeholders::_1));
    auto batteryFn(std::bind(&Light::setBatteryLight, this, std::placeholders::_1));
    auto notifFn(std::bind(&Light::setNotificationLight, this, std::placeholders::_1));
    mLights.emplace(std::make_pair(Type::ATTENTION, attnFn));
    mLights.emplace(std::make_pair(Type::BACKLIGHT, backlightFn));
    mLights.emplace(std::make_pair(Type::BATTERY, batteryFn));
    mLights.emplace(std::make_pair(Type::NOTIFICATIONS, notifFn));
}

// Methods from ::android::hardware::light::V2_0::ILight follow.
Return<Status> Light::setLight(Type type, const LightState& state) {
    auto it = mLights.find(type);

    if (it == mLights.end()) {
        return Status::LIGHT_NOT_SUPPORTED;
    }

    it->second(state);

    return Status::SUCCESS;
}

Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb) {
    std::vector<Type> types;

    for (auto const& light : mLights) {
        types.push_back(light.first);
    }

    _hidl_cb(types);

    return Void();
}

void Light::setAttentionLight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);
    mAttentionState = state;
    setSpeakerBatteryLightLocked();
}

void Light::setLcdBacklight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);

    uint32_t brightness = rgbToBrightness(state);

    // If max panel brightness is not the default (255),
    // apply linear scaling across the accepted range.
    if (mLcdBacklight.second != MAX_BRIGHTNESS) {
        int old_brightness = brightness;
        brightness = brightness * mLcdBacklight.second / MAX_BRIGHTNESS;
        LOG(VERBOSE) << "scaling brightness " << old_brightness << " => " << brightness;
    }

    mLcdBacklight.first << brightness << std::endl;
}

void Light::setBatteryLight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);
    mBatteryState = state;
    setSpeakerBatteryLightLocked();
}

void Light::setNotificationLight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);
    mNotificationState = state;
    setSpeakerBatteryLightLocked();
}

void Light::setSpeakerBatteryLightLocked() {
    if (isLit(mNotificationState)) {
        setSpeakerLightLocked(mNotificationState);
    } else if (isLit(mAttentionState)) {
        setSpeakerLightLocked(mAttentionState);
    } else if (isLit(mBatteryState)) {
        setSpeakerLightLocked(mBatteryState);
    } else {
        // Lights off
        mRedLed.off();
        mGreenLed.off();
        mBlueLed.off();
    }
}

void Light::setSpeakerLightLocked(const LightState& state) {
    int red, green, blue, blink;
    uint32_t alpha;

    // Extract brightness from AARRGGBB
    alpha = (state.color >> 24) & 0xff;

    // Retrieve each of the RGB colors
    red = (state.color >> 16) & 0xff;
    green = (state.color >> 8) & 0xff;
    blue = state.color & 0xff;

    // Scale RGB colors if a brightness has been applied by the user
    if (alpha != 0xff) {
        red = (red * alpha) / 0xff;
        green = (green * alpha) / 0xff;
        blue = (blue * alpha) / 0xff;
    }

    // Disable all blinking to start
    mRgbBlink << 0 << std::endl;

    switch (state.flashMode) {
        case Flash::TIMED:
            mRedLed.setBlink(red, state.flashOnMs, state.flashOffMs);
            mGreenLed.setBlink(green, state.flashOnMs, state.flashOffMs);
            mBlueLed.setBlink(blue, state.flashOnMs, state.flashOffMs);
            // Start the party
            mRgbBlink << 1 << std::endl;
            break;
        case Flash::NONE:
        default:
            mRedLed.setBrightness(red);
            mGreenLed.setBrightness(green);
            mBlueLed.setBrightness(blue);
            break;
    }
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace light
}  // namespace hardware
}  // namespace android
