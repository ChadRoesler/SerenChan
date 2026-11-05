/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <esp_heap_caps.h>
#include <cstddef>
#include <new>
#include <vector>

/**
 * @brief Allocator that places container storage in PSRAM.
 *
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is 512, so every allocation smaller than
 * that lands in internal SRAM no matter how much PSRAM is free. A sprite face
 * makes a great many small allocations -- asset tables, sprite maps, per-eye
 * lid sets -- and together they were costing more internal heap than the
 * 512 KB of sprite data they describe.
 *
 * The device idles around 16 KB free internal with a face loaded, so this is
 * the scarce pool. PSRAM has megabytes spare.
 *
 * Only for bookkeeping that is read occasionally. PSRAM is slower than
 * internal, so anything touched per-frame or from an ISR should stay put.
 */
template <typename T>
struct PsramAllocator {
    using value_type = T;

    PsramAllocator() noexcept = default;

    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept
    {
    }

    T* allocate(std::size_t count)
    {
        void* memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory == nullptr) {
            // Fall back rather than fail: a small internal allocation beats
            // losing the face entirely on a board with no PSRAM left.
            memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_8BIT);
        }
        if (memory == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(memory);
    }

    void deallocate(T* pointer, std::size_t) noexcept
    {
        heap_caps_free(pointer);
    }

    template <typename U>
    bool operator==(const PsramAllocator<U>&) const noexcept
    {
        return true;
    }

    template <typename U>
    bool operator!=(const PsramAllocator<U>&) const noexcept
    {
        return false;
    }
};

/** @brief std::vector with its storage in PSRAM. */
template <typename T>
using PsramVector = std::vector<T, PsramAllocator<T>>;
