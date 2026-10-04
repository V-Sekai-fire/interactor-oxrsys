// SPDX-License-Identifier: MPL-2.0

#include "ControllerLayout.h"

namespace
{

using S = ControllerSource;

std::vector<ControllerInput> Common(const char* stick, bool gripClick)
{
    const std::string base = std::string("/input/") + stick;
    std::vector<ControllerInput> inputs = {
        {"/input/trigger/value", S::Trigger, true, false},
        {"/input/trigger/touch", S::TriggerTouch, false, false},
        {"/input/grip/value", S::Grip, true, false},
        {"/input/grip/touch", S::GripTouch, false, false},
        {base + "/x", S::StickX, true, true},
        {base + "/y", S::StickY, true, true},
        {base + "/click", S::StickClick, false, false},
        {base + "/touch", S::StickTouch, false, false},
    };
    if (gripClick)
    {
        inputs.push_back({"/input/trigger/click", S::TriggerClick, false, false});
        inputs.push_back({"/input/grip/click", S::GripClick, false, false});
    }
    return inputs;
}

std::vector<ControllerInput> With(std::vector<ControllerInput> inputs, std::vector<ControllerInput> extra)
{
    inputs.insert(inputs.end(), extra.begin(), extra.end());
    return inputs;
}

std::vector<ControllerLayout> MakeLayouts()
{
    std::vector<ControllerLayout> layouts;
    // The left hand's d-pad mirrors the right hand's face buttons, so X and Y sit where A and B do.
    layouts.push_back({"frame", "frame_controller", "{frame_controller}/input/frame_controller_profile.json",
                       "/interaction_profiles/meta/touch_plus_controller",
                       With(Common("thumbstick", true), {{"/input/dpad_down/click", S::Lower, false, false},
                                                         {"/input/dpad_down/touch", S::Lower, false, false},
                                                         {"/input/dpad_right/click", S::Upper, false, false},
                                                         {"/input/dpad_right/touch", S::Upper, false, false}}),
                       With(Common("thumbstick", true), {{"/input/a/click", S::Lower, false, false},
                                                         {"/input/a/touch", S::Lower, false, false},
                                                         {"/input/b/click", S::Upper, false, false},
                                                         {"/input/b/touch", S::Upper, false, false},
                                                         {"/input/menu/click", S::Menu, false, false}})});
    // Each hand has A and B; the curl of the fingers follows the trigger and the grip.
    const std::vector<ControllerInput> indexExtra = {{"/input/a/click", S::Lower, false, false},
                                                     {"/input/a/touch", S::Lower, false, false},
                                                     {"/input/b/click", S::Upper, false, false},
                                                     {"/input/b/touch", S::Upper, false, false},
                                                     {"/input/trigger/click", S::TriggerClick, false, false},
                                                     {"/input/grip/force", S::Grip, true, false},
                                                     {"/input/finger/index", S::Trigger, true, false},
                                                     {"/input/finger/middle", S::Grip, true, false},
                                                     {"/input/finger/ring", S::Grip, true, false},
                                                     {"/input/finger/pinky", S::Grip, true, false}};
    layouts.push_back({"index", "knuckles", "{indexcontroller}/input/index_controller_profile.json",
                       "/interaction_profiles/valve/index_controller", With(Common("thumbstick", false), indexExtra),
                       With(Common("thumbstick", false), indexExtra)});
    layouts.push_back({"touch_plus", "oculus_touch", "{oculus}/input/touch_profile.json",
                       "/interaction_profiles/meta/touch_plus_controller",
                       With(Common("joystick", false), {{"/input/x/click", S::Lower, false, false},
                                                        {"/input/x/touch", S::Lower, false, false},
                                                        {"/input/y/click", S::Upper, false, false},
                                                        {"/input/y/touch", S::Upper, false, false},
                                                        {"/input/system/click", S::Menu, false, false}}),
                       With(Common("joystick", false), {{"/input/a/click", S::Lower, false, false},
                                                        {"/input/a/touch", S::Lower, false, false},
                                                        {"/input/b/click", S::Upper, false, false},
                                                        {"/input/b/touch", S::Upper, false, false}})});
    return layouts;
}

} // namespace

const ControllerLayout& LayoutForControllers(const std::string& name)
{
    static const std::vector<ControllerLayout> layouts = MakeLayouts();
    for (const ControllerLayout& layout : layouts)
    {
        if (name == layout.name)
            return layout;
    }
    return layouts.front();
}

float ControllerSourceValue(ControllerSource source, const oxr::protocol::TrackingPacket& packet, bool left)
{
    using namespace oxr::protocol;
    const uint32_t b = packet.buttonState;
    const float trigger = left ? packet.leftTrigger : packet.rightTrigger;
    const float grip = left ? packet.leftGrip : packet.rightGrip;
    const float* stick = left ? packet.leftThumbstick : packet.rightThumbstick;
    const bool stickClick = (b & (left ? BUTTON_LEFT_THUMBSTICK : BUTTON_RIGHT_THUMBSTICK)) != 0;
    switch (source)
    {
        case S::Trigger:
            return trigger;
        case S::TriggerClick:
            return trigger > 0.5f ? 1.0f : 0.0f;
        case S::TriggerTouch:
            return trigger > 0.0f ? 1.0f : 0.0f;
        case S::Grip:
            return grip;
        case S::GripClick:
            return grip > 0.5f ? 1.0f : 0.0f;
        case S::GripTouch:
            return grip > 0.0f ? 1.0f : 0.0f;
        case S::StickX:
            return stick[0];
        case S::StickY:
            return stick[1];
        case S::StickClick:
            return stickClick ? 1.0f : 0.0f;
        case S::StickTouch:
            return stickClick || stick[0] != 0.0f || stick[1] != 0.0f ? 1.0f : 0.0f;
        case S::Lower:
            return (b & (left ? BUTTON_X : BUTTON_A)) != 0 ? 1.0f : 0.0f;
        case S::Upper:
            return (b & (left ? BUTTON_Y : BUTTON_B)) != 0 ? 1.0f : 0.0f;
        case S::Menu:
            return (b & BUTTON_MENU) != 0 ? 1.0f : 0.0f;
    }
    return 0.0f;
}
