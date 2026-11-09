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
EWRAM_BSS static u16 sBucketOwners[WL_BUCKET_COUNT];
static u16 sBucketCount;

static volatile u16 sInputHeld;
static volatile u16 sInputPressed;
static volatile u16 sNavigationEvents;
static volatile u16 sNavigationRepeatEvents;
static u8 sNavigationRepeatTimer;

static void MenuWakeInterrupt(void) {
}

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
	if (sFlashStatus.settings_magic != MAGIC_MENU_SETTINGS) {
		sFlashStatus.settings_magic = MAGIC_MENU_SETTINGS;
		sFlashStatus.settings_flags = MENU_SETTINGS_DEFAULT;
	}
	UiSetLanguage((sFlashStatus.settings_flags & MENU_SETTING_ENGLISH) != 0);
}

static void MenuEnterSleep(void) {
	AudioShutdown();
	REG_DISPCNT |= 0x0080;
	REG_KEYCNT = KEY_START | KEYIRQ_ENABLE | KEYIRQ_AND;
	REG_IF = IRQ_KEYPAD;
	irqEnable(IRQ_KEYPAD);
	Stop();
	irqDisable(IRQ_KEYPAD);
	REG_KEYCNT = 0;
	REG_DISPCNT &= ~0x0080;
	AudioInitialize();
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
	SetStatusText(status_ascii, status_unicode, message, &status_length);
	UiMotionInitialize(&motion, 0, 0);
	UiFeedbackInitialize(&feedback);
	UiMenuView view = {
		.itemlist = NULL,
		.itemlist_offset = 0,
		.roms_total = 0,
		.selected_index = 0,
		.status_text = status_unicode,
		.status_length = status_length,
		.status_tone = UI_STATUS_ERROR,
		.motion = &motion,
		.feedback = &feedback
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
			SetStatusText(ascii, unicode, UiLanguageText("不支持此卡带", "UNSUPPORTED CART"), length);
		} else if (boot_failed == BOOT_GAME_ERR_MAPPER_NO_RESPONSE) {
			SetStatusText(ascii, unicode, UiLanguageText("映射器无响应", "MAPPER ERROR"), length);
		} else if (boot_failed == BOOT_GAME_ERR_WL_METADATA) {
			SetStatusText(ascii, unicode, UiLanguageText("缺少映射表", "MAP MISSING"), length);
		} else if (boot_failed == BOOT_GAME_ERR_WL_RECORD) {
			SetStatusText(ascii, unicode, UiLanguageText("映射记录无效", "INVALID MAP"), length);
		} else if (boot_failed == BOOT_GAME_ERR_IMAGE_METADATA) {
			SetStatusText(ascii, unicode, UiLanguageText("镜像信息错误", "IMAGE ERROR"), length);
		} else if (boot_failed == BOOT_GAME_ERR_ENTRY_RANGE) {
			SetStatusText(ascii, unicode, UiLanguageText("游戏目录错误", "ENTRY ERROR"), length);
		} else {
			SetStatusText(ascii, unicode, UiLanguageText("启动失败", "LAUNCH ERROR"), length);
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
				snprintf(ascii, 64, UiLanguageText("条目%03u 映射错误", "E%03u MAP ERROR"), (unsigned int)entry_slot);
				tone = UI_STATUS_ERROR;
			}
		}
		memset(unicode, 0, 64 * sizeof(u16));
		*length = Utf8ToUnicode(ascii, unicode, 64);
	} else {
		SetStatusText(ascii, unicode, UiLanguageText("系统就绪", "SYSTEM READY"), length);
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
	u16 title_scroll_index = 0xFFFF;
	MenuPendingAction pending_action = MENU_ACTION_NONE;
	BOOL pending_skip_save = FALSE;
	BOOL show_debug = FALSE;
	BOOL show_credits = FALSE;
	BOOL toolbar_focused = FALSE;
	u8 toolbar_selected = 0;
	u8 toolbar_press_step = 0;
	UiToolPage tool_page = UI_TOOL_NONE;
	u8 settings_row = 0;
	u8 map_scroll_row = 0;
	BOOL settings_dirty = FALSE;
	BOOL redraw = TRUE;
	u8 boot_failed = 0;

	irqInit();
	irqSet(IRQ_KEYPAD, MenuWakeInterrupt);
	// Finish NOR command-mode detection before enabling Direct Sound DMA.
	CartridgeInitialize();
	AudioInitialize();
	irqSet(IRQ_VBLANK, MenuVBlankInput);
	irqEnable(IRQ_VBLANK);

	UiInitialize();
	const CartridgeProfile *cartridge = CartridgeGetProfile();
	if (cartridge->metadata_error) ShowFatalMenu(UiLanguageText("镜像信息错误", "IMAGE ERROR"));
	if (cartridge->backend == CART_BACKEND_NONE || cartridge->flash_type == 0) ShowFatalMenu(UiLanguageText("卡带识别失败", "CART ERROR"));
	u16 image_entry_count = cartridge->entry_count;
	ReadCartridgeBucketOwners(sBucketOwners, WL_BUCKET_COUNT, &sBucketCount);

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
		.title_scroll = &title_scroll,
		.toolbar_focused = FALSE,
		.toolbar_selected = 0,
		.toolbar_pressed = FALSE
	};

	if (roms_total == 0) {
		SetStatusText(status_ascii, status_unicode, UiLanguageText("请先用合卡器添加游戏", "USE ROM BUILDER"), &status_length);
		view.status_length = status_length;
		UiRenderMenu(&view);
		MenuWaitForVBlank();
		UiPresent();
		while (1) MenuWaitForVBlank();
	}

	LoadFlashStatusOrDefaults(roms_total);
	AudioSetEnabled((sFlashStatus.settings_flags & MENU_SETTING_SOUND) != 0);
	if ((sFlashStatus.settings_flags & MENU_SETTING_RESUME) != 0 && sFlashStatus.last_boot_menu_index != 0xFFFF) {
		selected_index = sFlashStatus.last_boot_menu_index;
	}
	UiMotionInitialize(&motion, selected_index, roms_total);

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
			if (tool_page != UI_TOOL_NONE) {
				u16 entry_slot = GetAbsoluteEntrySlot(itemlist_offset, selected_index);
				UiToolView tool_view = {
					.page = tool_page,
					.item = &sItemConfig,
					.selected_index = selected_index,
					.roms_total = roms_total,
					.entry_slot = entry_slot,
					.bucket_owners = sBucketCount != 0 ? sBucketOwners : NULL,
					.bucket_count = sBucketCount,
					.map_scroll_row = map_scroll_row,
					.cartridge = cartridge,
					.held_keys = sInputHeld,
					.row = settings_row,
					.settings_flags = sFlashStatus.settings_flags,
				};
				UiRenderToolPage(&tool_view);
			} else {
				if (title_scroll_index != selected_index) {
					UiTitleScrollInitialize(&title_scroll);
					title_scroll_index = selected_index;
				}
				view.selected_index = selected_index;
				view.toolbar_focused = toolbar_focused;
				view.toolbar_selected = toolbar_selected;
				view.toolbar_pressed = toolbar_press_step != 0;
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
		if (tool_page != UI_TOOL_NONE) {
			if (tool_page == UI_TOOL_CONTROLLER) {
				redraw = TRUE;
				if ((kHeld & (KEY_L | KEY_R)) == (KEY_L | KEY_R)) {
					AudioPlay(UI_SOUND_BACK);
					tool_page = UI_TOOL_NONE;
					redraw = TRUE;
				}
			} else if (tool_page == UI_TOOL_MAP) {
				if (key_event & KEY_B) {
					AudioPlay(UI_SOUND_BACK);
					tool_page = UI_TOOL_NONE;
					redraw = TRUE;
				} else if (navigation_event & (KEY_UP | KEY_DOWN)) {
					u8 previous_row = map_scroll_row;
					u8 max_row = UiMapMaxScrollRow(sBucketCount);
					if ((navigation_event & KEY_UP) && map_scroll_row > 0) map_scroll_row--;
					else if ((navigation_event & KEY_DOWN) && map_scroll_row < max_row) map_scroll_row++;
					if (map_scroll_row != previous_row) {
						PlayNavigationSound((navigation_repeat_event & MENU_NAV_KEYS) != 0);
						redraw = TRUE;
					} else if ((navigation_repeat_event & MENU_NAV_KEYS) == 0 && max_row != 0) {
						AudioPlay(UI_SOUND_BOUNDARY);
					}
				}
			} else if (tool_page == UI_TOOL_SETTINGS) {
				if (key_event & KEY_B) {
					AudioPlay(UI_SOUND_BACK);
					if (settings_dirty) {
						AudioShutdown();
						FlashStoreStatus(sFlashStatus);
						AudioInitialize();
						AudioSetEnabled((sFlashStatus.settings_flags & MENU_SETTING_SOUND) != 0);
						settings_dirty = FALSE;
					}
					tool_page = UI_TOOL_NONE;
					redraw = TRUE;
				} else if (navigation_event & KEY_UP) {
					settings_row = settings_row == 0 ? MENU_SETTINGS_COUNT - 1 : settings_row - 1;
					PlayNavigationSound((navigation_repeat_event & KEY_UP) != 0);
					redraw = TRUE;
				} else if (navigation_event & KEY_DOWN) {
					settings_row = settings_row + 1 >= MENU_SETTINGS_COUNT ? 0 : settings_row + 1;
					PlayNavigationSound((navigation_repeat_event & KEY_DOWN) != 0);
					redraw = TRUE;
				} else if ((key_event & KEY_A) || (navigation_event & (KEY_LEFT | KEY_RIGHT))) {
					static const u8 setting_masks[MENU_SETTINGS_COUNT] = { MENU_SETTING_SOUND, MENU_SETTING_MOTION, MENU_SETTING_WRAP, MENU_SETTING_RESUME, MENU_SETTING_ENGLISH };
					sFlashStatus.settings_flags ^= setting_masks[settings_row];
					UiSetLanguage((sFlashStatus.settings_flags & MENU_SETTING_ENGLISH) != 0);
					settings_dirty = TRUE;
					AudioSetEnabled((sFlashStatus.settings_flags & MENU_SETTING_SOUND) != 0);
					AudioPlay(UI_SOUND_CONFIRM);
					redraw = TRUE;
				}
			} else if (tool_page == UI_TOOL_DETAILS) {
				if (key_event & KEY_A) {
					AudioPlay(UI_SOUND_CONFIRM);
					tool_page = UI_TOOL_NONE;
					toolbar_focused = FALSE;
					UiFeedbackStart(&feedback, UI_FEEDBACK_CONFIRM, 0);
					pending_skip_save = (kHeld & KEY_SELECT) != 0;
					pending_action = MENU_ACTION_LAUNCH;
					redraw = TRUE;
				} else if (key_event & KEY_B) {
					AudioPlay(UI_SOUND_BACK);
					tool_page = UI_TOOL_NONE;
					toolbar_focused = FALSE;
					UiMotionInitialize(&motion, selected_index, roms_total);
					redraw = TRUE;
				} else if (navigation_event & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)) {
					u16 next_index = selected_index;
					if (navigation_event & KEY_LEFT) {
						if (selected_index > 0) next_index--;
						else if (sFlashStatus.settings_flags & MENU_SETTING_WRAP) next_index = roms_total - 1;
					} else if (navigation_event & KEY_RIGHT) {
						if (selected_index + 1 < roms_total) next_index++;
						else if (sFlashStatus.settings_flags & MENU_SETTING_WRAP) next_index = 0;
					} else if (navigation_event & KEY_UP) {
						next_index = selected_index >= 5 ? selected_index - 5 : 0;
					} else {
						next_index = selected_index + 5 < roms_total ? selected_index + 5 : roms_total - 1;
					}
					if (next_index != selected_index) {
						selected_index = next_index;
						UiMotionInitialize(&motion, selected_index, roms_total);
						PlayNavigationSound((navigation_repeat_event & MENU_NAV_KEYS) != 0);
					} else if ((navigation_repeat_event & MENU_NAV_KEYS) == 0) {
						AudioPlay(UI_SOUND_BOUNDARY);
					}
					redraw = TRUE;
				}
			} else if (tool_page == UI_TOOL_SLEEP && (key_event & KEY_A)) {
				MenuEnterSleep();
				AudioSetEnabled((sFlashStatus.settings_flags & MENU_SETTING_SOUND) != 0);
				tool_page = UI_TOOL_NONE;
				redraw = TRUE;
			} else if (key_event & KEY_B) {
				AudioPlay(UI_SOUND_BACK);
				tool_page = UI_TOOL_NONE;
				redraw = TRUE;
			}
			continue;
		}
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
			SetStatusText(status_ascii, status_unicode, UiLanguageText("正在加载", "LOADING"), &status_length);
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
			if (tool_page == UI_TOOL_NONE && !toolbar_focused && !motion.active && !feedback.active &&
				UiTitleScrollAdvance(&title_scroll, &sItemConfig)) redraw = TRUE;
			if (toolbar_press_step != 0) {
				toolbar_press_step++;
				if (toolbar_press_step >= 6) toolbar_press_step = 0;
				redraw = TRUE;
			}
			continue;
		}

		if (boot_failed != 0) {
			AudioPlay(UI_SOUND_BACK);
			UiFeedbackStart(&feedback, UI_FEEDBACK_BACK, 0);
			pending_action = MENU_ACTION_RESET;
			redraw = TRUE;
			continue;
		}

		if (toolbar_focused) {
			if (key_event & KEY_A) {
				AudioPlay(UI_SOUND_CONFIRM);
				toolbar_press_step = 1;
				tool_page = (UiToolPage)(toolbar_selected + 1);
				map_scroll_row = 0;
				redraw = TRUE;
			} else if ((key_event & KEY_B) || (navigation_event & KEY_UP)) {
				AudioPlay((key_event & KEY_B) ? UI_SOUND_BACK : UI_SOUND_MOVE);
				toolbar_focused = FALSE;
				toolbar_press_step = 0;
				redraw = TRUE;
			} else if (navigation_event & KEY_LEFT) {
				toolbar_selected = toolbar_selected == 0 ? 5 : toolbar_selected - 1;
				PlayNavigationSound((navigation_repeat_event & KEY_LEFT) != 0);
				redraw = TRUE;
			} else if (navigation_event & KEY_RIGHT) {
				toolbar_selected = toolbar_selected >= 5 ? 0 : toolbar_selected + 1;
				PlayNavigationSound((navigation_repeat_event & KEY_RIGHT) != 0);
				redraw = TRUE;
			}
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
			if (selected_index == 0 && (sFlashStatus.settings_flags & MENU_SETTING_WRAP) == 0) {
				AudioPlay(UI_SOUND_BOUNDARY);
				UiFeedbackStart(&feedback, UI_FEEDBACK_BOUNDARY, -1);
				redraw = TRUE;
				continue;
			}
			selected_index = selected_index == 0 ? roms_total - 1 : selected_index - 1;
			UiMotionSelect(&motion, previous_index, selected_index, roms_total, -1, previous_index == 0);
			if ((sFlashStatus.settings_flags & MENU_SETTING_MOTION) == 0) UiMotionInitialize(&motion, selected_index, roms_total);
			PlayNavigationSound((navigation_repeat_event & KEY_LEFT) != 0);
			redraw = TRUE;
		} else if (navigation_event & KEY_RIGHT) {
			u16 previous_index = selected_index;
			if (selected_index + 1 >= roms_total && (sFlashStatus.settings_flags & MENU_SETTING_WRAP) == 0) {
				AudioPlay(UI_SOUND_BOUNDARY);
				UiFeedbackStart(&feedback, UI_FEEDBACK_BOUNDARY, 1);
				redraw = TRUE;
				continue;
			}
			selected_index = selected_index + 1 >= roms_total ? 0 : selected_index + 1;
			UiMotionSelect(&motion, previous_index, selected_index, roms_total, 1, selected_index == 0);
			if ((sFlashStatus.settings_flags & MENU_SETTING_MOTION) == 0) UiMotionInitialize(&motion, selected_index, roms_total);
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
				PlayNavigationSound((navigation_repeat_event & KEY_UP) != 0);
				redraw = TRUE;
			} else if ((navigation_repeat_event & KEY_UP) == 0) {
				AudioPlay(UI_SOUND_BOUNDARY);
				UiFeedbackStart(&feedback, UI_FEEDBACK_BOUNDARY, -1);
				redraw = TRUE;
			}
		} else if (navigation_event & KEY_DOWN) {
			toolbar_focused = TRUE;
			toolbar_press_step = 0;
			PlayNavigationSound((navigation_repeat_event & KEY_DOWN) != 0);
			redraw = TRUE;
		}
	}
}
