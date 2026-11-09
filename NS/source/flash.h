/*
GBA Multi Game Menu
Author: Lesserkuma (github.com/lesserkuma), ZaindORp (github.com/orzgithub)
*/

#ifndef FLASH_H_
#define FLASH_H_

#include "main.h"

#define _FLASH_WRITE(pa, pd)                     \
    {                                            \
        *(((vu16 *)AGB_ROM) + ((pa) >> 1)) = pd; \
        __asm("nop");                            \
    }

#define MAGIC_FLASH_STATUS 0x414D554B
#define MAGIC_MENU_SETTINGS 0x53

typedef enum BootGameError_
{
    BOOT_GAME_OK = 0,
    BOOT_GAME_ERR_UNSUPPORTED_CART = 1,
    BOOT_GAME_ERR_MAPPER_NO_RESPONSE = 2,
    BOOT_GAME_ERR_WL_METADATA = 3,
    BOOT_GAME_ERR_WL_RECORD = 4,
    BOOT_GAME_ERR_LAUNCH_FAILED = 5,
    BOOT_GAME_ERR_IMAGE_METADATA = 6,
    BOOT_GAME_ERR_ENTRY_RANGE = 7,
} BootGameError;

typedef struct __attribute__((packed)) FlashStatus_
{
    u32 magic;
    u8 version;
    u8 battery_present;
    u16 last_boot_menu_index;
    u8 last_boot_save_index;
    SAVE_TYPE last_boot_save_type;
    u8 sram_bank_type;
    u8 settings_magic;
    u8 settings_flags;
} FlashStatus;

void CartridgeInitialize(void);
BOOL CartridgeCanLaunch(ItemConfig config, u16 entry_slot);
const CartridgeProfile *CartridgeGetProfile(void);
IWRAM_CODE void FlashDetectType(void);
IWRAM_CODE void FlashEraseSector(u32 address);
IWRAM_CODE void FlashWriteData(u32 address, u32 length);
BOOL ReadWlRomMapRecord(u16 entry_slot, WlRomMapRecord *record);
BOOL ReadWlBucketOwners(u16 *owners, u16 capacity, u16 *bucket_count);
BOOL ReadCartridgeBucketOwners(u16 *owners, u16 capacity, u16 *bucket_count);
IWRAM_CODE void FlashStoreStatus(FlashStatus status);
IWRAM_CODE u8 BootGame(ItemConfig config, FlashStatus status, u16 entry_slot);

#endif
