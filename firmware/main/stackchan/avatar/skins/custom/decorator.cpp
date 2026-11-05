/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "custom.h"
#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

static const std::string_view _tag = "custom-face";

// Same two-frame wobble the built-in heart decorator uses.
static const int _pulse_frames[2]     = {150, 200};
static const uint32_t _pulse_interval = 500;

SpriteDecorator::SpriteDecorator(lv_obj_t* parent, const DecoratorSpec_t& spec, const Vector2i& anchorPoint)
{
    _dsc = assets::get_image(spec.image);
    if (_dsc.data == nullptr) {
        mclog::tagWarn(_tag, "decorator image missing: {}", spec.image);
        return;
    }

    _image = std::make_unique<Image>(parent);
    _image->setAlign(LV_ALIGN_CENTER);
    _image->setSrc(&_dsc);
    _image->setPivot(_image->getWidth() / 2, _image->getHeight() / 2);
    _image->setPos(anchorPoint.x + spec.offset.x, anchorPoint.y + spec.offset.y);

    const uint32_t now = GetHAL().millis();

    if (spec.lifetimeMs > 0) {
        _destroy_at   = now + spec.lifetimeMs;
        _has_lifetime = true;
    }

    if (spec.animate == "pulse") {
        _pulsing         = true;
        _next_pulse_tick = now + _pulse_interval;
        _image->setRotation(_pulse_frames[0]);
    }
}

SpriteDecorator::~SpriteDecorator()
{
    _image.reset();
}

void SpriteDecorator::_update()
{
    if (!_image) {
        // Nothing rendered, so there is nothing to keep alive.
        requestDestroy();
        return;
    }

    const uint32_t now = GetHAL().millis();

    if (_has_lifetime && now >= _destroy_at) {
        requestDestroy();
        return;
    }

    if (_pulsing && now >= _next_pulse_tick) {
        _next_pulse_tick = now + _pulse_interval;
        _pulse_index     = (_pulse_index + 1) % 2;
        _image->setRotation(_pulse_frames[_pulse_index]);
    }
}

void SpriteDecorator::setVisible(bool visible)
{
    Element::setVisible(visible);

    if (_image) {
        _image->setHidden(!visible);
    }
}
