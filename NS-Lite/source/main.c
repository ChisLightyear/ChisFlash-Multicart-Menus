/*
GBA Multi Game Menu
Author: Lesserkuma (github.com/lesserkuma), ZaindORp (github.com/orzgithub)
*/

#include <gba.h>
#include <gba_input.h>
#include <gba_interrupt.h>
#include <stdio.h>
#include <string.h>

#include "version.h"
#include "audio.h"
#include "main.h"
#include "font.h"
#include "flash.h"
#include "ui.h"

#define MENU_NAV_KEYS (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)
#define MENU_ALL_KEYS 0x03FF
#define MENU_REPEAT_INITIAL_DELAY 24
#define MENU_REPEAT_INTERVAL 6

typedef enum MenuPendingAction_
{
	MENU_ACTION_NONE = 0,
	MENU_ACTION_LAUNCH,
	MENU_ACTION_RESET
} MenuPendingAction;

extern u8 *itemlist;
extern u16 itemlist_offset;
extern u32 flash_sector_size;
extern u32 flash_status_sector_offset;

ItemConfig sItemConfig;
FlashStatus sFlashStatus;

static volatile u16 sInputHeld;
static volatile u16 sInputPressed;
static volatile u16 sNavigationEvents;
static volatile u16 sNavigationRepeatEvents;
static u8 sNavigationRepeatTimer;

static void MenuVBlankInput(void) {
	AudioVBlank();
	u16 held = (~REG_KEYINPUT) & MENU_ALL_KEYS;
	u16 pressed = held & ~sInputHeld;
	sInputHeld = held;
	sInputPressed |= pressed;

	u16 navigation_held = held & MENU_NAV_KEYS;
	u16 navigation_pressed = pressed & MENU_NAV_KEYS;
	if (navigation_pressed != 0) {
		sNavigationEvents |= navigation_pressed;
		sNavigationRepeatTimer = MENU_REPEAT_INITIAL_DELAY;
	} else if (navigation_held != 0) {
		if (sNavigationRepeatTimer > 0) sNavigationRepeatTimer--;
		if (sNavigationRepeatTimer == 0) {
			sNavigationEvents |= navigation_held;
			sNavigationRepeatEvents |= navigation_held;
			sNavigationRepeatTimer = MENU_REPEAT_INTERVAL;
		}
	} else {
		sNavigationRepeatTimer = 0;
	}
}

static void MenuWaitForVBlank(void) {
	VBlankIntrWait();
	AudioFrame();
}

static void ReadMenuInput(u16 *held, u16 *pressed, u16 *navigation_events, u16 *navigation_repeat_events) {
	u16 ime = REG_IME;
	REG_IME = 0;
	*held = sInputHeld;
	*pressed = sInputPressed;
	*navigation_events = sNavigationEvents;
	*navigation_repeat_events = sNavigationRepeatEvents;
	sInputPressed = 0;
	sNavigationEvents = 0;
	sNavigationRepeatEvents = 0;
	REG_IME = ime;
}

static void PlayNavigationSound(BOOL repeated) {
	AudioPlayMove(repeated);
}

static u16 GetAbsoluteEntrySlot(u16 group_offset_bytes, u16 visible_index) {
	return (group_offset_bytes / ITEM_CONFIG_SIZE) + visible_index;
}

static void LoadFlashStatusOrDefaults(u16 roms_total) {
	memcpy(&sFlashStatus, (void *)(AGB_ROM + flash_status_sector_offset * flash_sector_size), sizeof(sFlashStatus));
	if ((sFlashStatus.magic != MAGIC_FLASH_STATUS) || (sFlashStatus.last_boot_menu_index >= roms_total)) {
		if (sFlashStatus.magic != MAGIC_FLASH_STATUS) {
			sFlashStatus.battery_present = 1;
		}
		sFlashStatus.magic = MAGIC_FLASH_STATUS;
		sFlashStatus.version = 0;
		sFlashStatus.last_boot_menu_index = 0xFFFF;
		sFlashStatus.last_boot_save_index = 0xFF;
		sFlashStatus.last_boot_save_type = SRAM_NONE;
		sFlashStatus.sram_bank_type = 0;
	}
}

static void SetStatusText(char *ascii, u16 *unicode, const char *text, u8 *length) {
	snprintf(ascii, 64, "%s", text);
	memset(unicode, 0, 64 * sizeof(u16));
	*length = Utf8ToUnicode(ascii, unicode, 64);
}

static void ShowFatalMenu(const char *message) {
	char status_ascii[64];
	u16 status_unicode[64];
	u8 status_length;
	UiMenuMotion motion;
	UiFeedback feedback;
	UiTitleScroll title_scroll;
	SetStatusText(status_ascii, status_unicode, message, &status_length);
	UiMotionInitialize(&motion, 0, 0);
	UiFeedbackInitialize(&feedback);
	UiTitleScrollInitialize(&title_scroll);
	UiMenuView view = {
		.itemlist = NULL,
		.itemlist_offset = 0,
		.roms_total = 0,
		.selected_index = 0,
		.status_text = status_unicode,
		.status_length = status_length,
		.status_tone = UI_STATUS_ERROR,
		.motion = &motion,
		.feedback = &feedback,
		.title_scroll = &title_scroll
	};
	AudioPlay(UI_SOUND_ERROR);
	UiRenderMenu(&view);
	MenuWaitForVBlank();
	UiPresent();
	while (1) MenuWaitForVBlank();
}

static UiStatusTone BuildStatusText(
	u16 selected_index,
	BOOL show_credits,
	BOOL show_debug,
	u8 boot_failed,
	char *ascii,
	u16 *unicode,
	u8 *length
) {
	UiStatusTone tone = UI_STATUS_NORMAL;

	if (boot_failed != 0) {
		tone = UI_STATUS_ERROR;
		if (boot_failed == BOOT_GAME_ERR_UNSUPPORTED_CART) {
			SetStatusText(ascii, unicode, "不支持此卡带", length);
		} else if (boot_failed == BOOT_GAME_ERR_MAPPER_NO_RESPONSE) {
			SetStatusText(ascii, unicode, "映射器无响应", length);
		} else if (boot_failed == BOOT_GAME_ERR_WL_METADATA) {
			SetStatusText(ascii, unicode, "缺少映射表", length);
		} else if (boot_failed == BOOT_GAME_ERR_WL_RECORD) {
			SetStatusText(ascii, unicode, "映射记录无效", length);
		} else if (boot_failed == BOOT_GAME_ERR_IMAGE_METADATA) {
			SetStatusText(ascii, unicode, "镜像信息错误", length);
		} else if (boot_failed == BOOT_GAME_ERR_ENTRY_RANGE) {
			SetStatusText(ascii, unicode, "游戏目录错误", length);
		} else {
			SetStatusText(ascii, unicode, "启动失败", length);
		}
	} else if (show_credits) {
		snprintf(ascii, 64, "LK %s", BUILDTIME);
		memset(unicode, 0, 64 * sizeof(u16));
		*length = Utf8ToUnicode(ascii, unicode, 64);
	} else if (show_debug) {
		u16 entry_slot = GetAbsoluteEntrySlot(itemlist_offset, selected_index);
		const CartridgeProfile *cartridge = CartridgeGetProfile();
		if (cartridge->backend == CART_BACKEND_LEGACY) {
			snprintf(
				ascii,
				64,
				"LEG E%03u O%03u N%02u",
				(unsigned int)entry_slot,
				(unsigned int)sItemConfig.rom_offset,
				(unsigned int)sItemConfig.rom_size
			);
		} else {
			WlRomMapRecord wl_record;
			if (ReadWlRomMapRecord(entry_slot, &wl_record)) {
				const char *mode = cartridge->backend == CART_BACKEND_WL ? "WL" : "UNI";
				snprintf(
					ascii,
					64,
					"%s E%03u B%02u O%03u",
					mode,
					(unsigned int)entry_slot,
					(unsigned int)wl_record.logical_bucket_count,
					(unsigned int)sItemConfig.rom_offset
				);
			} else {
				snprintf(ascii, 64, "条目%03u 映射错误", (unsigned int)entry_slot);
				tone = UI_STATUS_ERROR;
			}
		}
		memset(unicode, 0, 64 * sizeof(u16));
		*length = Utf8ToUnicode(ascii, unicode, 64);
	} else {
		SetStatusText(ascii, unicode, "系统就绪", length);
	}

	return tone;
}

int main(void) {
	char status_ascii[64];
	u16 status_unicode[64];
	u16 roms_total = 0;
	u16 selected_index = 0;
	u16 kHeld = 0;
	u16 kDown = 0;
	u16 kHeld_boot = 0;
	u8 status_length = 0;
	UiMenuMotion motion;
	UiFeedback feedback;
	UiTitleScroll title_scroll;
	MenuPendingAction pending_action = MENU_ACTION_NONE;
	BOOL pending_skip_save = FALSE;
	BOOL show_debug = FALSE;
	BOOL show_credits = FALSE;
	BOOL redraw = TRUE;
	u8 boot_failed = 0;

	irqInit();
	// Finish NOR command-mode detection before enabling Direct Sound DMA.
	CartridgeInitialize();
	AudioInitialize();
	irqSet(IRQ_VBLANK, MenuVBlankInput);
	irqEnable(IRQ_VBLANK);

	UiInitialize();
	const CartridgeProfile *cartridge = CartridgeGetProfile();
	if (cartridge->metadata_error) ShowFatalMenu("镜像信息错误");
	if (cartridge->backend == CART_BACKEND_NONE || cartridge->flash_type == 0) ShowFatalMenu("卡带识别失败");
	u16 image_entry_count = cartridge->entry_count;

	scanKeys();
	kHeld = keysHeld();
	sInputHeld = kHeld;
	kHeld_boot = kHeld;
	if ((kHeld & KEY_SELECT) && (kHeld & KEY_R)) {
		show_credits = TRUE;
	} else if (kHeld & KEY_SELECT) {
		show_debug = TRUE;
	}
	if (kHeld != 0) {
		BOOL found_keys = FALSE;
		for (u16 entry_slot = 0; entry_slot < image_entry_count; entry_slot++) {
			itemlist_offset = entry_slot * ITEM_CONFIG_SIZE;
			memcpy(&sItemConfig, itemlist + itemlist_offset, sizeof(sItemConfig));
			if (sItemConfig.keys == kHeld) {
				found_keys = TRUE;
				break;
			}
		}
		if (!found_keys) {
			kHeld = 0;
			itemlist_offset = 0;
		}
	}

	u16 group_start_slot = itemlist_offset / ITEM_CONFIG_SIZE;
	for (roms_total = 0; group_start_slot + roms_total < image_entry_count; roms_total++) {
		memcpy(&sItemConfig, itemlist + itemlist_offset + ITEM_CONFIG_SIZE * roms_total, sizeof(sItemConfig));
		if (sItemConfig.keys != kHeld) break;
	}
	UiMotionInitialize(&motion, 0, roms_total);
	UiFeedbackInitialize(&feedback);
	UiTitleScrollInitialize(&title_scroll);

	UiMenuView view = {
		.itemlist = itemlist,
		.itemlist_offset = itemlist_offset,
		.roms_total = roms_total,
		.selected_index = 0,
		.status_text = status_unicode,
		.status_length = 0,
		.status_tone = UI_STATUS_NORMAL,
		.motion = &motion,
		.feedback = &feedback,
		.title_scroll = &title_scroll
	};

	if (roms_total == 0) {
		SetStatusText(status_ascii, status_unicode, "请先用合卡器添加游戏", &status_length);
		view.status_length = status_length;
		UiRenderMenu(&view);
		MenuWaitForVBlank();
		UiPresent();
		while (1) MenuWaitForVBlank();
	}

	LoadFlashStatusOrDefaults(roms_total);
	if (sFlashStatus.last_boot_menu_index != 0xFFFF) {
		selected_index = sFlashStatus.last_boot_menu_index;
	}
	UiMotionInitialize(&motion, selected_index, roms_total);
	UiTitleScrollInitialize(&title_scroll);

	if (roms_total == 1) {
		u16 entry_slot = GetAbsoluteEntrySlot(itemlist_offset, 0);
		memcpy(&sItemConfig, itemlist + itemlist_offset, sizeof(sItemConfig));
		AudioShutdown();
		if (!CartridgeCanLaunch(sItemConfig, entry_slot)) {
			boot_failed = BOOT_GAME_ERR_LAUNCH_FAILED;
		} else {
			boot_failed = BootGame(sItemConfig, sFlashStatus, entry_slot);
		}
		if (boot_failed != 0) {
			REG_IE = 1;
			AudioInitialize();
			AudioPlay(UI_SOUND_ERROR);
			UiFeedbackStart(&feedback, UI_FEEDBACK_ERROR, 0);
		}
	}

	while (1) {
		if (redraw) {
			memcpy(
				&sItemConfig,
				itemlist + itemlist_offset + ITEM_CONFIG_SIZE * selected_index,
				sizeof(sItemConfig)
			);
			view.selected_index = selected_index;
			view.status_tone = BuildStatusText(
				selected_index,
				show_credits,
				show_debug,
				boot_failed,
				status_ascii,
				status_unicode,
				&status_length
			);
			view.status_length = status_length;
			UiRenderMenu(&view);
		}

		MenuWaitForVBlank();
		if (redraw) {
			UiPresent();
			redraw = FALSE;
		}

		u16 navigation_event = 0;
		u16 navigation_repeat_event = 0;
		ReadMenuInput(&kHeld, &kDown, &navigation_event, &navigation_repeat_event);
		if ((kHeld_boot == 0) || (kHeld != kHeld_boot)) {
			kHeld_boot = 0;
		} else {
			kHeld = 0;
			kDown = 0;
			navigation_event = 0;
			navigation_repeat_event = 0;
		}

		u16 key_event = (kDown & ~MENU_NAV_KEYS) | navigation_event;
		if (pending_action != MENU_ACTION_NONE) {
			BOOL visual_changed = UiFeedbackAdvance(&feedback);
			if (UiMotionAdvance(&motion)) visual_changed = TRUE;
			if (visual_changed) {
				redraw = TRUE;
				continue;
			}

			MenuPendingAction action = pending_action;
			pending_action = MENU_ACTION_NONE;
			if (action == MENU_ACTION_RESET) {
				AudioShutdown();
				SystemCall(0);
				continue;
			}

			sFlashStatus.last_boot_menu_index = selected_index;
			AudioShutdown();
			SetStatusText(status_ascii, status_unicode, "正在加载", &status_length);
			view.status_length = status_length;
			view.status_tone = UI_STATUS_BUSY;
			UiRenderMenu(&view);
			MenuWaitForVBlank();
			UiPresent();

			if (pending_skip_save) sFlashStatus.last_boot_save_type = SRAM_NONE;
			pending_skip_save = FALSE;
			u16 entry_slot = GetAbsoluteEntrySlot(itemlist_offset, selected_index);
			if (!CartridgeCanLaunch(sItemConfig, entry_slot)) {
				boot_failed = BOOT_GAME_ERR_LAUNCH_FAILED;
			} else {
				boot_failed = BootGame(sItemConfig, sFlashStatus, entry_slot);
			}
			REG_IE = 1;
			if (boot_failed != 0) {
				AudioInitialize();
				AudioPlay(UI_SOUND_ERROR);
				UiFeedbackStart(&feedback, UI_FEEDBACK_ERROR, 0);
			}
			redraw = TRUE;
			continue;
		}

		if (key_event == 0) {
			if (UiMotionAdvance(&motion)) redraw = TRUE;
			if (UiFeedbackAdvance(&feedback)) redraw = TRUE;
			if (roms_total > 0 && UiTitleScrollAdvance(&title_scroll, &sItemConfig)) redraw = TRUE;
			continue;
		}

		if (boot_failed != 0) {
			AudioPlay(UI_SOUND_BACK);
			UiFeedbackStart(&feedback, UI_FEEDBACK_BACK, 0);
			pending_action = MENU_ACTION_RESET;
			redraw = TRUE;
			continue;
		}

		if ((key_event & KEY_A) || (key_event & KEY_START)) {
			AudioPlay(UI_SOUND_CONFIRM);
			UiFeedbackStart(&feedback, UI_FEEDBACK_CONFIRM, 0);
			pending_skip_save = (kHeld & KEY_SELECT) != 0;
			pending_action = MENU_ACTION_LAUNCH;
			redraw = TRUE;
		} else if (key_event & KEY_B) {
			AudioPlay(UI_SOUND_BACK);
			UiFeedbackStart(&feedback, UI_FEEDBACK_BACK, 0);
			pending_action = MENU_ACTION_RESET;
			redraw = TRUE;
		} else if (navigation_event & KEY_LEFT) {
			u16 previous_index = selected_index;
			selected_index = selected_index == 0 ? roms_total - 1 : selected_index - 1;
			UiMotionSelect(&motion, previous_index, selected_index, roms_total, -1, previous_index == 0);
			UiTitleScrollInitialize(&title_scroll);
			PlayNavigationSound((navigation_repeat_event & KEY_LEFT) != 0);
			redraw = TRUE;
		} else if (navigation_event & KEY_RIGHT) {
			u16 previous_index = selected_index;
			selected_index = selected_index + 1 >= roms_total ? 0 : selected_index + 1;
			UiMotionSelect(&motion, previous_index, selected_index, roms_total, 1, selected_index == 0);
			UiTitleScrollInitialize(&title_scroll);
			PlayNavigationSound((navigation_repeat_event & KEY_RIGHT) != 0);
			redraw = TRUE;
		} else if (navigation_event & KEY_UP) {
			u16 next_index = selected_index >= UI_VISIBLE_CARD_COUNT
				? selected_index - UI_VISIBLE_CARD_COUNT
				: 0;
			if (next_index != selected_index) {
				u16 previous_index = selected_index;
				selected_index = next_index;
				UiMotionSelect(&motion, previous_index, selected_index, roms_total, -1, TRUE);
				UiTitleScrollInitialize(&title_scroll);
				PlayNavigationSound((navigation_repeat_event & KEY_UP) != 0);
				redraw = TRUE;
			} else if ((navigation_repeat_event & KEY_UP) == 0) {
				AudioPlay(UI_SOUND_BOUNDARY);
				UiFeedbackStart(&feedback, UI_FEEDBACK_BOUNDARY, -1);
				redraw = TRUE;
			}
		} else if (navigation_event & KEY_DOWN) {
			u16 next_index = selected_index + UI_VISIBLE_CARD_COUNT;
			if (next_index >= roms_total) next_index = roms_total - 1;
			if (next_index != selected_index) {
				u16 previous_index = selected_index;
				selected_index = next_index;
				UiMotionSelect(&motion, previous_index, selected_index, roms_total, 1, TRUE);
				UiTitleScrollInitialize(&title_scroll);
				PlayNavigationSound((navigation_repeat_event & KEY_DOWN) != 0);
				redraw = TRUE;
			} else if ((navigation_repeat_event & KEY_DOWN) == 0) {
				AudioPlay(UI_SOUND_BOUNDARY);
				UiFeedbackStart(&feedback, UI_FEEDBACK_BOUNDARY, 1);
				redraw = TRUE;
			}
		}
	}
}
