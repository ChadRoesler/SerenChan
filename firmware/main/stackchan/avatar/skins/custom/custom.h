/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../../avatar/avatar.h"
#include "../../avatar/elements/feature.h"
#include "../../avatar/decorator.h"
#include "../default/default.h"
#include <lvgl.h>
#include <smooth_lvgl.hpp>
#include "../../../utils/psram_alloc.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace stackchan::avatar {

/**
 * @brief An ordered set of image frames loaded from the assets partition
 *
 * Owns the lv_image_dsc_t values so their addresses stay valid for the
 * lifetime of the lv_image widgets that point at them. assets::get_image()
 * returns by value, so a temporary would dangle.
 */
class SpriteSet {
public:
    bool load(const std::vector<std::string>& names);

    /** @brief Frame by index, clamped. nullptr when empty. */
    const lv_image_dsc_t* frame(int index) const;

    /** @brief Map 0~100 onto a frame. invert: 0 selects the last frame. */
    const lv_image_dsc_t* frameForValue(int value, bool invert) const;

    int count() const
    {
        return static_cast<int>(_frames.size());
    }
    bool empty() const
    {
        return _frames.empty();
    }

private:
    PsramVector<lv_image_dsc_t> _frames;
};

/**
 * @brief Named sprites, e.g. lid shapes keyed "open" / "tight" / "angle_down"
 *
 * std::map keeps element addresses stable across inserts, which matters
 * because lv_image holds a raw pointer to the descriptor.
 */
class SpriteMap {
public:
    /** @param names asset name keyed by pose name */
    bool load(const std::map<std::string, std::string>& names);

    /** @return nullptr when the key is unknown */
    const lv_image_dsc_t* get(const std::string& key) const;

    /** @brief The first sprite loaded, nullptr when empty */
    const lv_image_dsc_t* first() const
    {
        return _sprites.empty() ? nullptr : &_sprites.front().sprite;
    }

    bool empty() const
    {
        return _sprites.empty();
    }

private:
    // Fixed-size key for the same reason as the SD asset table: a std::string
    // per entry would be a sub-512-byte allocation in internal SRAM.
    struct Entry_t {
        char key[24] = {0};
        lv_image_dsc_t sprite{};
    };
    PsramVector<Entry_t> _sprites;
};

/**
 * @brief One emotion, resolved to the parts that express it
 *
 * Produced by the YAML compiler. See firmware/docs/custom-faces.md
 */
/**
 * @brief One eye flicking to another lid drawing now and then, while a pose holds
 *
 * Swapping drawings rather than nudging a lid works on both rigs and stays pixel art.
 */
struct PoseTwitch_t {
    bool on = false;
    std::string eye = "right";  // left | right | both (screen sides)
    std::string lid = "closed"; // an upper lid variant
    int everyMinMs  = 2500;     // the pause between twitches
    int everyMaxMs  = 6000;
    int flutters    = 3;        // flicks in one twitch
};

struct EmotionPose_t {
    std::string upperLid;
    std::string lowerLid;
    std::string mouthShape;
    std::string decorator;
    int eyeOpen     = 100;  // lid position, 0~100
    int browOffsetY = 0;
    int browRotation = 0;  // tenths of a degree, mirrored on the right brow

    // Face Studio's rig. Empty means "the same as the plain field above", so an
    // older face.json reads exactly as it always did.
    std::string upperLidLeft, upperLidRight;   // one eye each: a wink
    std::string lowerLidLeft, lowerLidRight;
    std::string browShapeLeft, browShapeRight; // a drawn brow per emotion, no rotation smear
    std::vector<std::string> decorators;       // every decorator, in order (decorator is the first)
    std::string baseImage;                     // this emotion's own backdrop (the clown)
    bool overlaySet = false;                   // true: overlay below replaces the face's
    std::string overlay;                       // "" with overlaySet = no hair over
    PoseTwitch_t twitch;
};

/**
 * @brief A decorator image plus where to put it
 */
struct DecoratorSpec_t {
    std::string image;
    std::string anchor = "face";  // face | left_eye | right_eye | mouth
    uitk::Vector2i offset{0, 0};
    std::string animate;  // "" | "pulse"
    uint32_t lifetimeMs = 0;
};

/**
 * @brief Everything a custom face needs, compiled from face.yaml into face.json
 *
 * Every field has a usable default so a partial manifest still renders.
 */
struct FaceManifest_t {
    bool valid = false;
    std::string name = "unnamed";
    int version = 2;

    lv_color_t background = lv_color_black();
    std::string baseImage;
    std::string overlayImage;  // hair over the face: above the rig, under the decorators

    /* ----------------------------------- Eyes ---------------------------------- */
    // Left/right are resolved at build time, so mirroring costs nothing here.
    std::string scleraLeft, scleraRight;
    std::string irisLeft, irisRight;
    std::map<std::string, std::string> lidsUpperLeft, lidsUpperRight;
    std::map<std::string, std::string> lidsLowerLeft, lidsLowerRight;

    uitk::Vector2i leftEyePos{-52, -18};
    uitk::Vector2i rightEyePos{52, -18};
    uitk::Vector2i socketSize{56, 48};
    uitk::Vector2i irisTravel{12, 8};
    int irisParallax = 25;  // percent of the gaze offset applied to the socket
    int clipRadius   = 0;   // >0 rounds the socket clip

    // lidMode "swap": lids sit where drawn, a blink shows "closed". Default slides.
    bool lidSwap = false;
    // irisMask "sclera": the iris is masked by the eye white (A8 masks below)
    bool irisMaskSclera = false;
    std::string scleraMaskLeft, scleraMaskRight;

    /* ---------------------------------- Mouth ---------------------------------- */
    // Each shape is an openness sequence: frame 0 closed, last frame widest.
    std::map<std::string, std::vector<std::string>> mouthShapes;
    uitk::Vector2i mouthPos{0, 46};
    uitk::Vector2i mouthTravel{8, 6};

    /* ---------------------------------- Brows ---------------------------------- */
    std::string browLeftImage, browRightImage;
    uitk::Vector2i leftBrowPos{-52, -46};
    uitk::Vector2i rightBrowPos{52, -46};
    std::map<std::string, std::string> browShapesLeft, browShapesRight;  // shape name -> asset
    bool browsAboveEyes = false;  // drawn after the eyes: no lid ever covers them

    /* -------------------------------- Expression ------------------------------- */
    std::map<std::string, DecoratorSpec_t> decorators;
    std::map<std::string, EmotionPose_t> emotions;

    /** @brief Load and parse face.json from the assets partition */
    static FaceManifest_t load();

    /** @return nullptr when the emotion is not described */
    const EmotionPose_t* pose(const std::string& emotion) const;
};

/**
 * @brief Four-layer eye: sclera, iris, lower lid, upper lid
 *
 * The layers live inside a Container, which LVGL clips children to. That is
 * what keeps the iris inside the eye and lets the upper lid slide out of view
 * to open -- the same trick the stock skin uses for blinking.
 *
 *   Container "socket"   <- clips
 *     sclera             static
 *     iris               moves for gaze
 *     lower lid          emotion shape
 *     upper lid          emotion shape, slides down to blink
 */
class EyeRig : public Feature {
public:
    EyeRig(lv_obj_t* parent, const FaceManifest_t& manifest, bool isLeftEye);
    ~EyeRig();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setVisible(bool visible) override;
    void setSize(int size) override;

    /** @brief Select lid shapes by name. Unknown names leave the lid unchanged. */
    void applyLids(const std::string& upper, const std::string& lower);

    bool isValid() const
    {
        return _socket != nullptr;
    }

    /** @brief Screen-space centre of this eye, for anchoring decorators */
    uitk::Vector2i anchorPoint() const
    {
        return _anchor;
    }

private:
    bool _is_left_eye = false;
    uitk::Vector2i _anchor;
    uitk::Vector2i _iris_travel;
    int _parallax = 25;

    lv_image_dsc_t _sclera_dsc{};
    lv_image_dsc_t _iris_dsc{};
    lv_image_dsc_t _mask_dsc{};
    SpriteMap _lids_upper;
    SpriteMap _lids_lower;

    // swap lids: the pose's sprites, the closed ones, and what is showing now
    bool _lid_swap = false;
    const lv_image_dsc_t* _upper_pose   = nullptr;
    const lv_image_dsc_t* _lower_pose   = nullptr;
    const lv_image_dsc_t* _upper_closed = nullptr;
    const lv_image_dsc_t* _lower_closed = nullptr;
    const lv_image_dsc_t* _upper_shown  = nullptr;
    const lv_image_dsc_t* _lower_shown  = nullptr;

    std::unique_ptr<uitk::lvgl_cpp::Container> _socket;
    std::unique_ptr<uitk::lvgl_cpp::Container> _iris_window;  // masked by the white, holds the iris
    std::unique_ptr<uitk::lvgl_cpp::Image> _sclera;
    std::unique_ptr<uitk::lvgl_cpp::Image> _iris;
    std::unique_ptr<uitk::lvgl_cpp::Image> _lid_lower;
    std::unique_ptr<uitk::lvgl_cpp::Image> _lid_upper;

    int _lid_upper_height = 0;

    void applyLidPosition();
};

/**
 * @brief Mouth with a shape per emotion and an openness sequence per shape
 *
 * setWeight() drives openness, which is what SpeakingModifier toggles for lip
 * sync. Changing shape is an emotion-level decision, not a per-frame one.
 */
class MouthRig : public Feature {
public:
    MouthRig(lv_obj_t* parent, const FaceManifest_t& manifest);
    ~MouthRig();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setVisible(bool visible) override;

    /** @brief Select a shape by name. Unknown names keep the current shape. */
    void applyShape(const std::string& shape);

    bool isValid() const
    {
        return _image != nullptr;
    }

    uitk::Vector2i anchorPoint() const
    {
        return _anchor;
    }

private:
    uitk::Vector2i _anchor;
    uitk::Vector2i _travel;
    std::map<std::string, SpriteSet> _shapes;
    SpriteSet* _current = nullptr;
    std::unique_ptr<uitk::lvgl_cpp::Image> _image;

    void applyFrame();
};

/**
 * @brief Image-backed eyebrow, driven directly by CustomAvatar
 *
 * Cheap and high value: offset plus tilt carries most of the emotional read.
 */
class SpriteBrow {
public:
    SpriteBrow(lv_obj_t* parent, const FaceManifest_t& manifest, bool isLeftBrow);
    ~SpriteBrow();

    /** @param shape a drawn brow shape; "" or unknown keeps the current image */
    void applyPose(int offsetY, int rotation, const std::string& shape = "");
    void setVisible(bool visible);
    bool isValid() const
    {
        return _image != nullptr;
    }

private:
    bool _is_left_brow = false;
    uitk::Vector2i _anchor;
    lv_image_dsc_t _dsc{};
    SpriteMap _shapes;
    std::unique_ptr<uitk::lvgl_cpp::Image> _image;
};

/**
 * @brief A manifest-described decorator: one image at an anchored offset
 *
 * Lives in the avatar's decorator pool, so it is destroyed the same way the
 * built-in heart and sweat decorators are.
 */
class SpriteDecorator : public Decorator {
public:
    SpriteDecorator(lv_obj_t* parent, const DecoratorSpec_t& spec, const uitk::Vector2i& anchorPoint);
    ~SpriteDecorator();

    void _update() override;
    void setVisible(bool visible) override;

private:
    lv_image_dsc_t _dsc{};
    std::unique_ptr<uitk::lvgl_cpp::Image> _image;

    bool _has_lifetime = false;
    uint32_t _destroy_at = 0;

    bool _pulsing = false;
    uint32_t _next_pulse_tick = 0;
    int _pulse_index = 0;
};

/**
 * @brief Sprite avatar composed from parts, driven by face.json
 *
 * Implements the same Feature interface as DefaultAvatar, so every modifier
 * (blink, speaking, breath, idle_expression, imu) drives it unchanged.
 */
class CustomAvatar : public Avatar {
public:
    bool init(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16);

    bool isValid() const
    {
        return _valid;
    }
    const std::string& faceName() const
    {
        return _manifest.name;
    }
    uitk::lvgl_cpp::Container* getPanel() const;

    /**
     * @brief Apply an emotion by its protocol name, e.g. "loving", "confused"
     *
     * This is the path that gives access to all of the assistant's emotions
     * rather than the six-value Emotion enum.
     *
     * @return false when the manifest does not describe this emotion
     */
    bool setEmotionByName(const std::string& emotion);

    /** @brief Enum path, kept for compatibility. Routes to setEmotionByName(). */
    void setEmotion(const Emotion& emotion) override;

    /** @brief Event decorators (petting, shaking) drawn from this face. */
    int addNamedDecorator(const std::string& name, uint32_t lifetimeMs = 0) override;

    /** @brief Show a pose for a moment, then go back to what was showing. Unknown names do nothing. */
    void react(const std::string& emotion, uint32_t ms);

    /** @brief Replace the current pose's twitch, until the next emotion. For trying one out. */
    void setTwitch(const PoseTwitch_t& twitch);
    /** @brief Runs the pose's own motion. Driven by PoseMotionModifier. */
    void tickMotion(uint32_t nowMs);
    uint32_t twitchCount() const
    {
        return _twitch_count;
    }

private:
    bool _valid = false;
    FaceManifest_t _manifest;
    lv_image_dsc_t _base_dsc{};

    std::unique_ptr<uitk::lvgl_cpp::Container> _pannel;
    std::unique_ptr<uitk::lvgl_cpp::Image> _base;
    std::unique_ptr<uitk::lvgl_cpp::Image> _overlay;
    std::unique_ptr<SpriteBrow> _left_brow;
    std::unique_ptr<SpriteBrow> _right_brow;
    // per-pose bases and overlays, loaded once; std::map keeps the addresses stable
    std::map<std::string, lv_image_dsc_t> _images;

    std::vector<int> _emotion_decorator_ids;
    std::string _current_emotion;

    lv_timer_t* _react_timer = nullptr;
    std::string _react_name;
    std::string _react_back;

    const EmotionPose_t* _pose = nullptr;  // std::map keeps it stable
    PoseTwitch_t _twitch;
    uint32_t _twitch_next  = 0;
    int _twitch_flips_left = 0;
    bool _twitch_shown     = false;
    uint32_t _twitch_count = 0;
    void showTwitchLid(bool shown);

    uitk::Vector2i anchorFor(const std::string& anchor) const;
    void applyDecorators(const std::vector<std::string>& names);
    void applyBackdrop(const EmotionPose_t& pose);
    const lv_image_dsc_t* cachedImage(const std::string& name);
};

}  // namespace stackchan::avatar
