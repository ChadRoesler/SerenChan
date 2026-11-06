/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>
#include <cstdint>

/**
 * @brief A new face sent over the network, without touching the microSD card.
 *
 * The card can't be written while the display runs (GPIO 35 is both its MISO and
 * the LCD's D/C line, see hal_sdcard.cpp). So an upload waits in the unused ota_1
 * flash partition, and the next boot copies it onto the card before the display
 * claims the bus. The card stays the one home of a face.
 *
 * Nothing can leave the device faceless: the waiting mark is written only after
 * the checksum passes, the files go to face_new first, and the old face stays as
 * face_old (a boot that finds face missing puts face_old back).
 *
 * Bundle: repeated [u8 name length][name][u32 size, little endian][bytes].
 */
namespace face_push {

/** @brief Start an upload of this many bytes: erases the slot, dropping anything waiting. */
bool begin(size_t length);
bool write(const void* data, size_t length);
/** @brief Check the CRC-32 of everything written, and mark it waiting for the next boot. */
bool commit(uint32_t crc);

/** @brief On the next boot, swap face and face_old. */
bool request_restore();

/** @brief Boot only, with the card mounted and before its face is read. */
void apply_to_card();
/** @brief Boot only, when no card mounted: a waiting face stays waiting. */
void note_no_card();

/** @brief What the last boot did with a waiting face; empty if there was none. */
const char* last_result();

}  // namespace face_push
