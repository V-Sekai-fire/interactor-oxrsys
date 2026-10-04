// SPDX-License-Identifier: MPL-2.0
//
// The controllers OXRSys presents, chosen by the `controllers` config key: the PC VR runtime's
// controller type and input profile, the OpenXR interaction profile, and which packet value drives
// each input.

#pragma once

#include <oxrsys/protocol/Protocol.h>

#include <string>
#include <vector>

enum class ControllerSource
{
    Trigger,
    TriggerClick,
    TriggerTouch,
    Grip,
    GripClick,
    GripTouch,
    StickX,
    StickY,
    StickClick,
    StickTouch,
    Lower, // A on the right hand, X on the left
    Upper, // B on the right hand, Y on the left
    Menu,
};

struct ControllerInput
{
    std::string path;
    ControllerSource source;
    bool scalar;
    bool twoSided;
};

struct ControllerLayout
{
    const char* name;
    const char* controllerType;
    const char* inputProfilePath;
    const char* interactionProfile;
    std::vector<ControllerInput> left;
    std::vector<ControllerInput> right;
};

// The layout for a `controllers` value; anything unknown gets the default, "frame".
const ControllerLayout& LayoutForControllers(const std::string& name);

// The value a source reads from a tracking packet for one hand; booleans are 0 or 1.
float ControllerSourceValue(ControllerSource source, const oxr::protocol::TrackingPacket& packet, bool left);
