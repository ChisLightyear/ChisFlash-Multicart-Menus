/*
GBA Multi Game Menu
Author: Lesserkuma (github.com/lesserkuma), ZaindORp (github.com/orzgithub)
*/

#include <gba.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "flash.h"

extern void BootGameReset(void) __attribute__((long_call, noreturn));

u8 flash_type;
u8 sram_bank_type;
u8 *itemlist;
u16 itemlist_offset;
u32 flash_sector_size;
u32 flash_itemlist_sector_offset;
u32 flash_status_sector_offset;
u32 flash_save_sector_offset;
EWRAM_BSS u8 sram_register_backup[4];
EWRAM_BSS u8 data_buffer[SRAM_SIZE * 2];

static const WlMappingHeader *GetWlMappingHeader(void)
{
	return (const WlMappingHeader *)((const u8 *)AGB_ROM + WL_MAPPING_OFFSET);
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

static BOOL IsWlMappingHeaderValid(const WlMappingHeader *header)
{
	const u8 *mapping_base;
	u32 bucket_owner_end;
	u32 record_table_end;
	u32 computed_checksum;

	if (header->magic != WLMI_MAGIC)
		return FALSE;
	if (header->version != WLMI_VERSION_CURRENT)
		return FALSE;
	if (header->header_size < sizeof(WlMappingHeader))
		return FALSE;
	if (header->bucket_owner_entry_size != sizeof(u16))
		return FALSE;
	if (header->bucket_owner_count < WL_MIN_BUCKET_COUNT)
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

	mapping_base = (const u8 *)AGB_ROM + WL_MAPPING_OFFSET;
	computed_checksum = ComputeWlMappingChecksum(mapping_base);
	if (computed_checksum != header->mapping_checksum)
		return FALSE;

	return TRUE;
}

static BOOL IsWlRomMapRecordValid(const WlRomMapRecord *record)
{
	if (record->logical_bucket_count == 0 || record->logical_bucket_count > WL_RECORD_BUCKET_CAPACITY)
		return FALSE;

	for (u8 i = 0; i < record->logical_bucket_count; i++)
	{
		if (record->phys_block_id[i] == 0 || record->phys_block_id[i] > WL_MAX_PHYSICAL_BUCKET_ID)
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
	if (!IsWlMappingHeaderValid(header))
		return BOOT_GAME_ERR_WL_METADATA;

	record_offset = header->rom_map_record_offset + ((u32)entry_slot * header->rom_map_record_size);
	if ((record_offset + sizeof(WlRomMapRecord)) > WL_MAPPING_SECTOR_SIZE)
		return BOOT_GAME_ERR_WL_METADATA;

	record_base = (const u8 *)AGB_ROM + WL_MAPPING_OFFSET + record_offset;
	memcpy(record, record_base, sizeof(WlRomMapRecord));
	if (!IsWlRomMapRecordValid(record))
		return BOOT_GAME_ERR_WL_RECORD;

	return BOOT_GAME_OK;
}

BOOL ReadWlRomMapRecord(u16 entry_slot, WlRomMapRecord *record)
{
	return ReadWlRomMapRecordInternal(entry_slot, record) == BOOT_GAME_OK;
}

IWRAM_CODE static u8 WlSendLaunchMap(const WlRomMapRecord *record)
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
		if (!timeout--)
		{
			REG_IE = 1;
			REG_BLDY = 0;
			SystemCall(5);
			return BOOT_GAME_ERR_MAPPER_NO_RESPONSE;
		}
	}

	return BOOT_GAME_OK;
}

IWRAM_CODE static void WlSendSaveSlotIndex(u8 save_slot_index)
{
	// Sideband launch metadata for the main-controller save-slot path.
	// This stays off the existing 6/7 magic-map protocol so current carts
	// keep booting exactly as before.
	*(vu8 *)WL_SAVE_SLOT_INDEX_PORT = save_slot_index;
}

void FlashCalcOffsets(void)
{
	if (flash_sector_size == WL_MAPPING_SECTOR_SIZE)
	{
		flash_itemlist_sector_offset = WL_ITEMLIST_OFFSET / flash_sector_size;
		flash_status_sector_offset = WL_STATUS_OFFSET / flash_sector_size;
		flash_save_sector_offset = WL_SAVE_AREA_OFFSET / flash_sector_size;
	}
	else
	{
		u32 own_size = (u32)(&__rom_end__) - 0x8000000;
		flash_itemlist_sector_offset = own_size;
		flash_itemlist_sector_offset = 0x40000 - (flash_itemlist_sector_offset % 0x40000) + flash_itemlist_sector_offset;
		flash_itemlist_sector_offset = _DIV_CEIL(flash_itemlist_sector_offset, flash_sector_size);
		flash_status_sector_offset = flash_itemlist_sector_offset + 1;
		flash_save_sector_offset = flash_status_sector_offset + 1;
	}
	itemlist = (u8 *)(AGB_ROM + flash_itemlist_sector_offset * flash_sector_size);
}

IWRAM_CODE void FlashDetectType(void)
{
	u32 data;
	u16 ie = REG_IE;
	REG_IE = ie & 0xFFFE;

	// 2G cart with 6600M0U0BE (369-in-1)
	_FLASH_WRITE(0, 0xFF);
	_FLASH_WRITE(0, 0x90);
	data = *(vu32 *)AGB_ROM;
	_FLASH_WRITE(0, 0xFF);
	if (data == 0x88B0008A)
	{
		REG_IE = ie;
		flash_type = 1;
		flash_sector_size = 0x40000;
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
		REG_IE = ie;
		flash_type = 2;
		flash_sector_size = 0x20000;
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
		REG_IE = ie;
		flash_type = 3;
		flash_sector_size = 0x20000;
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
	_FLASH_WRITE(0, 0xF0);
	if (data == 0x227E0001 || data == 0x227E0089 || data == 0x227E00C2)
	{
		REG_IE = ie;
		flash_type = 4;
		flash_sector_size = 0x20000;
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

IWRAM_CODE u8 BootGame(ItemConfig config, FlashStatus status, u16 entry_slot, u8 save_slot_index)
{
	u32 _flash_sector_size = flash_sector_size;
	u32 _flash_save_block_offset = flash_save_sector_offset;
	u32 _flash_status_block_offset = flash_status_sector_offset;
	WlRomMapRecord wl_record;
	u8 wl_error;

	// Check if supported flash chip is present
	FlashDetectType();
	u8 _flash_type = flash_type;
	if (_flash_type == 0)
		return BOOT_GAME_ERR_UNSUPPORTED_CART;
	BOOL local_magic_mapper = HasLocalMagicMapper();
	BOOL menu_managed_save = local_magic_mapper || !WL_MCU_SAVE_CONTROLLER;
	if (local_magic_mapper) status.sram_bank_type = 1;

	wl_error = ReadWlRomMapRecordInternal(entry_slot, &wl_record);
	if (wl_error != BOOT_GAME_OK)
		return wl_error;
	if (local_magic_mapper)
	{
		if (wl_record.logical_bucket_count > 8) return BOOT_GAME_ERR_WL_RECORD;
		for (u8 i = 0; i < wl_record.logical_bucket_count; i++)
			if (wl_record.phys_block_id[i] > 63) return BOOT_GAME_ERR_WL_RECORD;
	}
	
	if (menu_managed_save && status.sram_bank_type == 0) {
		SRAMBankDetectType();
		status.sram_bank_type = sram_bank_type;
	} else {
		sram_bank_type = status.sram_bank_type;
	}
	u8 _sram_bank_type = sram_bank_type;
	BOOL _support_sram_bank = _sram_bank_type == 0 ? FALSE : TRUE;

	// Temporarily store SRAM values located at mapper registers
	sram_register_backup[0] = *(vu8 *)MAPPER_CONFIG1;
	sram_register_backup[1] = *(vu8 *)MAPPER_CONFIG2;
	sram_register_backup[2] = *(vu8 *)MAPPER_CONFIG3;
	sram_register_backup[3] = *(vu8 *)MAPPER_CONFIG4;

	if (menu_managed_save) {
		// Enable SRAM access
		*(vu8 *)MAPPER_CONFIG4 = 1;

		// Write previous SRAM to flash
		if (status.battery_present)
		{
			if (status.last_boot_save_type != SRAM_NONE)
			{
				for (int i = 0; i < SRAM_SIZE; i++)
				{
					data_buffer[i] = ((vu8 *)AGB_SRAM)[i];
				}
				if (_support_sram_bank)
				{
					SwitchBank(1);
					for (int i = 0; i < SRAM_SIZE; i++)
					{
						data_buffer[SRAM_SIZE + i] = ((vu8 *)AGB_SRAM)[i];
					}
					SwitchBank(0);
				}
				data_buffer[2] = sram_register_backup[0];
				data_buffer[3] = sram_register_backup[1];
				data_buffer[4] = sram_register_backup[2];
				data_buffer[5] = sram_register_backup[3];
				FlashEraseSector((_flash_save_block_offset + status.last_boot_save_index) * _flash_sector_size);
				FlashWriteData((_flash_save_block_offset + status.last_boot_save_index) * _flash_sector_size, _support_sram_bank ? SRAM_SIZE * 2 : SRAM_SIZE);
			}
		}

		// Save status to flash
	}
	status.last_boot_save_index = menu_managed_save ? config.save_index : 0xFF;
	status.last_boot_save_type = menu_managed_save ? config.save_type : SRAM_NONE;
	memset((void *)data_buffer, 0, 0x1000);
	memcpy(data_buffer, &status, sizeof(status));
	FlashEraseSector(_flash_status_block_offset * flash_sector_size);
	FlashWriteData(_flash_status_block_offset * flash_sector_size, 0x1000);

	if (menu_managed_save) {
		// Disable SRAM access
		*(vu8 *)MAPPER_CONFIG4 = 0;

		// Read new SRAM from flash
		if (config.save_type != SRAM_NONE)
		{
			for (int i = 0; i < (_support_sram_bank ? SRAM_SIZE * 2 : SRAM_SIZE); i += 2)
			{
				data_buffer[i] = *((vu16 *)(AGB_ROM + ((_flash_save_block_offset + config.save_index) * _flash_sector_size) + i)) & 0xFF;
				data_buffer[i + 1] = *((vu16 *)(AGB_ROM + ((_flash_save_block_offset + config.save_index) * _flash_sector_size) + i)) >> 8;
			}
		}
	}

	// Fade out
	REG_BLDCNT = 0x00FF;
	for (u8 i = 0; i < 17; i++)
	{
		REG_BLDY = i;
		SystemCall(5);
	}

	// Disable interrupts
	REG_IE = 0;

	// Emit the stable 0-based entry slot for main-controller save isolation.
	if (!local_magic_mapper) WlSendSaveSlotIndex(save_slot_index);

	// Load fragmented bucket map into the new mapper protocol.
	wl_error = WlSendLaunchMap(&wl_record);
	if (wl_error != BOOT_GAME_OK)
		return wl_error;

	if (menu_managed_save) {
		// Write buffer to SRAM
		if (config.save_type != SRAM_NONE)
		{
			for (int i = 0; i < SRAM_SIZE; i++)
			{
				((vu8 *)AGB_SRAM)[i] = data_buffer[i];
			}
			if (_support_sram_bank)
			{
				SwitchBank(1);
				for (int i = 0; i < SRAM_SIZE; i++)
				{
					((vu8 *)AGB_SRAM)[i] = data_buffer[SRAM_SIZE + i];
				}
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
	}

	// Clear palette
	for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT >> 1; i++)
	{
		((vu16 *)AGB_VRAM)[i] = 0;
	}
	REG_BLDY = 0;

	// Boot ROM
	BootGameReset();

	return BOOT_GAME_ERR_LAUNCH_FAILED;
}
