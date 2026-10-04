// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "ControllerLayout.h"

#include <set>
#include <string>

namespace
{

bool Has(const std::vector<ControllerInput>& inputs, const std::string& path, ControllerSource source)
{
    for (const ControllerInput& input : inputs)
    {
        if (input.path == path)
            return input.source == source;
    }
    return false;
}

} // namespace

TEST_CASE("Controller layouts default to the frame controller", "[ControllerLayout]")
{
    CHECK(std::string(LayoutForControllers("frame").controllerType) == "frame_controller");
    CHECK(std::string(LayoutForControllers("").name) == "frame");
    CHECK(std::string(LayoutForControllers("quest").name) == "frame");
    CHECK(std::string(LayoutForControllers("index").controllerType) == "knuckles");
    CHECK(std::string(LayoutForControllers("touch_plus").controllerType) == "oculus_touch");
}

TEST_CASE("Controller layouts give each hand its own buttons and no path twice", "[ControllerLayout]")
{
    for (const char* name : {"frame", "index", "touch_plus"})
    {
        INFO(name);
        const ControllerLayout& layout = LayoutForControllers(name);
        for (const std::vector<ControllerInput>* hand : {&layout.left, &layout.right})
        {
            std::set<std::string> paths;
            for (const ControllerInput& input : *hand)
                CHECK(paths.insert(input.path).second);
            CHECK(Has(*hand, "/input/trigger/value", ControllerSource::Trigger));
            CHECK(Has(*hand, "/input/grip/value", ControllerSource::Grip));
        }
    }
    const ControllerLayout& frame = LayoutForControllers("frame");
    CHECK(Has(frame.left, "/input/dpad_down/click", ControllerSource::Lower));
    CHECK(Has(frame.right, "/input/a/click", ControllerSource::Lower));
    CHECK(Has(frame.right, "/input/menu/click", ControllerSource::Menu));
    // Control: the frame controller's left hand has a d-pad, not A.
    CHECK_FALSE(Has(frame.left, "/input/a/click", ControllerSource::Lower));
    CHECK(Has(LayoutForControllers("index").left, "/input/finger/index", ControllerSource::Trigger));
    CHECK(Has(LayoutForControllers("touch_plus").left, "/input/system/click", ControllerSource::Menu));
}

TEST_CASE("Controller sources read the hand's own buttons", "[ControllerLayout]")
{
    oxr::protocol::TrackingPacket packet = {};
    packet.buttonState = oxr::protocol::BUTTON_X | oxr::protocol::BUTTON_MENU;
    packet.leftTrigger = 0.6f;
    packet.rightThumbstick[1] = -0.5f;
    CHECK(ControllerSourceValue(ControllerSource::Lower, packet, true) == 1.0f);
    CHECK(ControllerSourceValue(ControllerSource::Lower, packet, false) == 0.0f);
    CHECK(ControllerSourceValue(ControllerSource::TriggerClick, packet, true) == 1.0f);
    CHECK(ControllerSourceValue(ControllerSource::TriggerClick, packet, false) == 0.0f);
    CHECK(ControllerSourceValue(ControllerSource::StickY, packet, false) == -0.5f);
    CHECK(ControllerSourceValue(ControllerSource::StickTouch, packet, false) == 1.0f);
    CHECK(ControllerSourceValue(ControllerSource::StickTouch, packet, true) == 0.0f);
    CHECK(ControllerSourceValue(ControllerSource::Menu, packet, false) == 1.0f);
}
