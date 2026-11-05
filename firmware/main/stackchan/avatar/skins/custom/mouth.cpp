/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "custom.h"
#include <mooncake_log.h>

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

static const std::string_view _tag = "custom-face";

MouthRig::MouthRig(lv_obj_t* parent, const FaceManifest_t& manifest)
{
    _anchor = manifest.mouthPos;
    _travel = manifest.mouthTravel;

    for (const auto& entry : manifest.mouthShapes) {
        SpriteSet set;
        if (set.load(entry.second)) {
            _shapes.emplace(entry.first, std::move(set));
        }
    }

    if (_shapes.empty()) {
        // A face without a mouth still blinks and emotes. Do not fail the face.
        mclog::tagWarn(_tag, "no mouth shapes loaded, mouth disabled");
        return;
    }

    // Prefer a shape called "neutral", otherwise take whichever sorts first.
    auto it  = _shapes.find("neutral");
    _current = (it != _shapes.end()) ? &it->second : &_shapes.begin()->second;

    _image = std::make_unique<Image>(parent);
    _image->setAlign(LV_ALIGN_CENTER);
    _image->setSrc(_current->frame(0));
    _image->setPivot(_image->getWidth() / 2, _image->getHeight() / 2);

    setWeight(0);
    setPosition(_position);
}

MouthRig::~MouthRig()
{
    _image.reset();
}

void MouthRig::applyShape(const std::string& shape)
{
    if (shape.empty()) {
        return;
    }

    auto it = _shapes.find(shape);
    if (it == _shapes.end()) {
        // Unknown shape: keep whatever is showing rather than blanking the mouth.
        return;
    }

    _current = &it->second;
    applyFrame();
}

void MouthRig::applyFrame()
{
    if (!_image || !_current) {
        return;
    }

    // weight 0 == closed == frame 0. SpeakingModifier toggles this every 180ms,
    // which is what produces the talk cycle.
    if (auto* frame = _current->frameForValue(_weight, false)) {
        _image->setSrc(frame);
    }
}

void MouthRig::setPosition(const Vector2i& position)
{
    Element::setPosition(position);

    if (!_image) {
        return;
    }

    auto pos_x = _anchor.x + map_range(_position.x, -100, 100, -_travel.x, _travel.x);
    auto pos_y = _anchor.y + map_range(_position.y, -100, 100, -_travel.y, _travel.y);

    _image->setPos(pos_x, pos_y);
}

void MouthRig::setWeight(int weight)
{
    Feature::setWeight(weight);
    applyFrame();
}

void MouthRig::setRotation(int rotation)
{
    Element::setRotation(rotation);

    if (_image) {
        _image->setRotation(_rotation);
    }
}

void MouthRig::setVisible(bool visible)
{
    Element::setVisible(visible);

    if (_image) {
        _image->setHidden(!visible);
    }
}
