/*
 * Flash Save Implementation
 */

#include "save.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/mutex.h"
#include <string.h>

namespace Chuni245Tof {

// Flash sector for config storage (last sector of first 256KB)
#define FLASH_TARGET_OFFSET (256 * 1024 - FLASH_SECTOR_SIZE)
#define FLASH_ADDR (XIP_BASE + FLASH_TARGET_OFFSET)

static mutex_t* save_mutex = nullptr;
static uint32_t save_magic = 0;
static bool save_initialized = false;

void save_init(uint32_t magic, mutex_t* mutex) {
    save_magic = magic;
    save_mutex = mutex;
    save_initialized = true;
}

bool save_load(void* data, uint32_t len) {
    if (!save_initialized || !data) {
        return false;
    }

    const uint8_t* flash_data = (const uint8_t*)FLASH_ADDR;

    // Check magic number
    uint32_t stored_magic;
    memcpy(&stored_magic, flash_data, sizeof(stored_magic));

    if (stored_magic != save_magic) {
        return false;
    }

    // Copy data from flash
    memcpy(data, flash_data + sizeof(stored_magic), len);

    return true;
}

bool save_write(const void* data, uint32_t len) {
    if (!save_initialized || !data) {
        return false;
    }

    if (save_mutex) {
        mutex_enter_blocking(save_mutex);
    }

    // Prepare buffer with magic number
    uint8_t buffer[FLASH_PAGE_SIZE];
    uint32_t total_len = sizeof(save_magic) + len;

    if (total_len > FLASH_PAGE_SIZE) {
        if (save_mutex) mutex_exit(save_mutex);
        return false;
    }

    memcpy(buffer, &save_magic, sizeof(save_magic));
    memcpy(buffer + sizeof(save_magic), data, len);

    // Erase and write flash
    uint32_t ints = save_and_disable_interrupts();

    // Erase sector
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);

    // Program data
    flash_range_program(FLASH_TARGET_OFFSET, buffer, FLASH_PAGE_SIZE);

    restore_interrupts(ints);

    if (save_mutex) {
        mutex_exit(save_mutex);
    }

    return true;
}

void save_loop() {
    // Handle deferred save operations if needed
    // For now, saves happen synchronously
}

} // namespace Chuni245Tof