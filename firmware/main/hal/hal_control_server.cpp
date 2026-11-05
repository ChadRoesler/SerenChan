/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
/*
 * LAN control API.
 *
 * A small REST surface over the same robot primitives hal_mcp.cpp exposes to
 * the assistant backend, so tools on your own network can drive the device
 * without going through the cloud. Modelled on
 * xiaozhi-esp32/main/boards/otto-robot/websocket_control_server.cc, but REST
 * rather than websocket because it is far easier to call from a script.
 *
 *   GET  /api/status            battery, wifi, heap, head angles, face name
 *   POST /api/head    {"yaw":0,"pitch":20,"speed":150}
 *   POST /api/led     {"r":0,"g":120,"b":80}
 *   POST /api/emotion {"name":"happy"}
 *   POST /api/say     {"text":"hello"}
 *
 * Anything touching the avatar or motion runs under LvglLockGuard: the
 * StackChan update task owns those on core 1 and will corrupt without it.
 *
 * SECURITY: this is an unauthenticated control surface on your LAN unless
 * CONFIG_STACKCHAN_CONTROL_TOKEN is set, in which case every request must
 * carry it in an X-StackChan-Token header.
 */
#include <atomic>
#include <cstdio>
#include <sdkconfig.h>
#ifdef CONFIG_STACKCHAN_CAMERA
#include "board/hal_bridge.h"
#include "board/stackchan_camera.h"
#include <jpg/image_to_jpeg.h>
#endif
#include "ram_map.h"
#include "hal.h"
#include <mooncake_log.h>
#include <sdkconfig.h>

static const std::string_view _tag = "HAL-Control";

#ifdef CONFIG_STACKCHAN_CONTROL_SERVER

#include <esp_http_server.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <board.h>
#include <display/display.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <cstring>
#include <string>

using namespace stackchan;

namespace {

httpd_handle_t _server = nullptr;

bool token_ok(httpd_req_t* req)
{
    const char* expected = CONFIG_STACKCHAN_CONTROL_TOKEN;
    if (expected == nullptr || std::strlen(expected) == 0) {
        return true;  // no token configured, open on the LAN
    }

    char got[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-StackChan-Token", got, sizeof(got)) != ESP_OK) {
        return false;
    }
    return std::strcmp(got, expected) == 0;
}

esp_err_t send_json(httpd_req_t* req, cJSON* root, const char* status = "200 OK")
{
    char* text = cJSON_PrintUnformatted(root);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(root);
    return ESP_OK;
}

// Plain text from a PSRAM buffer: cJSON's small nodes would come out of the very
// internal heap this reports on, and at 5 KB free it would fail to report at all.
esp_err_t send_text_and_free(httpd_req_t* req, char* text, size_t length)
{
    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = text ? httpd_resp_send(req, text, length) : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    heap_caps_free(text);
    return err;
}

// Head touch, for /api/touch. Atomics: the head touch task writes, the server task reads.
std::atomic<uint32_t> _touch_pats{0};
std::atomic<uint32_t> _touch_presses{0};
std::atomic<uint32_t> _touch_last_ms{0};

esp_err_t send_error(httpd_req_t* req, const char* status, const char* message)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, root, status);
}

/** @brief Read a JSON body. Caller owns the returned cJSON. */
cJSON* read_body(httpd_req_t* req)
{
    if (req->content_len <= 0 || req->content_len > 1024) {
        return nullptr;
    }

    std::string buffer(req->content_len, '\0');
    int received = httpd_req_recv(req, &buffer[0], req->content_len);
    if (received <= 0) {
        return nullptr;
    }
    buffer.resize(received);
    return cJSON_Parse(buffer.c_str());
}

int json_int(cJSON* root, const char* key, int fallback)
{
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

/* ---------------------------------- Routes --------------------------------- */

const char* reset_reason_name()
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power on";
        case ESP_RST_EXT:      return "external pin";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO:     return "sdio";
        case ESP_RST_USB:      return "usb";
        case ESP_RST_JTAG:     return "jtag";
        default:               return "unknown";
    }
}

esp_err_t handle_status(httpd_req_t* req)
{
    if (!token_ok(req)) {
        return send_error(req, "401 Unauthorized", "bad or missing token");
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "firmware", FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "uptimeMs", static_cast<double>(esp_timer_get_time() / 1000));
    cJSON_AddStringToObject(root, "resetReason", reset_reason_name());
    cJSON_AddNumberToObject(root, "batteryLevel", GetHAL().getBatteryLevel());
    cJSON_AddBoolToObject(root, "charging", GetHAL().isBatteryCharging());
    cJSON_AddNumberToObject(root, "freeInternalHeap",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    cJSON_AddNumberToObject(root, "freePsram",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    {
        LvglLockGuard lock;
        auto& stackchan = GetStackChan();
        if (stackchan.hasAvatar()) {
            cJSON_AddNumberToObject(root, "yaw", stackchan.motion().yawServo().getCurrentAngle() / 10);
            cJSON_AddNumberToObject(root, "pitch", stackchan.motion().pitchServo().getCurrentAngle() / 10);

            auto* custom = dynamic_cast<avatar::CustomAvatar*>(&stackchan.avatar());
            cJSON_AddStringToObject(root, "face", custom ? custom->faceName().c_str() : "default");
        }
    }

    return send_json(req, root);
}

esp_err_t handle_head(httpd_req_t* req)
{
    if (!token_ok(req)) {
        return send_error(req, "401 Unauthorized", "bad or missing token");
    }

    cJSON* body = read_body(req);
    if (body == nullptr) {
        return send_error(req, "400 Bad Request", "expected a JSON body");
    }

    // Same ranges hal_mcp.cpp advertises to the assistant.
    const int yaw   = json_int(body, "yaw", -9999);
    const int pitch = json_int(body, "pitch", -9999);
    const int speed = uitk::clamp(json_int(body, "speed", 150), 100, 1000);
    cJSON_Delete(body);

    {
        LvglLockGuard lock;
        auto& motion = GetStackChan().motion();
        if (pitch != -9999) {
            motion.pitchServo().moveWithSpeed(uitk::clamp(pitch, 0, 90) * 10, speed);
        }
        if (yaw != -9999) {
            motion.yawServo().moveWithSpeed(uitk::clamp(yaw, -128, 128) * 10, speed);
        }
    }

    mclog::tagInfo(_tag, "head: yaw={} pitch={} speed={}", yaw, pitch, speed);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

esp_err_t handle_led(httpd_req_t* req)
{
    if (!token_ok(req)) {
        return send_error(req, "401 Unauthorized", "bad or missing token");
    }

    cJSON* body = read_body(req);
    if (body == nullptr) {
        return send_error(req, "400 Bad Request", "expected a JSON body");
    }

    const int r = uitk::clamp(json_int(body, "r", 0), 0, 168);
    const int g = uitk::clamp(json_int(body, "g", 0), 0, 168);
    const int b = uitk::clamp(json_int(body, "b", 0), 0, 168);
    cJSON_Delete(body);

    {
        LvglLockGuard lock;
        GetStackChan().leftNeonLight().setColor(r, g, b);
        GetStackChan().rightNeonLight().setColor(r, g, b);
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

esp_err_t handle_emotion(httpd_req_t* req)
{
    if (!token_ok(req)) {
        return send_error(req, "401 Unauthorized", "bad or missing token");
    }

    cJSON* body = read_body(req);
    if (body == nullptr) {
        return send_error(req, "400 Bad Request", "expected a JSON body");
    }

    cJSON* name = cJSON_GetObjectItem(body, "name");
    if (!cJSON_IsString(name)) {
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request", "expected {\"name\": \"...\"}");
    }

    const std::string emotion = name->valuestring;
    cJSON_Delete(body);

    // Routed through the display so the sleepy side effects (servo park, idle
    // teardown) still happen, rather than poking the avatar directly.
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetEmotion(emotion.c_str());
    }

    mclog::tagInfo(_tag, "emotion: {}", emotion);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "emotion", emotion.c_str());
    return send_json(req, root);
}

esp_err_t handle_say(httpd_req_t* req)
{
    if (!token_ok(req)) {
        return send_error(req, "401 Unauthorized", "bad or missing token");
    }

    cJSON* body = read_body(req);
    if (body == nullptr) {
        return send_error(req, "400 Bad Request", "expected a JSON body");
    }

    cJSON* text = cJSON_GetObjectItem(body, "text");
    if (!cJSON_IsString(text)) {
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request", "expected {\"text\": \"...\"}");
    }

    const std::string message = text->valuestring;
    cJSON_Delete(body);

    {
        LvglLockGuard lock;
        auto& stackchan = GetStackChan();
        if (stackchan.hasAvatar()) {
            stackchan.avatar().setSpeech(message);
        }
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

}  // namespace

namespace {

void start_httpd();

/** @brief WiFi can come up via mooncake's setup app or via xiaozhi's
 *  WifiBoard. Binding to the IP event covers both. */
void on_got_ip(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;
    (void)base;
    (void)id;
    auto* event = static_cast<ip_event_got_ip_t*>(data);
    mclog::tagInfo(_tag, "got ip " IPSTR ", starting control server", IP2STR(&event->ip_info.ip));
    start_httpd();
}

void start_httpd()
{
    if (_server != nullptr) {
        return;
    }

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = CONFIG_STACKCHAN_CONTROL_PORT;
    config.ctrl_port        = CONFIG_STACKCHAN_CONTROL_PORT + 1;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
#ifdef CONFIG_STACKCHAN_CAMERA
    config.stack_size = 8192;  // the JPEG encoder runs on this task
#else
    config.stack_size = 5120;
#endif

    if (httpd_start(&_server, &config) != ESP_OK) {
        mclog::tagError(_tag, "failed to start control server");
        _server = nullptr;
        return;
    }

    static const auto handle_ram = [](httpd_req_t* req) -> esp_err_t {
        if (!token_ok(req)) {
            return send_error(req, "401 Unauthorized", "bad or missing token");
        }
        size_t length = 0;
        char* text    = ram_map::report(length);
        return send_text_and_free(req, text, length);
    };
    static const auto handle_log = [](httpd_req_t* req) -> esp_err_t {
        if (!token_ok(req)) {
            return send_error(req, "401 Unauthorized", "bad or missing token");
        }
        size_t length = 0;
        char* text    = ram_map::recent_log(length);
        return send_text_and_free(req, text, length);
    };

#ifdef CONFIG_STACKCHAN_CAMERA
    // The first frames after a pause are dark while the sensor settles, so a few go by.
    static const auto handle_photo = [](httpd_req_t* req) -> esp_err_t {
        if (!token_ok(req)) {
            return send_error(req, "401 Unauthorized", "bad or missing token");
        }
        auto* camera = hal_bridge::board_get_camera();
        if (camera == nullptr) {
            return send_error(req, "503 Service Unavailable", "no camera");
        }
        auto lock = camera->TryLockFrame();
        if (!lock.owns_lock()) {
            return send_error(req, "503 Service Unavailable", "camera busy");
        }
        bool captured = false;
        for (int i = 0; i < 4; i++) {
            captured = camera->StreamCaptures();
        }
        if (!captured) {
            return send_error(req, "500 Internal Server Error", "capture failed");
        }
        uint8_t* jpeg = nullptr;
        size_t size   = 0;
        bool encoded  = image_to_jpeg(const_cast<uint8_t*>(camera->GetFrameData()), camera->GetFrameSize(),
                                      camera->GetFrameWidth(), camera->GetFrameHeight(),
                                      (v4l2_pix_fmt_t)camera->GetFrameFormat(), 60, &jpeg, &size);
        if (!encoded || jpeg == nullptr) {
            free(jpeg);
            return send_error(req, "500 Internal Server Error", "jpeg failed");
        }
        httpd_resp_set_type(req, "image/jpeg");
        esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char*>(jpeg), size);
        free(jpeg);
        return err;
    };
#endif

    static const auto handle_touch = [](httpd_req_t* req) -> esp_err_t {
        if (!token_ok(req)) {
            return send_error(req, "401 Unauthorized", "bad or missing token");
        }
        char text[96];
        const uint32_t last = _touch_last_ms.load();
        const uint32_t ago  = last ? GetHAL().millis() - last : 0;
        int n = std::snprintf(text, sizeof(text), "{\"ok\":true,\"pats\":%u,\"presses\":%u,\"ms_ago\":%u}",
                              (unsigned)_touch_pats.load(), (unsigned)_touch_presses.load(), (unsigned)ago);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, text, n);
    };

    static const httpd_uri_t routes[] = {
        {"/api/touch", HTTP_GET, handle_touch, nullptr},
#ifdef CONFIG_STACKCHAN_CAMERA
        {"/api/photo", HTTP_GET, handle_photo, nullptr},
#endif
        {"/api/status", HTTP_GET, handle_status, nullptr},
        {"/api/ram", HTTP_GET, handle_ram, nullptr},
        {"/api/log", HTTP_GET, handle_log, nullptr},
        {"/api/head", HTTP_POST, handle_head, nullptr},
        {"/api/led", HTTP_POST, handle_led, nullptr},
        {"/api/emotion", HTTP_POST, handle_emotion, nullptr},
        {"/api/say", HTTP_POST, handle_say, nullptr},
    };
    for (const auto& route : routes) {
        httpd_register_uri_handler(_server, &route);
    }

    const char* token = CONFIG_STACKCHAN_CONTROL_TOKEN;
    mclog::tagInfo(_tag, "control server on port {} ({})", CONFIG_STACKCHAN_CONTROL_PORT,
                   (token && std::strlen(token)) ? "token required" : "NO AUTH - LAN open");
}

}  // namespace

void Hal::startControlServer()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    onHeadPetGesture.connect([this](HeadPetGesture gesture) {
        if (gesture == HeadPetGesture::SwipeForward || gesture == HeadPetGesture::SwipeBackward) {
            _touch_pats++;
            _touch_last_ms = millis();
        } else if (gesture == HeadPetGesture::Press) {
            _touch_presses++;
            _touch_last_ms = millis();
        }
    });

    // The default loop may not exist this early. Creating it is idempotent --
    // ESP_ERR_INVALID_STATE just means somebody got there first.
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        mclog::tagError(_tag, "esp_event_loop_create_default failed: {}", esp_err_to_name(err));
        return;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, nullptr, nullptr);
    if (err != ESP_OK) {
        mclog::tagError(_tag, "could not bind IP_EVENT_STA_GOT_IP: {}", esp_err_to_name(err));
        return;
    }
    mclog::tagInfo(_tag, "waiting for an IP to start the control server");

    // Already online (a warm reboot, or this ran late) -- do not wait.
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        start_httpd();
    }
}

void Hal::stopControlServer()
{
    if (_server != nullptr) {
        httpd_stop(_server);
        _server = nullptr;
        mclog::tagInfo(_tag, "control server stopped");
    }
}

#else /* CONFIG_STACKCHAN_CONTROL_SERVER */

void Hal::startControlServer()
{
}

void Hal::stopControlServer()
{
}

#endif
