/*
GBA Multi Game Menu
Author: Lesserkuma (github.com/lesserkuma), ZaindORp (github.com/orzgithub)
*/

#include <gba.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "flash.h"

u8 flash_type;
u8 sram_bank_type;
u8 *itemlist;
u16 itemlist_offset;
u32 flash_sector_size;
u32 flash_status_sector_offset;
u32 flash_capacity_bytes;
CartridgeCapacitySource flash_capacity_source;
CartridgeProfile sCartridgeProfile;
static u32 sDetectedNorId;
static u16 sDetectedNorId2;
static u16 sDetectedNorId3;
static u32 sDetectedBufferBytes;
EWRAM_BSS u8 sram_register_backup[7];
EWRAM_BSS u8 data_buffer[SRAM_SIZE * 2];

static void FlashCalcOffsets(void);
static BOOL IsWlMappingHeaderStructureValid(const WlMappingHeader *header);
static BOOL IsWlMappingHeaderValid(const WlMappingHeader *header);
static u16 CountItemListEntries(void);

IWRAM_CODE static u32 FlashProbeCfiCapacity(void)
{
	u8 signature0;
	u8 signature1;
	u8 signature2;
	u8 exponent;
	u8 buffer_low;
	u8 buffer_high;
	BOOL swapped_data_bits = FALSE;

	_FLASH_WRITE(0, 0xF0);
	_FLASH_WRITE(0xAA, 0x98);
	signature0 = *(vu16 *)(AGB_ROM + 0x20) & 0xFF;
	signature1 = *(vu16 *)(AGB_ROM + 0x22) & 0xFF;
	signature2 = *(vu16 *)(AGB_ROM + 0x24) & 0xFF;
	exponent = *(vu16 *)(AGB_ROM + 0x4E) & 0xFF;
	buffer_low = *(vu16 *)(AGB_ROM + 0x54) & 0xFF;
	buffer_high = *(vu16 *)(AGB_ROM + 0x56) & 0xFF;
	_FLASH_WRITE(0, 0xF0);

	if (signature0 == 'R' && signature1 == 'Q' && signature2 == 'Z')
		swapped_data_bits = TRUE;
	else if (signature0 != 'Q' || signature1 != 'R' || signature2 != 'Y')
		return 0;

	if (swapped_data_bits) {
		exponent = (exponent & ~3U) | ((exponent & 1) << 1) | ((exponent & 2) >> 1);
		buffer_low = (buffer_low & ~3U) | ((buffer_low & 1) << 1) | ((buffer_low & 2) >> 1);
		buffer_high = (buffer_high & ~3U) | ((buffer_high & 1) << 1) | ((buffer_high & 2) >> 1);
	}
	if (exponent < 20 || exponent > 29)
		return 0;
	u16 buffer_exponent = buffer_low | ((u16)buffer_high << 8);
	if (buffer_exponent > 1 && buffer_exponent < 16)
		sDetectedBufferBytes = 1U << buffer_exponent;
	return 1U << exponent;
}

static const WlMappingHeader *GetWlMappingHeader(void)
{
	return (const WlMappingHeader *)((const u8 *)AGB_ROM + WL_MAPPING_OFFSET);
}

static u32 ComputeChecksum32(const u8 *data, u32 length, u32 checksum_offset)
{
	u32 hash = 2166136261u;
	for (u32 i = 0; i < length; i++)
	{
		u8 value = (i >= checksum_offset && i < checksum_offset + sizeof(u32)) ? 0 : data[i];
		hash ^= value;
		hash *= 16777619u;
	}
	return hash;
}

static BOOL ReadImageManifest(ImageManifest *manifest)
{
	const u8 *base = (const u8 *)AGB_ROM + IMAGE_MANIFEST_OFFSET;
	u32 save_area_end;
	memcpy(manifest, base, sizeof(*manifest));
	if (manifest->magic != IMAGE_MANIFEST_MAGIC ||
		manifest->version != IMAGE_MANIFEST_VERSION ||
		manifest->size != sizeof(*manifest) ||
		manifest->flags != (IMAGE_MANIFEST_FLAG_LEGACY | IMAGE_MANIFEST_FLAG_WL) ||
		manifest->itemlist_offset != IMAGE_ITEMLIST_OFFSET ||
		manifest->status_offset != IMAGE_STATUS_OFFSET ||
		manifest->save_area_offset != IMAGE_SAVE_AREA_OFFSET ||
		manifest->mapping_offset != WL_MAPPING_OFFSET ||
		manifest->mapping_size != WL_MAPPING_SECTOR_SIZE ||
		manifest->item_config_size != ITEM_CONFIG_SIZE)
		return FALSE;
	if (manifest->entry_count > WL_ROM_MAP_RECORD_COUNT ||
		manifest->itemlist_size != (u32)manifest->entry_count * ITEM_CONFIG_SIZE ||
		manifest->itemlist_size > 0xE000 ||
		manifest->status_size < 0x1000 || manifest->status_size > 0x40000 ||
		(manifest->save_slot_stride != 0x20000 && manifest->save_slot_stride != 0x40000) ||
		manifest->wl_save_slot_count != WL_SAVE_SLOT_COUNT ||
		manifest->entry_count > manifest->wl_save_slot_count)
		return FALSE;
	save_area_end = manifest->save_area_offset + (u32)manifest->save_slot_count * manifest->save_slot_stride;
	if (save_area_end < manifest->save_area_offset || save_area_end > manifest->mapping_offset)
		return FALSE;
	return ComputeChecksum32(base, manifest->size, manifest->size - sizeof(u32)) == manifest->checksum;
}

void CartridgeInitialize(void)
{
	const ImageManifest *raw_manifest = (const ImageManifest *)((const u8 *)AGB_ROM + IMAGE_MANIFEST_OFFSET);
	const WlMappingHeader *wl_header = GetWlMappingHeader();
	BOOL manifest_present = raw_manifest->magic == IMAGE_MANIFEST_MAGIC;
	BOOL wl_mapping_present = wl_header->magic == WLMI_MAGIC;

	memset(&sCartridgeProfile, 0, sizeof(sCartridgeProfile));
	sCartridgeProfile.manifest_valid = ReadImageManifest(&sCartridgeProfile.manifest);
	FlashDetectType();
	sCartridgeProfile.flash_type = flash_type;
	sCartridgeProfile.flash_sector_size = flash_sector_size;
	sCartridgeProfile.wl_mapping_valid = IsWlMappingHeaderValid(wl_header);
	sCartridgeProfile.device_capacity_bytes = flash_capacity_bytes;
	sCartridgeProfile.capacity_source = flash_capacity_source;
	sCartridgeProfile.nor_id = sDetectedNorId;
	sCartridgeProfile.nor_device_id2 = sDetectedNorId2;
	sCartridgeProfile.nor_device_id3 = sDetectedNorId3;
	sCartridgeProfile.nor_buffer_bytes = sDetectedBufferBytes;
	if (sCartridgeProfile.wl_mapping_valid) sCartridgeProfile.mapping_flags = wl_header->flags;
	sCartridgeProfile.probe_capacity_bytes = flash_capacity_bytes;
	sCartridgeProfile.probe_capacity_source = flash_capacity_source;

	if (flash_type == 0)
	{
		sCartridgeProfile.backend = CART_BACKEND_NONE;
	}
	else if (manifest_present)
	{
		if (!sCartridgeProfile.manifest_valid || !sCartridgeProfile.wl_mapping_valid)
		{
			sCartridgeProfile.backend = CART_BACKEND_NONE;
			sCartridgeProfile.metadata_error = TRUE;
		}
		else
		{
			sCartridgeProfile.backend = CART_BACKEND_UNIVERSAL;
			sCartridgeProfile.itemlist_offset = sCartridgeProfile.manifest.itemlist_offset;
			sCartridgeProfile.status_offset = sCartridgeProfile.manifest.status_offset;
			sCartridgeProfile.save_area_offset = sCartridgeProfile.manifest.save_area_offset;
			sCartridgeProfile.save_slot_stride = sCartridgeProfile.manifest.save_slot_stride;
			sCartridgeProfile.entry_count = sCartridgeProfile.manifest.entry_count;
			sCartridgeProfile.save_slot_count = sCartridgeProfile.manifest.save_slot_count;
			sCartridgeProfile.device_capacity_bytes = (u32)wl_header->bucket_owner_count * WL_BUCKET_SIZE;
			sCartridgeProfile.capacity_source = CART_CAPACITY_MAPPING;
		}
	}
	else if (wl_mapping_present)
	{
		if (!sCartridgeProfile.wl_mapping_valid || flash_sector_size != WL_MAPPING_SECTOR_SIZE)
		{
			sCartridgeProfile.backend = CART_BACKEND_NONE;
			sCartridgeProfile.metadata_error = TRUE;
		}
		else
		{
			sCartridgeProfile.backend = CART_BACKEND_WL;
			sCartridgeProfile.itemlist_offset = WL_ITEMLIST_OFFSET;
			sCartridgeProfile.status_offset = WL_STATUS_OFFSET;
			sCartridgeProfile.save_area_offset = WL_SAVE_AREA_OFFSET;
			sCartridgeProfile.save_slot_stride = flash_sector_size;
			sCartridgeProfile.save_slot_count = WL_SAVE_SLOT_COUNT;
			sCartridgeProfile.device_capacity_bytes = (u32)wl_header->bucket_owner_count * WL_BUCKET_SIZE;
			sCartridgeProfile.capacity_source = CART_CAPACITY_MAPPING;
		}
	}
	else
	{
		u32 own_size = (u32)(&__rom_end__) - (u32)AGB_ROM;
		u32 itemlist_address = own_size + (0x40000 - (own_size % 0x40000));
		sCartridgeProfile.backend = CART_BACKEND_LEGACY;
		sCartridgeProfile.itemlist_offset = itemlist_address;
		sCartridgeProfile.status_offset = itemlist_address + flash_sector_size;
		sCartridgeProfile.save_area_offset = itemlist_address + (2 * flash_sector_size);
		sCartridgeProfile.save_slot_stride = flash_sector_size;
		sCartridgeProfile.save_slot_count = 0x100;
	}
	sCartridgeProfile.initialized = TRUE;
	FlashCalcOffsets();
	if (sCartridgeProfile.backend == CART_BACKEND_LEGACY || sCartridgeProfile.backend == CART_BACKEND_WL)
		sCartridgeProfile.entry_count = CountItemListEntries();
}

const CartridgeProfile *CartridgeGetProfile(void)
{
	return &sCartridgeProfile;
}

BOOL CartridgeCanLaunch(ItemConfig config, u16 entry_slot)
{
	WlRomMapRecord wl_record;
	if (!sCartridgeProfile.initialized || sCartridgeProfile.backend == CART_BACKEND_NONE ||
		sCartridgeProfile.flash_type == 0 || entry_slot >= sCartridgeProfile.entry_count)
		return FALSE;
	if (config.rom_size == 0 || config.rom_size > 0x40)
		return FALSE;
	if (config.save_type != SRAM_NONE && config.save_index >= sCartridgeProfile.save_slot_count)
		return FALSE;
	if (sCartridgeProfile.backend == CART_BACKEND_WL || sCartridgeProfile.backend == CART_BACKEND_UNIVERSAL)
		return ReadWlRomMapRecord(entry_slot, &wl_record);
	return TRUE;
}

IWRAM_CODE void FlashStoreStatus(FlashStatus status)
{
	memset(data_buffer, 0, 0x1000);
	memcpy(data_buffer, &status, sizeof(status));
	FlashEraseSector(sCartridgeProfile.status_offset);
	FlashWriteData(sCartridgeProfile.status_offset, 0x1000);
}

static const u32 sWlCrc32NibbleTable[16] =
{
	0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
	0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
	0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
	0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
};

static u32 ComputeWlMappingChecksum(const u8 *sector_base)
{
	u32 crc = 0xFFFFFFFF;

	for (u32 i = 0; i < WL_MAPPING_SECTOR_SIZE; i++)
	{
		u8 value = sector_base[i];
		if (i >= WL_MAPPING_CHECKSUM_OFFSET && i < WL_MAPPING_CHECKSUM_OFFSET + sizeof(u32))
			value = 0;

		crc ^= value;
		crc = (crc >> 4) ^ sWlCrc32NibbleTable[crc & 0x0F];
		crc = (crc >> 4) ^ sWlCrc32NibbleTable[crc & 0x0F];
	}

	return ~crc;
}

static BOOL IsWlMappingHeaderStructureValid(const WlMappingHeader *header)
{
	u32 bucket_owner_end;
	u32 record_table_end;

	if (header->magic != WLMI_MAGIC)
		return FALSE;
	if (header->version != WLMI_VERSION_CURRENT)
		return FALSE;
	if (header->header_size < sizeof(WlMappingHeader))
		return FALSE;
	if (header->bucket_owner_entry_size != sizeof(u16))
		return FALSE;
	if (header->bucket_owner_count < WL_MIN_BUCKET_COUNT || header->bucket_owner_count > WL_BUCKET_COUNT)
		return FALSE;
	if (header->rom_map_record_size != sizeof(WlRomMapRecord))
		return FALSE;
	if (header->rom_map_record_count < WL_ROM_MAP_RECORD_COUNT)
		return FALSE;
	if (header->bucket_owner_offset < header->header_size)
		return FALSE;
	bucket_owner_end = header->bucket_owner_offset + ((u32)header->bucket_owner_entry_size * header->bucket_owner_count);
	if (bucket_owner_end > WL_MAPPING_SECTOR_SIZE)
		return FALSE;
	if (header->rom_map_record_offset < header->header_size)
		return FALSE;
	if (header->rom_map_record_offset < header->bucket_owner_offset)
		return FALSE;

	record_table_end = header->rom_map_record_offset + ((u32)header->rom_map_record_size * header->rom_map_record_count);
	if (record_table_end > WL_MAPPING_SECTOR_SIZE)
		return FALSE;

	return TRUE;
}

static BOOL IsWlMappingHeaderValid(const WlMappingHeader *header)
{
	const u8 *mapping_base;
	if (!IsWlMappingHeaderStructureValid(header)) return FALSE;
	mapping_base = (const u8 *)AGB_ROM + WL_MAPPING_OFFSET;
	return ComputeWlMappingChecksum(mapping_base) == header->mapping_checksum;
}

static BOOL IsWlRomMapRecordValid(const WlRomMapRecord *record, u16 bucket_owner_count)
{
	if (record->logical_bucket_count == 0 || record->logical_bucket_count > WL_MAX_MAPPED_BUCKETS)
		return FALSE;

	for (u8 i = 0; i < record->logical_bucket_count; i++)
	{
		if (record->phys_block_id[i] == 0 || record->phys_block_id[i] >= bucket_owner_count)
			return FALSE;
	}

	return TRUE;
}

static u8 ReadWlRomMapRecordInternal(u16 entry_slot, WlRomMapRecord *record)
{
	const WlMappingHeader *header;
	const u8 *record_base;
	u32 record_offset;

	if (record == NULL || entry_slot >= WL_ROM_MAP_RECORD_COUNT)
		return BOOT_GAME_ERR_WL_RECORD;

	header = GetWlMappingHeader();
	if (!sCartridgeProfile.wl_mapping_valid || !IsWlMappingHeaderStructureValid(header))
		return BOOT_GAME_ERR_WL_METADATA;

	record_offset = header->rom_map_record_offset + ((u32)entry_slot * header->rom_map_record_size);
	if ((record_offset + sizeof(WlRomMapRecord)) > WL_MAPPING_SECTOR_SIZE)
		return BOOT_GAME_ERR_WL_METADATA;

	record_base = (const u8 *)AGB_ROM + WL_MAPPING_OFFSET + record_offset;
	memcpy(record, record_base, sizeof(WlRomMapRecord));
	if (!IsWlRomMapRecordValid(record, header->bucket_owner_count))
		return BOOT_GAME_ERR_WL_RECORD;

	return BOOT_GAME_OK;
}

BOOL ReadWlRomMapRecord(u16 entry_slot, WlRomMapRecord *record)
{
	return ReadWlRomMapRecordInternal(entry_slot, record) == BOOT_GAME_OK;
}

BOOL ReadWlBucketOwners(u16 *owners, u16 capacity, u16 *bucket_count)
{
	const WlMappingHeader *header = GetWlMappingHeader();
	const u8 *owner_base;

	if (bucket_count != NULL) *bucket_count = 0;
	if (owners == NULL || bucket_count == NULL || !sCartridgeProfile.wl_mapping_valid ||
		!IsWlMappingHeaderStructureValid(header) ||
		capacity < header->bucket_owner_count)
		return FALSE;

	owner_base = (const u8 *)AGB_ROM + WL_MAPPING_OFFSET + header->bucket_owner_offset;
	memcpy(owners, owner_base, (u32)header->bucket_owner_count * sizeof(u16));
	*bucket_count = header->bucket_owner_count;
	return TRUE;
}

BOOL ReadCartridgeBucketOwners(u16 *owners, u16 capacity, u16 *bucket_count)
{
	if (bucket_count != NULL) *bucket_count = 0;
	if (owners == NULL || bucket_count == NULL)
		return FALSE;
	if (sCartridgeProfile.backend == CART_BACKEND_WL ||
		sCartridgeProfile.backend == CART_BACKEND_UNIVERSAL)
		return ReadWlBucketOwners(owners, capacity, bucket_count);
	if (sCartridgeProfile.backend != CART_BACKEND_LEGACY ||
		sCartridgeProfile.device_capacity_bytes < WL_BUCKET_SIZE)
		return FALSE;

	u16 count = sCartridgeProfile.device_capacity_bytes / WL_BUCKET_SIZE;
	if (count > capacity) count = capacity;
	if (count == 0) return FALSE;
	for (u16 bucket = 0; bucket < count; bucket++) owners[bucket] = 0xFFFF;
	owners[0] = 0xFFFE;

	for (u16 entry_slot = 0; entry_slot < sCartridgeProfile.entry_count; entry_slot++)
	{
		ItemConfig config;
		memcpy(&config, itemlist + (u32)entry_slot * ITEM_CONFIG_SIZE, sizeof(config));
		u16 first_bucket = config.rom_offset / 8;
		u16 game_bucket_count = (config.rom_size + 7) / 8;
		if (game_bucket_count == 0) continue;
		for (u16 offset = 0; offset < game_bucket_count; offset++)
		{
			u16 bucket = first_bucket + offset;
			if (bucket < count && owners[bucket] == 0xFFFF) owners[bucket] = entry_slot;
		}
	}

	*bucket_count = count;
	return TRUE;
}

IWRAM_CODE static BOOL WlTryLaunchMap(const WlRomMapRecord *record)
{
	*(vu8 *)WL_MAPPER_CONTROL = WL_CMD_ENTER_STAGE1;
	*(vu8 *)WL_MAPPER_CONTROL = WL_CMD_ENTER_STAGE2;
	*(vu8 *)WL_MAPPER_DATA = record->logical_bucket_count;
	for (u8 i = 0; i < record->logical_bucket_count; i++)
	{
		*(vu8 *)WL_MAPPER_DATA = record->phys_block_id[i];
	}
	*(vu8 *)WL_MAPPER_CONTROL = WL_CMD_COMMIT;

	u32 timeout = 0x2FFF;
	while (((vu16 *)AGB_ROM)[0x58] == 0x4B4C)
	{
		if (!timeout--) return FALSE;
	}
	return TRUE;
}

IWRAM_CODE static void WlSendSaveSlotIndex(u8 save_slot_index)
{
	// Sideband launch metadata for the main-controller save-slot path.
	// This stays off the existing 6/7 magic-map protocol so current carts
	// keep booting exactly as before.
	*(vu8 *)WL_SAVE_SLOT_INDEX_PORT = save_slot_index;
}

static void FlashCalcOffsets(void)
{
	itemlist = NULL;
	itemlist_offset = 0;
	flash_status_sector_offset = 0;
	if (sCartridgeProfile.backend != CART_BACKEND_NONE && flash_sector_size != 0)
	{
		flash_status_sector_offset = sCartridgeProfile.status_offset / flash_sector_size;
		itemlist = (u8 *)(AGB_ROM + sCartridgeProfile.itemlist_offset);
	}
}

static u16 CountItemListEntries(void)
{
	ItemConfig config;
	if (itemlist == NULL) return 0;
	for (u16 count = 0; count < WL_ROM_MAP_RECORD_COUNT; count++)
	{
		memcpy(&config, itemlist + (u32)count * ITEM_CONFIG_SIZE, sizeof(config));
		if (config.title_length == 0 || config.title_length == 0xFF) return count;
	}
	return WL_ROM_MAP_RECORD_COUNT;
}

IWRAM_CODE void FlashDetectType(void)
{
	u32 data;
	u16 ie = REG_IE;
	REG_IE = ie & 0xFFFE;
	flash_capacity_bytes = 0;
	flash_capacity_source = CART_CAPACITY_UNKNOWN;
	sDetectedNorId = 0;
	sDetectedNorId2 = 0xFFFF;
	sDetectedNorId3 = 0xFFFF;
	sDetectedBufferBytes = 0;

	// 2G cart with 6600M0U0BE (369-in-1)
	_FLASH_WRITE(0, 0xFF);
	_FLASH_WRITE(0, 0x90);
	data = *(vu32 *)AGB_ROM;
	_FLASH_WRITE(0, 0xFF);
	if (data == 0x88B0008A)
	{
		sDetectedNorId = data;
		REG_IE = ie;
		flash_type = 1;
		flash_sector_size = 0x40000;
		flash_capacity_bytes = 256U * 1024 * 1024;
		flash_capacity_source = CART_CAPACITY_ID;
		FlashCalcOffsets();
		return;
	}

	// 512M cart with MSP55LV100S (Zelda Classic Collection 7-in-1)
	_FLASH_WRITE(0, 0xF0F0);
	_FLASH_WRITE(0xAAA, 0xAAA9);
	_FLASH_WRITE(0x555, 0x5556);
	_FLASH_WRITE(0xAAA, 0x9090);
	data = *(vu32 *)AGB_ROM;
	_FLASH_WRITE(0, 0xF0F0);
	if (data == 0x7E7D0102)
	{
		sDetectedNorId = data;
		REG_IE = ie;
		flash_type = 2;
		flash_sector_size = 0x20000;
		flash_capacity_bytes = 64U * 1024 * 1024;
		flash_capacity_source = CART_CAPACITY_ID;
		FlashCalcOffsets();
		return;
	}

	// 1G cart with MSP54LV100S (Zelda Classic Collection 7-in-1)
	_FLASH_WRITE(0, 0xF0);
	_FLASH_WRITE(0xAAA, 0xA9);
	_FLASH_WRITE(0x555, 0x56);
	_FLASH_WRITE(0xAAA, 0x90);
	data = *(vu32 *)AGB_ROM;
	_FLASH_WRITE(0, 0xF0);
	if (data == 0x227D0002)
	{
		sDetectedNorId = data;
		flash_capacity_bytes = FlashProbeCfiCapacity();
		REG_IE = ie;
		flash_type = 3;
		flash_sector_size = 0x20000;
		flash_capacity_source = flash_capacity_bytes != 0 ? CART_CAPACITY_CFI : CART_CAPACITY_UNKNOWN;
		FlashCalcOffsets();
		return;
	}
	
	// 1G or 2G cart with S29GL01G or MT28EW01G or S70GL02G and other similar nor flash (Chisflash 1.0G and 2.0G)
	// S70GL02G has two S29GL01G and has two command addresses, but the menu only write on the first die so it's as same as S29GL01G.
	_FLASH_WRITE(0, 0xF0);
	_FLASH_WRITE(0xAAA, 0xAA);
	_FLASH_WRITE(0x555, 0x55);
	_FLASH_WRITE(0xAAA, 0x90);
	data = *(vu32 *)AGB_ROM;
	if (data == 0x227E0001 || data == 0x227E0089 || data == 0x227E00C2) {
		// 在已有自动选择查询中读取完整器件ID，不增加擦除或编程操作。
		sDetectedNorId2 = *(vu16 *)(AGB_ROM + 0x1C);
		sDetectedNorId3 = *(vu16 *)(AGB_ROM + 0x1E);
	}
	_FLASH_WRITE(0, 0xF0);
	if (data == 0x227E0001 || data == 0x227E0089 || data == 0x227E00C2)
	{
		sDetectedNorId = data;
		flash_capacity_bytes = FlashProbeCfiCapacity();
		REG_IE = ie;
		flash_type = 4;
		flash_sector_size = 0x20000;
		flash_capacity_source = flash_capacity_bytes != 0 ? CART_CAPACITY_CFI : CART_CAPACITY_UNKNOWN;
		FlashCalcOffsets();
		return;
	}

	// Unknown type
	REG_IE = ie;
	flash_type = 0;
	flash_sector_size = 0x20000;
	FlashCalcOffsets();
	return;
}

IWRAM_CODE void FlashEraseSector(u32 address)
{
	if (flash_type == 0)
	{
		FlashDetectType();
	}
	vu8 _flash_type = flash_type;
	vu16 ie = REG_IE;
	REG_IE = ie & 0xFFFE;

	if (_flash_type == 1)
	{
		_FLASH_WRITE(address, 0xFF);
		_FLASH_WRITE(address, 0x60);
		_FLASH_WRITE(address, 0xD0);
		_FLASH_WRITE(address, 0x20);
		_FLASH_WRITE(address, 0xD0);
		while (1)
		{
			__asm("nop");
			if ((*((vu16 *)(AGB_ROM + address)) & 0x80) == 0x80)
			{
				break;
			}
		}
		_FLASH_WRITE(address, 0xFF);
	}
	else if (_flash_type == 2)
	{
		_FLASH_WRITE(0xAAA, 0xAAA9);
		_FLASH_WRITE(0x555, 0x5556);
		_FLASH_WRITE(0xAAA, 0x8080);
		_FLASH_WRITE(0xAAA, 0xAAA9);
		_FLASH_WRITE(0x555, 0x5556);
		_FLASH_WRITE(address, 0x3030);
		while (1)
		{
			__asm("nop");
			if ((*((vu16 *)(AGB_ROM + address))) == 0xFFFF)
			{
				break;
			}
		}
		_FLASH_WRITE(address, 0xF0F0);
	}
	else if (_flash_type == 3)
	{
		_FLASH_WRITE(0xAAA, 0xA9);
		_FLASH_WRITE(0x555, 0x56);
		_FLASH_WRITE(0xAAA, 0x80);
		_FLASH_WRITE(0xAAA, 0xA9);
		_FLASH_WRITE(0x555, 0x56);
		_FLASH_WRITE(address, 0x30);
		while (1)
		{
			__asm("nop");
			if ((*((vu16 *)(AGB_ROM + address))) == 0xFFFF)
			{
				break;
			}
		}
		_FLASH_WRITE(address, 0xF0);
	}
	else if (_flash_type == 4)
	{
		_FLASH_WRITE(0xAAA, 0xAA);
		_FLASH_WRITE(0x555, 0x55);
		_FLASH_WRITE(0xAAA, 0x80);
		_FLASH_WRITE(0xAAA, 0xAA);
		_FLASH_WRITE(0x555, 0x55);
		_FLASH_WRITE(address, 0x30);
		while (1)
		{
			__asm("nop");
			if ((*((vu16 *)(AGB_ROM + address))) == 0xFFFF)
			{
				break;
			}
		}
		_FLASH_WRITE(address, 0xF0);
	}

	REG_IE = ie;
}

IWRAM_CODE void FlashWriteData(u32 address, u32 length)
{
	if (flash_type == 0)
	{
		FlashDetectType();
	}
	u8 _flash_type = flash_type;
	vu16 *p_rom = (vu16 *)(AGB_ROM + address);
	vu16 ie = REG_IE;
	REG_IE = ie & 0xFFFE;

	if (_flash_type == 1)
	{
		for (int j = 0; j < (int)(length / 0x400); j++)
		{
			_FLASH_WRITE(address + (j * 0x400), 0xEA);
			while (1)
			{
				__asm("nop");
				if ((p_rom[(j * 0x200)] & 0x80) == 0x80)
				{
					break;
				}
			}
			_FLASH_WRITE(address + (j * 0x400), 0x1FF);
			for (int i = 0; i < 0x400; i += 2)
			{
				_FLASH_WRITE(address + (j * 0x400) + i, data_buffer[(j * 0x400) + i + 1] << 8 | data_buffer[(j * 0x400) + i]);
			}
			_FLASH_WRITE(address + (j * 0x400), 0xD0);
			while (1)
			{
				__asm("nop");
				if ((p_rom[(j * 0x200)] & 0x80) == 0x80)
				{
					break;
				}
			}
		}
		_FLASH_WRITE(address, 0xFF);
	}
	else if (_flash_type == 2)
	{
		for (int j = 0; j < (int)(length / 0x20); j++)
		{
			_FLASH_WRITE(0xAAA, 0xAAA9);
			_FLASH_WRITE(0x555, 0x5556);
			_FLASH_WRITE(address + (j * 0x20), 0x2526);
			_FLASH_WRITE(address + (j * 0x20), 0x0F0F);
			u16 data = 0;
			for (int i = 0; i < 0x20; i += 2)
			{
				__asm("nop");
				data = data_buffer[(j * 0x20) + i + 1] << 8 | data_buffer[(j * 0x20) + i];
				_FLASH_WRITE(address + (j * 0x20) + i, data);
			}
			_FLASH_WRITE(address + (j * 0x20), 0x292A);
			while (1)
			{
				__asm("nop");
				if (p_rom[(j * 0x10) + 0x0F] == data)
				{
					break;
				}
			}
		}
		_FLASH_WRITE(address, 0xF0F0);
	}
	else if (_flash_type == 3)
	{
		for (int j = 0; j < (int)(length / 0x40); j++)
		{
			_FLASH_WRITE(0xAAA, 0xA9);
			_FLASH_WRITE(0x555, 0x56);
			_FLASH_WRITE(address + (j * 0x40), 0x26);
			_FLASH_WRITE(address + (j * 0x40), 0x1F);
			u16 data = 0;
			for (int i = 0; i < 0x40; i += 2)
			{
				__asm("nop");
				data = data_buffer[(j * 0x40) + i + 1] << 8 | data_buffer[(j * 0x40) + i];
				_FLASH_WRITE(address + (j * 0x40) + i, data);
			}
			_FLASH_WRITE(address + (j * 0x40), 0x2A);
			while (1)
			{
				__asm("nop");
				if (p_rom[(j * 0x20) + 0x1F] == data)
				{
					break;
				}
			}
		}
		_FLASH_WRITE(address, 0xF0);
	}
	else if (_flash_type == 4)
	{
		for (int j = 0; j < (int)(length / 0x40); j++)
		{
			_FLASH_WRITE(0xAAA, 0xAA);
			_FLASH_WRITE(0x555, 0x55);
			_FLASH_WRITE(address + (j * 0x40), 0x25);
			_FLASH_WRITE(address + (j * 0x40), 0x1F);
			u16 data = 0;
			for (int i = 0; i < 0x40; i += 2)
			{
				__asm("nop");
				data = data_buffer[(j * 0x40) + i + 1] << 8 | data_buffer[(j * 0x40) + i];
				_FLASH_WRITE(address + (j * 0x40) + i, data);
			}
			_FLASH_WRITE(address + (j * 0x40), 0x29);
			while (1)
			{
				__asm("nop");
				if (p_rom[(j * 0x20) + 0x1F] == data)
				{
					break;
				}
			}
		}
		_FLASH_WRITE(address, 0xF0);
	}

	REG_IE = ie;
}

IWRAM_CODE void SRAMBankDetectType(void)
{
	vu8 _mapper_config4_bak = *(vu8 *)MAPPER_CONFIG4;
	*(vu8 *)MAPPER_CONFIG4 = 1;

	for (int i = 0; i < SRAM_SIZE; i++)
	{
		data_buffer[i] = ((vu8 *)AGB_SRAM)[i]; // read bank 0.
	}

	// method 1: select bank on 0x9000000
	*((volatile u8*)0x9000000) = 1;
	for (int i = 0; i < SRAM_SIZE; i++)
	{
		if (data_buffer[i] != ((vu8 *)AGB_SRAM)[i]) // not really a good way, but seems that no game save the same data in both banks so it works.
		{
			sram_bank_type = 1;
			*(vu8 *)MAPPER_CONFIG4 = _mapper_config4_bak;
			return;
		}
	}

	// add more methods here if needed.
	sram_bank_type = 0;
	*(vu8 *)MAPPER_CONFIG4 = _mapper_config4_bak;
}

IWRAM_CODE void SwitchBank(u8 bank)
{
	u8 _sram_bank_type = sram_bank_type;

	if (_sram_bank_type == 1) {
		*((volatile u8*)0x9000000) = bank & 0x01; // Chisflash 1.0G and 2.0G can switch between two ram banks.
		for(int i = 0; i < 5; i++) {
	        asm("nop");
	    }
		return;
	}
}

IWRAM_CODE void DrawBootStatusLine(u8 begin, u8 end)
{
	for (int i = begin; i < end; i++)
	{
		((u16 *)AGB_VRAM)[(120 * 159) + i] = 0;
	}
}

IWRAM_CODE static u8 LegacySendLaunchMap(ItemConfig config)
{
	*(vu8 *)MAPPER_CONFIG1 = ((config.rom_offset / 0x40) & 0xF) << 4;
	*(vu8 *)MAPPER_CONFIG2 = 0x40 + (config.rom_offset % 0x40);
	*(vu8 *)MAPPER_CONFIG3 = 0x40 - config.rom_size;

	u32 timeout = 0x2FFF;
	while (((vu16 *)AGB_ROM)[0x58] == 0x4B4C)
	{
		if (!timeout--)
		{
			REG_IE = 1;
			REG_BLDY = 0;
			SystemCall(5);
			return BOOT_GAME_ERR_MAPPER_NO_RESPONSE;
		}
	}
	*(vu8 *)MAPPER_CONFIG2 |= 0x80;
	return BOOT_GAME_OK;
}

IWRAM_CODE static BOOL HasLocalMagicMapper(void)
{
	static const u8 expected[8] = { 'C', 'H', 'M', '1', 1, 3, 63, 8 };
	u8 reply[8];
	(void)*(vu8 *)(AGB_SRAM + 0xFFF0);
	(void)*(vu8 *)(AGB_SRAM + 0xFFE0);
	(void)*(vu8 *)(AGB_SRAM + 0xFFD0);
	for (u8 i = 0; i < sizeof(reply); i++) reply[i] = *(vu8 *)(AGB_SRAM + 0xFFF0 + i);
	return memcmp(reply, expected, sizeof(reply)) == 0;
}

IWRAM_CODE u8 BootGame(ItemConfig config, FlashStatus status, u16 entry_slot)
{
	if (!sCartridgeProfile.initialized) CartridgeInitialize();
	if (sCartridgeProfile.metadata_error) return BOOT_GAME_ERR_IMAGE_METADATA;
	if (sCartridgeProfile.flash_type == 0 || sCartridgeProfile.backend == CART_BACKEND_NONE)
		return BOOT_GAME_ERR_UNSUPPORTED_CART;
	if (!CartridgeCanLaunch(config, entry_slot)) return BOOT_GAME_ERR_ENTRY_RANGE;
	BOOL local_magic_mapper = HasLocalMagicMapper();
	if (local_magic_mapper) status.sram_bank_type = 1;

	WlRomMapRecord wl_record;
	if (sCartridgeProfile.backend == CART_BACKEND_WL || sCartridgeProfile.backend == CART_BACKEND_UNIVERSAL)
	{
		u8 wl_error = ReadWlRomMapRecordInternal(entry_slot, &wl_record);
		if (wl_error != BOOT_GAME_OK) return wl_error;
		if (local_magic_mapper)
		{
			if (wl_record.logical_bucket_count > 8) return BOOT_GAME_ERR_ENTRY_RANGE;
			for (u8 i = 0; i < wl_record.logical_bucket_count; i++)
				if (wl_record.phys_block_id[i] > 63) return BOOT_GAME_ERR_ENTRY_RANGE;
		}
	}

	u8 previous_save_index = status.last_boot_save_index;
	SAVE_TYPE previous_save_type = status.last_boot_save_type;
	if (sCartridgeProfile.backend == CART_BACKEND_WL && !local_magic_mapper)
	{
		status.last_boot_save_index = 0xFF;
		status.last_boot_save_type = SRAM_NONE;
	}
	else
	{
		status.last_boot_save_index = config.save_index;
		status.last_boot_save_type = config.save_type;
	}
	FlashStoreStatus(status);

	REG_BLDCNT = 0x00FF;
	for (u8 i = 0; i < 17; i++)
	{
		REG_BLDY = i;
		SystemCall(5);
	}
	REG_IE = 0;

	if (sCartridgeProfile.backend == CART_BACKEND_WL && !local_magic_mapper)
	{
		WlSendSaveSlotIndex((u8)entry_slot);
		if (!WlTryLaunchMap(&wl_record))
		{
			REG_IE = 1;
			REG_BLDY = 0;
			return BOOT_GAME_ERR_MAPPER_NO_RESPONSE;
		}
		for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT >> 1; i++) ((vu16 *)AGB_VRAM)[i] = 0;
		REG_BLDY = 0;
		__asm("swi 0");
		return BOOT_GAME_ERR_LAUNCH_FAILED;
	}

	if (sCartridgeProfile.backend == CART_BACKEND_UNIVERSAL && !local_magic_mapper)
	{
		// A universal image carries both maps. The physical WL mapper has no ID
		// register, so use its native launch handshake and restore SRAM ports when
		// the ROM window does not move on a normal multicart.
		sram_register_backup[4] = *(vu8 *)WL_MAPPER_CONTROL;
		sram_register_backup[5] = *(vu8 *)WL_MAPPER_DATA;
		sram_register_backup[6] = *(vu8 *)WL_SAVE_SLOT_INDEX_PORT;
		WlSendSaveSlotIndex((u8)entry_slot);
		if (WlTryLaunchMap(&wl_record))
		{
			for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT >> 1; i++) ((vu16 *)AGB_VRAM)[i] = 0;
			REG_BLDY = 0;
			__asm("swi 0");
			return BOOT_GAME_ERR_LAUNCH_FAILED;
		}
		*(vu8 *)WL_MAPPER_CONTROL = sram_register_backup[4];
		*(vu8 *)WL_MAPPER_DATA = sram_register_backup[5];
		*(vu8 *)WL_SAVE_SLOT_INDEX_PORT = sram_register_backup[6];
	}

	if (status.sram_bank_type == 0)
	{
		SRAMBankDetectType();
		status.sram_bank_type = sram_bank_type;
		FlashStoreStatus(status);
	}
	else
	{
		sram_bank_type = status.sram_bank_type;
	}
	BOOL support_sram_bank = sram_bank_type != 0;

	sram_register_backup[0] = *(vu8 *)MAPPER_CONFIG1;
	sram_register_backup[1] = *(vu8 *)MAPPER_CONFIG2;
	sram_register_backup[2] = *(vu8 *)MAPPER_CONFIG3;
	sram_register_backup[3] = *(vu8 *)MAPPER_CONFIG4;
	*(vu8 *)MAPPER_CONFIG4 = 1;

	if (status.battery_present && previous_save_type != SRAM_NONE &&
		previous_save_index < sCartridgeProfile.save_slot_count)
	{
		for (int i = 0; i < SRAM_SIZE; i++) data_buffer[i] = ((vu8 *)AGB_SRAM)[i];
		if (support_sram_bank)
		{
			SwitchBank(1);
			for (int i = 0; i < SRAM_SIZE; i++) data_buffer[SRAM_SIZE + i] = ((vu8 *)AGB_SRAM)[i];
			SwitchBank(0);
		}
		data_buffer[2] = sram_register_backup[0];
		data_buffer[3] = sram_register_backup[1];
		data_buffer[4] = sram_register_backup[2];
		data_buffer[5] = sram_register_backup[3];
		u32 previous_save_address = sCartridgeProfile.save_area_offset +
			(u32)previous_save_index * sCartridgeProfile.save_slot_stride;
		FlashEraseSector(previous_save_address);
		FlashWriteData(previous_save_address, support_sram_bank ? SRAM_SIZE * 2 : SRAM_SIZE);
	}

	*(vu8 *)MAPPER_CONFIG4 = 0;
	if (config.save_type != SRAM_NONE)
	{
		u32 save_address = sCartridgeProfile.save_area_offset +
			(u32)config.save_index * sCartridgeProfile.save_slot_stride;
		for (int i = 0; i < (support_sram_bank ? SRAM_SIZE * 2 : SRAM_SIZE); i += 2)
		{
			u16 value = *((vu16 *)(AGB_ROM + save_address + i));
			data_buffer[i] = value & 0xFF;
			data_buffer[i + 1] = value >> 8;
		}
	}

	// Standalone 01G magic cards retain menu-managed SRAM saves; only the ROM mapper changes.
	u8 launch_error;
	if (local_magic_mapper && sCartridgeProfile.backend != CART_BACKEND_LEGACY)
	{
		launch_error = WlTryLaunchMap(&wl_record) ? BOOT_GAME_OK : BOOT_GAME_ERR_MAPPER_NO_RESPONSE;
		if (launch_error != BOOT_GAME_OK)
		{
			REG_IE = 1;
			REG_BLDY = 0;
		}
	}
	else launch_error = LegacySendLaunchMap(config);
	if (launch_error != BOOT_GAME_OK) return launch_error;

	if (config.save_type != SRAM_NONE)
	{
		for (int i = 0; i < SRAM_SIZE; i++) ((vu8 *)AGB_SRAM)[i] = data_buffer[i];
		if (support_sram_bank)
		{
			SwitchBank(1);
			for (int i = 0; i < SRAM_SIZE; i++) ((vu8 *)AGB_SRAM)[i] = data_buffer[SRAM_SIZE + i];
			SwitchBank(0);
		}
	}
	else
	{
		*(vu8 *)MAPPER_CONFIG1 = sram_register_backup[0];
		*(vu8 *)MAPPER_CONFIG2 = sram_register_backup[1];
		*(vu8 *)MAPPER_CONFIG3 = sram_register_backup[2];
		*(vu8 *)MAPPER_CONFIG4 = sram_register_backup[3];
	}

	for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT >> 1; i++) ((vu16 *)AGB_VRAM)[i] = 0;
	REG_BLDY = 0;
	__asm("swi 0");
	return BOOT_GAME_ERR_LAUNCH_FAILED;
}
