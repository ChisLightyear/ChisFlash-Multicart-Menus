#include <gba.h>
#include <gba_compression.h>
#include <stdio.h>
#include <string.h>

#include "font.h"
#include "ui.h"

const char GbaSimplifiedNsSignature[] = "SNSF";

#define UI_PAGE_0 ((volatile u16 *)AGB_VRAM)
#define UI_PAGE_1 ((volatile u16 *)((u8 *)AGB_VRAM + 0xA000))

#define UI_COLOR_BACKGROUND 0
#define UI_COLOR_SURFACE 1
#define UI_COLOR_DIVIDER 2
#define UI_COLOR_SHADOW 3
#define UI_COLOR_SELECTION 4
#define UI_COLOR_READY 5
#define UI_COLOR_BUSY 6
#define UI_COLOR_ERROR 7
#define UI_COLOR_CARD_FIRST 16
#define UI_COLOR_TEXT_LIGHT 244
#define UI_COLOR_TEXT_DARK 254

#define UI_CARD_WIDTH 56
#define UI_CARD_HEIGHT 56
#define UI_CARD_GAP 1
#define UI_CARD_X 7
#define UI_CARD_Y 44
#define UI_CARD_PITCH (UI_CARD_WIDTH + UI_CARD_GAP)
#define UI_TITLE_VIEW_WIDTH (UI_CARD_WIDTH * 2)
#define UI_TITLE_SIDE_WIDTH ((UI_TITLE_VIEW_WIDTH - UI_CARD_WIDTH) / 2)
#define UI_TITLE_Y 22
#define UI_TITLE_SCROLL_DELAY 60
#define UI_TITLE_SCROLL_GAP 18
#define UI_TITLE_SCROLL_FRAME_DIVISOR 2
#define UI_CARD_PIXEL_COUNT (UI_CARD_WIDTH * UI_CARD_HEIGHT)
#define UI_COVER_TAIL_TRAILER_OFFSET 0x0AFFF0
#define UI_COVER_TAIL_MAGIC 0x54564347
#define UI_COVER_PACK_MAGIC 0x564F4347
#define UI_COVER_FORMAT_VERSION 1
#define UI_COVER_SLOT_FORMAT_VERSION 2
#define UI_COVER_FLAG_PRESENT 0x01
#define UI_COVER_ENTRY_FLAG_LZ77 0x01
#define UI_COVER_PALETTE_FIRST 32
#define UI_COVER_PALETTE_LIMIT 240
#define UI_COVER_ROM_BASE ((const u8 *)0x08000000)
#define UI_COVER_CACHE_SLOTS 7
#define UI_SAFE_SLOT_LEFT 1
#define UI_SAFE_SLOT_RIGHT 2
#define UI_MOTION_FINAL_STEP 3
#define UI_PAGE_ENTRY_OFFSET 12
#define UI_FEEDBACK_FINAL_STEP 3
#define UI_CONFIRM_FEEDBACK_FINAL_STEP 5
#define UI_BACK_FEEDBACK_FINAL_STEP 6
#define UI_ERROR_FEEDBACK_FINAL_STEP 5

#define UI_SCREEN_WORD_COUNT ((SCREEN_WIDTH * SCREEN_HEIGHT) / sizeof(u32))

static const u8 card_colors[7] = {
	UI_COLOR_CARD_FIRST + 0,
	UI_COLOR_CARD_FIRST + 1,
	UI_COLOR_CARD_FIRST + 2,
	UI_COLOR_CARD_FIRST + 3,
	UI_COLOR_CARD_FIRST + 4,
	UI_COLOR_CARD_FIRST + 5,
	UI_COLOR_CARD_FIRST + 6
};

typedef struct UiCoverCache_ {
	u16 cover_index;
	BOOL valid;
	u32 stamp;
	u8 pixels[UI_CARD_PIXEL_COUNT];
} UiCoverCache;

EWRAM_BSS static UiCoverCache sCoverCache[UI_COVER_CACHE_SLOTS] ALIGN(4);
static u32 sCoverCacheStamp;
static const u8 *sCoverPack;
static CoverPackHeader sCoverHeader;
static u32 sCoverPackSize;
static BOOL sCoverUsesEntrySlots;

static const u16 digit_pixels[10] = {
	0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9,
	0x79CF, 0x79EF, 0x7292, 0x7BEF, 0x7BCF
};

static volatile u16 *UiBackBuffer(void) {
	return (REG_DISPCNT & 0x0010) ? UI_PAGE_0 : UI_PAGE_1;
}

IWRAM_CODE ARM_CODE static void UiBlitCover(volatile u16 *buffer, s16 x, s16 y, const u8 *pixels) {
	s16 left = x < 0 ? 0 : x;
	s16 top = y < 0 ? 0 : y;
	s16 right = x + UI_CARD_WIDTH;
	s16 bottom = y + UI_CARD_HEIGHT;
	if (right > SCREEN_WIDTH) right = SCREEN_WIDTH;
	if (bottom > SCREEN_HEIGHT) bottom = SCREEN_HEIGHT;
	if (left >= right || top >= bottom) return;

	for (s16 screen_y = top; screen_y < bottom; screen_y++) {
		const u8 *source = pixels + (screen_y - y) * UI_CARD_WIDTH + (left - x);
		volatile u16 *destination = buffer + (screen_y * SCREEN_WIDTH + left) / 2;
		s16 width = right - left;
		if (left & 1) {
			*destination = (*destination & 0x00FF) | ((u16)*source << 8);
			destination++;
			source++;
			width--;
		}
		while (width >= 2) {
			*destination++ = source[0] | ((u16)source[1] << 8);
			source += 2;
			width -= 2;
		}
		if (width != 0) *destination = (*destination & 0xFF00) | *source;
	}
}

static void UiClear(volatile u16 *buffer, u8 color) {
	u32 color_word = (u32)color * 0x01010101;
	CpuFastSet(&color_word, (void *)buffer, FILL | COPY32 | UI_SCREEN_WORD_COUNT);
}

IWRAM_CODE ARM_CODE static void UiFillRect(volatile u16 *buffer, s16 x, s16 y, s16 width, s16 height, u8 color) {
	if (x < 0) {
		width += x;
		x = 0;
	}
	if (y < 0) {
		height += y;
		y = 0;
	}
	if (x + width > SCREEN_WIDTH) width = SCREEN_WIDTH - x;
	if (y + height > SCREEN_HEIGHT) height = SCREEN_HEIGHT - y;
	if (width <= 0 || height <= 0) return;

	s16 end_x = x + width;
	u16 color_pair = color | ((u16)color << 8);

	for (s16 row = y; row < y + height; row++) {
		volatile u16 *destination = buffer + row * (SCREEN_WIDTH / 2) + (x >> 1);
		s16 col = x;

		if (col & 1) {
			*destination = (*destination & 0x00FF) | ((u16)color << 8);
			destination++;
			col++;
		}

		while (col + 1 < end_x) {
			*destination++ = color_pair;
			col += 2;
		}

		if (col < end_x) {
			*destination = (*destination & 0xFF00) | color;
		}
	}
}

IWRAM_CODE ARM_CODE static void UiDrawOutline(volatile u16 *buffer, s16 x, s16 y, s16 width, s16 height, u8 thickness, u8 color) {
	UiFillRect(buffer, x, y, width, thickness, color);
	UiFillRect(buffer, x, y + height - thickness, width, thickness, color);
	UiFillRect(buffer, x, y, thickness, height, color);
	UiFillRect(buffer, x + width - thickness, y, thickness, height, color);
}

IWRAM_CODE ARM_CODE static void UiTintTextRect(volatile u16 *buffer, s16 x, s16 y, s16 width, s16 height, u8 color) {
	for (s16 row = y; row < y + height; row++) {
		for (s16 col = x; col < x + width; col++) {
			if (col < 0 || col >= SCREEN_WIDTH || row < 0 || row >= SCREEN_HEIGHT) continue;
			u16 offset = (row * SCREEN_WIDTH + col) >> 1;
			u16 pair = buffer[offset];
			u8 pixel = (col & 1) ? pair >> 8 : pair & 0xFF;
			if (pixel < 250 || pixel > UI_COLOR_TEXT_DARK) continue;
			if (col & 1) buffer[offset] = (pair & 0x00FF) | ((u16)color << 8);
			else buffer[offset] = (pair & 0xFF00) | color;
		}
	}
}

static s32 UiInitialScroll(u16 selected_index, u16 roms_total) {
	if (roms_total <= UI_VISIBLE_CARD_COUNT) return 0;

	u16 first_index = selected_index > 2 ? selected_index - 2 : 0;
	u16 last_window_start = roms_total - UI_VISIBLE_CARD_COUNT;
	if (first_index > last_window_start) first_index = last_window_start;
	return (s32)first_index * UI_CARD_PITCH;
}

static s32 UiScrollTarget(const UiMenuMotion *motion, u16 selected_index, u16 roms_total) {
	if (roms_total <= UI_VISIBLE_CARD_COUNT) return 0;

	s32 first_index = (motion->scroll_x + UI_CARD_PITCH / 2) / UI_CARD_PITCH;
	if ((s32)selected_index < first_index + UI_SAFE_SLOT_LEFT) {
		first_index = selected_index > UI_SAFE_SLOT_LEFT ? selected_index - UI_SAFE_SLOT_LEFT : 0;
	} else if ((s32)selected_index > first_index + UI_SAFE_SLOT_RIGHT) {
		first_index = selected_index - UI_SAFE_SLOT_RIGHT;
	}

	s32 last_window_start = roms_total - UI_VISIBLE_CARD_COUNT;
	if (first_index < 0) first_index = 0;
	if (first_index > last_window_start) first_index = last_window_start;
	return first_index * UI_CARD_PITCH;
}

void UiMotionInitialize(UiMenuMotion *motion, u16 selected_index, u16 roms_total) {
	motion->scroll_x = UiInitialScroll(selected_index, roms_total);
	motion->scroll_start_x = motion->scroll_x;
	motion->scroll_target_x = motion->scroll_x;
	motion->previous_selected_index = selected_index;
	motion->step = UI_MOTION_FINAL_STEP;
	motion->direction = 0;
	motion->active = FALSE;
}

void UiMotionSelect(UiMenuMotion *motion, u16 previous_index, u16 selected_index, u16 roms_total, s8 direction, BOOL page_jump) {
	if (motion->active && motion->direction == direction && !page_jump) {
		motion->scroll_x = motion->scroll_target_x;
	}
	motion->previous_selected_index = previous_index;

	if (roms_total <= UI_VISIBLE_CARD_COUNT) {
		motion->scroll_x = 0;
		motion->scroll_start_x = 0;
		motion->scroll_target_x = 0;
	} else {
		s32 previous_scroll_target_x = motion->scroll_target_x;
		s32 next_scroll_target_x = UiScrollTarget(motion, selected_index, roms_total);
		motion->scroll_target_x = next_scroll_target_x;
		if (page_jump && next_scroll_target_x != previous_scroll_target_x) {
			motion->scroll_x = next_scroll_target_x - direction * UI_PAGE_ENTRY_OFFSET;
		}
		motion->scroll_start_x = motion->scroll_x;
	}
	motion->step = 0;
	motion->direction = direction;
	motion->active = TRUE;
}

BOOL UiMotionAdvance(UiMenuMotion *motion) {
	static const u8 ease_out[UI_MOTION_FINAL_STEP + 1] = { 0, 10, 14, 16 };
	if (!motion->active) return FALSE;

	if (motion->step < UI_MOTION_FINAL_STEP) motion->step++;
	s32 distance = motion->scroll_target_x - motion->scroll_start_x;
	motion->scroll_x = motion->scroll_start_x + distance * ease_out[motion->step] / 16;
	if (motion->step == UI_MOTION_FINAL_STEP) {
		motion->scroll_x = motion->scroll_target_x;
		motion->direction = 0;
		motion->active = FALSE;
	}
	return TRUE;
}

void UiTitleScrollInitialize(UiTitleScroll *scroll) {
	scroll->frame = 0;
	scroll->offset = 0;
}

BOOL UiTitleScrollAdvance(UiTitleScroll *scroll, const ItemConfig *item) {
	LoadFont(item->font);
	u16 title_width = MeasureTextWidth(item->title, item->title_length, font);
	if (title_width <= UI_TITLE_VIEW_WIDTH) return FALSE;
	if (scroll->frame < UI_TITLE_SCROLL_DELAY) {
		scroll->frame++;
		return FALSE;
	}
	scroll->frame++;
	if (((scroll->frame - UI_TITLE_SCROLL_DELAY) % UI_TITLE_SCROLL_FRAME_DIVISOR) != 0) return FALSE;
	scroll->offset++;
	if (scroll->offset >= title_width + UI_TITLE_SCROLL_GAP) UiTitleScrollInitialize(scroll);
	return TRUE;
}

void UiFeedbackInitialize(UiFeedback *feedback) {
	feedback->type = UI_FEEDBACK_NONE;
	feedback->step = 0;
	feedback->direction = 0;
	feedback->active = FALSE;
}

void UiFeedbackStart(UiFeedback *feedback, UiFeedbackType type, s8 direction) {
	feedback->type = type;
	feedback->step = 0;
	feedback->direction = direction;
	feedback->active = type != UI_FEEDBACK_NONE;
}

BOOL UiFeedbackAdvance(UiFeedback *feedback) {
	if (!feedback->active) return FALSE;
	u8 final_step = UI_FEEDBACK_FINAL_STEP;
	if (feedback->type == UI_FEEDBACK_CONFIRM) {
		final_step = UI_CONFIRM_FEEDBACK_FINAL_STEP;
	} else if (feedback->type == UI_FEEDBACK_BACK) {
		final_step = UI_BACK_FEEDBACK_FINAL_STEP;
	} else if (feedback->type == UI_FEEDBACK_ERROR) {
		final_step = UI_ERROR_FEEDBACK_FINAL_STEP;
	}
	if (feedback->step < final_step) feedback->step++;
	if (feedback->step >= final_step) {
		feedback->type = UI_FEEDBACK_NONE;
		feedback->direction = 0;
		feedback->active = FALSE;
	}
	return TRUE;
}

IWRAM_CODE ARM_CODE static void UiDrawNumber(volatile u16 *buffer, s16 center_x, s16 y, u16 number, u8 color) {
	u8 digits[3];
	u8 count = number >= 100 ? 3 : 2;
	digits[0] = number >= 100 ? (number / 100) % 10 : (number / 10) % 10;
	digits[1] = number >= 100 ? (number / 10) % 10 : number % 10;
	digits[2] = number % 10;
	s16 x = center_x - (count * 7 - 1) / 2;

	for (u8 digit = 0; digit < count; digit++) {
		u16 pixels = digit_pixels[digits[digit]];
		for (u8 row = 0; row < 5; row++) {
			for (u8 col = 0; col < 3; col++) {
				if (pixels & (1 << (14 - row * 3 - col))) {
					UiFillRect(buffer, x + col * 2, y + row * 2, 2, 2, color);
				}
			}
		}
		x += 7;
	}
}

static const u8 *UiGetCoverBitmap(u8 cover_index) {
	if (sCoverPack == NULL || cover_index >= sCoverHeader.cover_count) return NULL;

	UiCoverCache *oldest = &sCoverCache[0];
	for (u8 slot_index = 0; slot_index < UI_COVER_CACHE_SLOTS; slot_index++) {
		UiCoverCache *slot = &sCoverCache[slot_index];
		if (slot->valid && slot->cover_index == cover_index) {
			slot->stamp = ++sCoverCacheStamp;
			return slot->pixels;
		}
		if (!slot->valid) {
			oldest = slot;
			break;
		}
		if (slot->stamp < oldest->stamp) oldest = slot;
	}

	CoverPackEntry entry;
	memcpy(
		&entry,
		sCoverPack + sCoverHeader.entries_offset + (u32)cover_index * sCoverHeader.entry_size,
		sizeof(entry)
	);
	if (entry.raw_size != UI_CARD_PIXEL_COUNT ||
		entry.data_offset < sCoverHeader.data_offset ||
		entry.stored_size == 0 ||
		entry.data_offset > sCoverPackSize ||
		entry.stored_size > sCoverPackSize - entry.data_offset) return NULL;

	const u8 *source = sCoverPack + entry.data_offset;
	if ((entry.flags & UI_COVER_ENTRY_FLAG_LZ77) != 0) {
		u32 lz_header;
		memcpy(&lz_header, source, sizeof(lz_header));
		if ((lz_header & 0xFF) != 0x10 || (lz_header >> 8) != UI_CARD_PIXEL_COUNT) return NULL;
		LZ77UnCompWram(source, oldest->pixels);
	} else {
		if (entry.stored_size != UI_CARD_PIXEL_COUNT) return NULL;
		memcpy(oldest->pixels, source, UI_CARD_PIXEL_COUNT);
	}

	oldest->cover_index = cover_index;
	oldest->valid = TRUE;
	oldest->stamp = ++sCoverCacheStamp;
	return oldest->pixels;
}

static const u8 *UiGetItemCover(const ItemConfig *item, u16 entry_slot) {
	u16 cover_index;
	if (item == NULL || sCoverPack == NULL) return NULL;
	if ((item->cover_flags & UI_COVER_FLAG_PRESENT) != 0) {
		cover_index = item->cover_index;
	} else if (sCoverUsesEntrySlots) {
		cover_index = entry_slot;
	} else {
		return NULL;
	}
	if (cover_index >= sCoverHeader.cover_count) return NULL;
	return UiGetCoverBitmap((u8)cover_index);
}

IWRAM_CODE ARM_CODE static void UiDrawCard(volatile u16 *buffer, s16 x, u16 item_index, u16 entry_slot, const ItemConfig *item, BOOL selected, u8 lift, u8 expansion, u8 focus_color) {
	s16 card_x = x - expansion;
	s16 card_y = UI_CARD_Y - lift - expansion;
	u8 card_width = UI_CARD_WIDTH + expansion * 2;
	u8 card_height = UI_CARD_HEIGHT + expansion * 2;
	u8 color = card_colors[item_index % 7];

	UiFillRect(buffer, card_x + 2, card_y + 3, card_width, card_height, UI_COLOR_SHADOW);
	if (selected) {
		UiDrawOutline(buffer, card_x - 3, card_y - 3, card_width + 6, card_height + 6, 2, focus_color);
		UiDrawOutline(buffer, card_x - 1, card_y - 1, card_width + 2, card_height + 2, 1, UI_COLOR_BACKGROUND);
	}
	UiFillRect(buffer, card_x, card_y, card_width, card_height, color);

	BOOL cover_drawn = FALSE;
	if (sCoverPack != NULL) {
		const u8 *cover = UiGetItemCover(item, entry_slot);
		if (cover != NULL) {
			UiBlitCover(buffer, x, UI_CARD_Y - lift, cover);
			cover_drawn = TRUE;
		}
	}

	if (!cover_drawn && card_x >= 0 && card_x + card_width <= SCREEN_WIDTH) {
		BOOL light_text = (item_index % 7) != 1 && (item_index % 7) != 2;
		UiDrawNumber(
			buffer,
			card_x + card_width / 2,
			card_y + 17 + expansion,
			item_index + 1,
			light_text ? UI_COLOR_TEXT_LIGHT : UI_COLOR_TEXT_DARK
		);
	}

}

void UiInitialize(void) {
	SetMode(MODE_4 | BG2_ENABLE);
	sCoverPack = NULL;
	sCoverPackSize = 0;
	memset(&sCoverHeader, 0, sizeof(sCoverHeader));
	memset(sCoverCache, 0, sizeof(sCoverCache));
	sCoverCacheStamp = 0;
	sCoverUsesEntrySlots = FALSE;

	BG_PALETTE[UI_COLOR_BACKGROUND] = RGB555(0xF7, 0xF8, 0xFA);
	BG_PALETTE[UI_COLOR_SURFACE] = RGB555(0xFF, 0xFF, 0xFF);
	BG_PALETTE[UI_COLOR_DIVIDER] = RGB555(0xD8, 0xDB, 0xE0);
	BG_PALETTE[UI_COLOR_SHADOW] = RGB555(0xD9, 0xDC, 0xE2);
	BG_PALETTE[UI_COLOR_SELECTION] = RGB555(0x18, 0x9A, 0xD3);
	BG_PALETTE[UI_COLOR_READY] = RGB555(0x42, 0xA9, 0x6B);
	BG_PALETTE[UI_COLOR_BUSY] = RGB555(0xE3, 0xB3, 0x41);
	BG_PALETTE[UI_COLOR_ERROR] = RGB555(0xE7, 0x4C, 0x5B);

	BG_PALETTE[UI_COLOR_CARD_FIRST + 0] = RGB555(0xE7, 0x4C, 0x5B);
	BG_PALETTE[UI_COLOR_CARD_FIRST + 1] = RGB555(0xF0, 0x8C, 0x46);
	BG_PALETTE[UI_COLOR_CARD_FIRST + 2] = RGB555(0xE3, 0xB3, 0x41);
	BG_PALETTE[UI_COLOR_CARD_FIRST + 3] = RGB555(0x42, 0xA9, 0x6B);
	BG_PALETTE[UI_COLOR_CARD_FIRST + 4] = RGB555(0x2F, 0xA9, 0xB8);
	BG_PALETTE[UI_COLOR_CARD_FIRST + 5] = RGB555(0x4D, 0x77, 0xD0);
	BG_PALETTE[UI_COLOR_CARD_FIRST + 6] = RGB555(0x8A, 0x5C, 0xC8);

	for (u8 i = 240; i <= 244; i++) BG_PALETTE[i] = RGB555(0xFF, 0xFF, 0xFF);
	BG_PALETTE[250] = RGB555(0x62, 0x66, 0x6D);
	BG_PALETTE[251] = RGB555(0x4B, 0x4F, 0x56);
	BG_PALETTE[252] = RGB555(0x35, 0x38, 0x3E);
	BG_PALETTE[253] = RGB555(0x23, 0x25, 0x29);
	BG_PALETTE[254] = RGB555(0x18, 0x1A, 0x1E);
	BG_PALETTE[255] = RGB555(0xF7, 0xF8, 0xFA);

	CoverTailTrailer trailer;
	memcpy(&trailer, UI_COVER_ROM_BASE + UI_COVER_TAIL_TRAILER_OFFSET, sizeof(trailer));
	if (trailer.magic == UI_COVER_TAIL_MAGIC &&
		trailer.version == UI_COVER_FORMAT_VERSION &&
		trailer.size == sizeof(CoverTailTrailer) &&
		trailer.pack_offset < UI_COVER_TAIL_TRAILER_OFFSET &&
		trailer.pack_size == UI_COVER_TAIL_TRAILER_OFFSET - trailer.pack_offset) {
		const u8 *pack = UI_COVER_ROM_BASE + trailer.pack_offset;
		CoverPackHeader header;
		memcpy(&header, pack, sizeof(header));
		if (header.magic == UI_COVER_PACK_MAGIC &&
			(header.version == UI_COVER_FORMAT_VERSION ||
			 header.version == UI_COVER_SLOT_FORMAT_VERSION) &&
			header.header_size == sizeof(CoverPackHeader) &&
			header.width == UI_CARD_WIDTH && header.height == UI_CARD_HEIGHT &&
			header.entry_size == sizeof(CoverPackEntry) && header.cover_count > 0 &&
			header.palette_base >= UI_COVER_PALETTE_FIRST &&
			header.palette_count > 0 &&
			header.palette_base + header.palette_count <= UI_COVER_PALETTE_LIMIT &&
			header.palette_offset >= header.header_size &&
			header.palette_offset + (u32)header.palette_count * sizeof(u16) <= header.entries_offset &&
			header.entries_offset + (u32)header.cover_count * header.entry_size <= header.data_offset &&
			header.data_offset <= trailer.pack_size) {
			sCoverPack = pack;
			sCoverHeader = header;
			sCoverPackSize = trailer.pack_size;
			sCoverUsesEntrySlots = header.version == UI_COVER_SLOT_FORMAT_VERSION;
			memcpy(
				&BG_PALETTE[header.palette_base],
				pack + header.palette_offset,
				(u32)header.palette_count * sizeof(u16)
			);
			for (u16 cover_index = 0;
				cover_index < header.cover_count && cover_index < UI_COVER_CACHE_SLOTS;
				cover_index++) {
				UiGetCoverBitmap(cover_index);
			}
		}
	}

	UiClear(UI_PAGE_0, UI_COLOR_BACKGROUND);
	UiClear(UI_PAGE_1, UI_COLOR_BACKGROUND);
}

IWRAM_CODE ARM_CODE void UiRenderMenu(const UiMenuView *view) {
	volatile u16 *buffer = UiBackBuffer();
	UiClear(buffer, UI_COLOR_BACKGROUND);
	UiFillRect(buffer, 0, 124, SCREEN_WIDTH, 36, UI_COLOR_SURFACE);
	UiFillRect(buffer, 0, 123, SCREEN_WIDTH, 1, UI_COLOR_DIVIDER);

	LoadFont(0);
	DrawText(11, 7, ALIGN_LEFT, u"CHIS", 4, font, buffer, FALSE);

	char position_ascii[16];
	u16 position_unicode[16] = { 0 };
	snprintf(
		position_ascii,
		sizeof(position_ascii),
		"%03u/%03u",
		(unsigned int)(view->roms_total == 0 ? 0 : view->selected_index + 1),
		(unsigned int)view->roms_total
	);
	AsciiToUnicode(position_ascii, position_unicode);
	DrawText(10, 7, ALIGN_RIGHT, position_unicode, strlen(position_ascii), font, buffer, FALSE);

	if (view->roms_total > 0) {
		ItemConfig selected_item;
		memcpy(
			&selected_item,
			view->itemlist + view->itemlist_offset + ITEM_CONFIG_SIZE * view->selected_index,
			sizeof(selected_item)
		);
		s32 scroll_x = view->motion != NULL ? view->motion->scroll_x : UiInitialScroll(view->selected_index, view->roms_total);
		s16 selected_x = UI_CARD_X + (s32)view->selected_index * UI_CARD_PITCH - scroll_x;
		s16 title_left = selected_x - UI_TITLE_SIDE_WIDTH;
		s16 title_right = title_left + UI_TITLE_VIEW_WIDTH;
		LoadFont(selected_item.font);
		u16 title_width = MeasureTextWidth(selected_item.title, selected_item.title_length, font);
		if (title_width <= UI_TITLE_VIEW_WIDTH) {
			s16 title_x = title_left + (UI_TITLE_VIEW_WIDTH - title_width) / 2;
			DrawTextClipped(title_x, UI_TITLE_Y, selected_item.title, selected_item.title_length, font, buffer, title_left, title_right, FALSE);
		} else {
			u16 title_offset = view->title_scroll != NULL ? view->title_scroll->offset : 0;
			s16 first_x = title_left - title_offset;
			DrawTextClipped(first_x, UI_TITLE_Y, selected_item.title, selected_item.title_length, font, buffer, title_left, title_right, FALSE);
			DrawTextClipped(first_x + title_width + UI_TITLE_SCROLL_GAP, UI_TITLE_Y, selected_item.title, selected_item.title_length, font, buffer, title_left, title_right, FALSE);
		}
		UiTintTextRect(buffer, title_left, UI_TITLE_Y, UI_TITLE_VIEW_WIDTH, GetFontHeight(), UI_COLOR_SELECTION);

		s32 first_index = scroll_x / UI_CARD_PITCH;
		if (first_index > 0) first_index--;
		if (first_index < 0) first_index = 0;
		u16 final_index = first_index + UI_VISIBLE_CARD_COUNT + 2;
		if (final_index > view->roms_total) final_index = view->roms_total;

		// The selected cover is drawn last so its enlarged right edge is never hidden.
		for (u8 layer = 0; layer < 2; layer++) {
			for (u16 item_index = first_index; item_index < final_index; item_index++) {
				s16 x = UI_CARD_X + (s32)item_index * UI_CARD_PITCH - scroll_x;
				if (x >= SCREEN_WIDTH || x + UI_CARD_WIDTH <= 0) continue;

				BOOL selected = item_index == view->selected_index;
				if ((layer == 0 && selected) || (layer == 1 && !selected)) continue;
				u8 lift = 0;
				u8 expansion = 0;
				u8 focus_color = UI_COLOR_SELECTION;
				if (selected) {
					u8 step = view->motion != NULL ? view->motion->step : UI_MOTION_FINAL_STEP;
					lift = step == 0 ? 1 : (step == 1 ? 2 : 3);
					expansion = step >= 2 ? 1 : 0;
				} else if (view->motion != NULL && view->motion->active && item_index == view->motion->previous_selected_index) {
					lift = view->motion->step == 0 ? 2 : (view->motion->step == 1 ? 1 : 0);
				}
				if (selected && view->feedback != NULL && view->feedback->active) {
					if (view->feedback->type == UI_FEEDBACK_CONFIRM) {
						u8 step = view->feedback->step;
						lift = step <= 1 ? 0 : (step == 2 ? 1 : 2);
						expansion = step >= 4 ? 1 : 0;
					} else if (view->feedback->type == UI_FEEDBACK_BACK) {
						lift = view->feedback->step == 0 ? 2 : (view->feedback->step == 1 ? 1 : 0);
						expansion = 0;
						focus_color = UI_COLOR_DIVIDER;
					} else if (view->feedback->type == UI_FEEDBACK_BOUNDARY) {
						static const s8 nudge[3] = { 2, -1, 0 };
						u8 step = view->feedback->step > 2 ? 2 : view->feedback->step;
						x += nudge[step] * view->feedback->direction;
					} else if (view->feedback->type == UI_FEEDBACK_ERROR) {
						static const s8 shake[5] = { -2, 2, -1, 1, 0 };
						u8 step = view->feedback->step > 4 ? 4 : view->feedback->step;
						x += shake[step];
						focus_color = UI_COLOR_ERROR;
					}
				}
				ItemConfig card_item;
				memcpy(
					&card_item,
					view->itemlist + view->itemlist_offset + ITEM_CONFIG_SIZE * item_index,
					sizeof(card_item)
				);
				UiDrawCard(
					buffer,
					x,
					item_index,
					view->itemlist_offset / ITEM_CONFIG_SIZE + item_index,
					&card_item,
					selected,
					lift,
					expansion,
					focus_color
				);
			}
		}

		char save_ascii[32];
		u16 save_unicode[16] = { 0 };
		if (selected_item.save_type == SRAM_NONE) {
			snprintf(save_ascii, sizeof(save_ascii), "无存档");
		} else {
			snprintf(save_ascii, sizeof(save_ascii), "存档 %02u", (unsigned int)selected_item.save_index + 1);
		}
		u8 save_length = Utf8ToUnicode(save_ascii, save_unicode, 16);
		LoadFont(0);
		DrawText(11, 139, ALIGN_LEFT, save_unicode, save_length, font, buffer, FALSE);
	} else {
		if (view->status_tone == UI_STATUS_ERROR && view->status_text != NULL && view->status_length > 0) {
			DrawText(11, 27, ALIGN_LEFT, (u16 *)view->status_text, view->status_length, font, buffer, FALSE);
		} else {
			DrawText(11, 27, ALIGN_LEFT, u"暂无游戏", 4, font, buffer, FALSE);
		}
	}

	u8 tone_color = UI_COLOR_READY;
	if (view->status_tone == UI_STATUS_BUSY) tone_color = UI_COLOR_BUSY;
	if (view->status_tone == UI_STATUS_ERROR) tone_color = UI_COLOR_ERROR;
	UiFillRect(buffer, 225, 141, 4, 4, tone_color);
	if (view->status_text != NULL && view->status_length > 0) {
		LoadFont(0);
		DrawText(17, 139, ALIGN_RIGHT, (u16 *)view->status_text, view->status_length, font, buffer, FALSE);
	}
}

void UiPresent(void) {
	REG_DISPCNT ^= 0x0010;
}
