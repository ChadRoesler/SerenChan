/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../avatar/skins/custom/custom.h"
#include <hal/hal.h>

namespace stackchan {

/**
 * @brief Runs the motion a sprite face's current pose asks for (face.yaml: motion)
 */
class PoseMotionModifier : public Modifier {
public:
    void _update(Modifiable& stackchan) override
    {
        if (!stackchan.hasAvatar() || stackchan.avatar().isModifyLocked()) {
            return;
        }
        if (auto* custom = dynamic_cast<avatar::CustomAvatar*>(&stackchan.avatar())) {
            custom->tickMotion(GetHAL().millis());
        }
    }
};

}  // namespace stackchan
