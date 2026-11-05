/*
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>

/**
 * @brief Where the internal RAM went, read over the control API.
 *
 * Internal SRAM is what runs out on this board while PSRAM sits megabytes free,
 * so every cut has to be measured against it. mark() records the heap at a boot
 * stage; the control server hands the stages, the live tasks and the recent log
 * back as plain text. Everything here lives in PSRAM, so looking costs nothing
 * in the pool being looked at.
 */
namespace ram_map {

/** Keep a copy of the ESP log in a PSRAM ring. Call once, as early as possible. */
void start_log_capture();

/** Record internal heap now under this stage name (a string literal). */
void mark(const char* stage);

/** The stages and the live tasks as text. Returns a PSRAM buffer to heap_caps_free. */
char* report(size_t& length);

/** The captured log, oldest first. Returns a PSRAM buffer to heap_caps_free. */
char* recent_log(size_t& length);

}  // namespace ram_map
