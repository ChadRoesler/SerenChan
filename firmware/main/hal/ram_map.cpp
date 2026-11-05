/*
 * SPDX-License-Identifier: MIT
 */
#include "ram_map.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr size_t kMaxStages  = 48;
constexpr size_t kLogSize    = 32 * 1024;
constexpr size_t kReportSize = 12 * 1024;
constexpr int kMaxTasks      = 48;

struct Stage_t {
    const char* name;
    uint32_t ms;
    uint32_t free_internal;
    uint32_t largest_internal;
    uint32_t min_internal;
};

Stage_t* _stages = nullptr;
size_t _stage_count = 0;

char* _log          = nullptr;
size_t _log_head    = 0;
bool _log_wrapped   = false;
vprintf_like_t _previous_vprintf = nullptr;
portMUX_TYPE _log_lock = portMUX_INITIALIZER_UNLOCKED;

void* psram(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

int capture_vprintf(const char* format, va_list args)
{
    // A small stack buffer: logging runs on whichever task logged, and some of
    // those stacks are tight. A long line is cut here and whole on the console.
    char line[192];
    va_list copy;
    va_copy(copy, args);
    int written = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);

    if (written > 0 && _log != nullptr) {
        size_t n = written < static_cast<int>(sizeof(line)) ? written : sizeof(line) - 1;
        portENTER_CRITICAL(&_log_lock);
        for (size_t i = 0; i < n; i++) {
            _log[_log_head++] = line[i];
            if (_log_head == kLogSize) {
                _log_head    = 0;
                _log_wrapped = true;
            }
        }
        portEXIT_CRITICAL(&_log_lock);
    }
    return _previous_vprintf ? _previous_vprintf(format, args) : vprintf(format, args);
}

}  // namespace

void ram_map::start_log_capture()
{
    if (_log != nullptr) {
        return;
    }
    _log = static_cast<char*>(psram(kLogSize));
    if (_log != nullptr) {
        _previous_vprintf = esp_log_set_vprintf(capture_vprintf);
    }
}

void ram_map::mark(const char* stage)
{
    if (_stages == nullptr) {
        _stages = static_cast<Stage_t*>(psram(sizeof(Stage_t) * kMaxStages));
    }
    if (_stages == nullptr || _stage_count >= kMaxStages) {
        return;
    }
    _stages[_stage_count++] = {
        stage,
        static_cast<uint32_t>(esp_timer_get_time() / 1000),
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
    };
}

char* ram_map::report(size_t& length)
{
    length     = 0;
    char* text = static_cast<char*>(psram(kReportSize));
    if (text == nullptr) {
        return nullptr;
    }
    size_t at = 0;
    auto add  = [&](const char* format, auto... values) {
        if (at < kReportSize) {
            int n = std::snprintf(text + at, kReportSize - at, format, values...);
            if (n > 0) {
                at += static_cast<size_t>(n) < kReportSize - at ? n : kReportSize - at - 1;
            }
        }
    };

    add("now: free %u largest %u min %u psram %u\n\nstage                          ms    free   delta largest     min\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    for (size_t i = 0; i < _stage_count; i++) {
        const auto& s = _stages[i];
        int delta     = i ? static_cast<int>(s.free_internal) - static_cast<int>(_stages[i - 1].free_internal) : 0;
        add("%-26s %6u %7u %7d %7u %7u\n", s.name, (unsigned)s.ms, (unsigned)s.free_internal, delta,
            (unsigned)s.largest_internal, (unsigned)s.min_internal);
    }

    auto* tasks = static_cast<TaskStatus_t*>(psram(sizeof(TaskStatus_t) * kMaxTasks));
    if (tasks != nullptr) {
        UBaseType_t count = uxTaskGetSystemState(tasks, kMaxTasks, nullptr);
        add("\n%u tasks: name, stack memory, unused stack bytes, priority, core\n", (unsigned)count);
        for (UBaseType_t i = 0; i < count; i++) {
            add("%-18s %-8s %6u %3u %d\n", tasks[i].pcTaskName,
                esp_ptr_internal(tasks[i].pxStackBase) ? "internal" : "psram",
                (unsigned)tasks[i].usStackHighWaterMark, (unsigned)tasks[i].uxCurrentPriority,
                tasks[i].xCoreID == tskNO_AFFINITY ? -1 : (int)tasks[i].xCoreID);
        }
        heap_caps_free(tasks);
    }
    length = at;
    return text;
}

char* ram_map::recent_log(size_t& length)
{
    length = 0;
    if (_log == nullptr) {
        return nullptr;
    }
    char* text = static_cast<char*>(psram(kLogSize));
    if (text == nullptr) {
        return nullptr;
    }
    portENTER_CRITICAL(&_log_lock);
    if (_log_wrapped) {
        std::memcpy(text, _log + _log_head, kLogSize - _log_head);
        std::memcpy(text + (kLogSize - _log_head), _log, _log_head);
        length = kLogSize;
    } else {
        std::memcpy(text, _log, _log_head);
        length = _log_head;
    }
    portEXIT_CRITICAL(&_log_lock);
    return text;
}
