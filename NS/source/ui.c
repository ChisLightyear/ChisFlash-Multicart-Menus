#include <gba.h>
#include <gba_compression.h>
#include <stdio.h>
#include <string.h>

#include "font.h"
#include "ui.h"
#include "nor_info.h"

static BOOL sUiEnglish;

void UiSetLanguage(BOOL english) {
	sUiEnglish = english;
}

const char *UiLanguageText(const char *chinese, const char *english) {
	return sUiEnglish ? english : chinese;
}

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
#define UI_COLOR_CHEESE_EDGE 8
#define UI_COLOR_CHEESE 9
#define UI_COLOR_CHEESE_HOLE 10
#define UI_COLOR_TOOLBAR_EDGE_SOFT 11
#define UI_COLOR_ICON_RED_SOFT 12
#define UI_COLOR_ICON_YELLOW_SOFT 13
#define UI_COLOR_ICON_BLUE_SOFT 14
#define UI_COLOR_ICON_DARK_SOFT 15
#define UI_COLOR_CARD_FIRST 16
#define UI_COLOR_TEXT_LIGHT 244
#define UI_COLOR_TEXT_DARK 254

#define UI_CARD_WIDTH 56
#define UI_CARD_HEIGHT 56
#define UI_CARD_GAP 1
#define UI_CARD_X 7
#define UI_CARD_Y 44
#define UI_CARD_PITCH (UI_CARD_WIDTH + UI_CARD_GAP)
#define UI_CARD_TEXT_WIDTH (UI_CARD_WIDTH - 4)
#define UI_CARD_TEXT_HEIGHT (UI_CARD_HEIGHT - 2)
#define UI_CARD_TEXT_MAX_LINES 3
#define UI_CARD_TEXT_LINE_CAPACITY 16
#define UI_CARD_TITLE_CACHE_SLOTS (UI_VISIBLE_CARD_COUNT + 2)
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
#define UI_TOOLBAR_CACHE_Y 110
#define UI_TOOLBAR_CACHE_HEIGHT 23
#define UI_TOOLBAR_CACHE_WORDS (((SCREEN_WIDTH * UI_TOOLBAR_CACHE_HEIGHT / 4) + 7) & ~7)
#define UI_TOOLBAR_ICON_COUNT 6
#define UI_TOOLBAR_ICON_FIRST_X 69
#define UI_TOOLBAR_ICON_PITCH 21
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

static const u8 toolbar_icon_colors[UI_TOOLBAR_ICON_COUNT] = {
	UI_COLOR_ERROR,
	UI_COLOR_BUSY,
	UI_COLOR_SELECTION,
	UI_COLOR_TEXT_DARK,
	UI_COLOR_TEXT_DARK,
	UI_COLOR_TEXT_DARK
};

static const u8 toolbar_icon_soft_colors[UI_TOOLBAR_ICON_COUNT] = {
	UI_COLOR_ICON_RED_SOFT,
	UI_COLOR_ICON_YELLOW_SOFT,
	UI_COLOR_ICON_BLUE_SOFT,
	UI_COLOR_ICON_DARK_SOFT,
	UI_COLOR_ICON_DARK_SOFT,
	UI_COLOR_ICON_DARK_SOFT
};

typedef struct UiCardTitleLayout_ {
	u16 lines[UI_CARD_TEXT_MAX_LINES][UI_CARD_TEXT_LINE_CAPACITY];
	u8 lengths[UI_CARD_TEXT_MAX_LINES];
	u8 line_count;
	u8 scale;
} UiCardTitleLayout;

typedef struct UiCardTitleCache_ {
	u32 key;
	u16 positions[UI_CARD_PIXEL_COUNT];
	u8 colors[UI_CARD_PIXEL_COUNT];
	u16 pixel_count;
	u32 stamp;
	BOOL valid;
} UiCardTitleCache;

typedef struct UiCoverCache_ {
	u16 cover_index;
	BOOL valid;
	u32 stamp;
	u8 pixels[UI_CARD_PIXEL_COUNT];
} UiCoverCache;

EWRAM_BSS static UiCardTitleCache sCardTitleCache[UI_CARD_TITLE_CACHE_SLOTS];
EWRAM_BSS static u8 sCardTitleBitmap[UI_CARD_PIXEL_COUNT];
EWRAM_BSS static UiCoverCache sCoverCache[UI_COVER_CACHE_SLOTS] ALIGN(4);
EWRAM_BSS static u32 sToolbarCache[2][UI_TOOLBAR_CACHE_WORDS];
static u32 sCardTitleCacheStamp;
static u32 sCoverCacheStamp;
static const u8 *sCoverPack;
static CoverPackHeader sCoverHeader;
static u32 sCoverPackSize;
static BOOL sCoverUsesEntrySlots;

static volatile u16 *UiBackBuffer(void) {
	return (REG_DISPCNT & 0x0010) ? UI_PAGE_0 : UI_PAGE_1;
}

static inline void UiDrawPixel(volatile u16 *buffer, u8 x, u8 y, u8 color) {
	u16 offset = (y * SCREEN_WIDTH + x) >> 1;
	u16 pixel = buffer[offset];
	if (x & 1) {
		buffer[offset] = ((u16)color << 8) | (pixel & 0x00FF);
	} else {
		buffer[offset] = (pixel & 0xFF00) | color;
	}
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
		if (width != 0) {
			*destination = (*destination & 0xFF00) | *source;
		}
	}
}

static void UiClear(volatile u16 *buffer, u8 color) {
	u32 color_word = (u32)color * 0x01010101;
	CpuFastSet(&color_word, (void *)buffer, FILL | COPY32 | UI_SCREEN_WORD_COUNT);
}

IWRAM_CODE ARM_CODE static void UiFillRect(volatile u16 *buffer, s16 x, s16 y, s16 width, s16 height, u8 color);

IWRAM_CODE ARM_CODE static void UiFillCircle(volatile u16 *buffer, s16 center_x, s16 center_y, u8 radius, u8 color) {
	s16 radius_squared = radius * radius;
	for (s16 y = -radius; y <= radius; y++) {
		s16 half_width = radius;
		while (half_width > 0 && half_width * half_width + y * y > radius_squared) half_width--;
		UiFillRect(buffer, center_x - half_width, center_y + y, half_width * 2 + 1, 1, color);
	}
}

IWRAM_CODE ARM_CODE static void UiFillToolbarCircle(volatile u16 *buffer, s16 center_x, s16 center_y, u8 radius, u8 color) {
	// Test pixel areas against a half-pixel-expanded radius so small circles
	// start with a short flat arc instead of a one-pixel spike.
	s16 diameter = radius * 2 + 1;
	s32 radius_squared = diameter * diameter;
	for (s16 y = -radius; y <= radius; y++) {
		s16 half_width = radius;
		while (half_width > 0) {
			s32 scaled_x = half_width * 2;
			s32 scaled_y = y * 2;
			if (scaled_x * scaled_x + scaled_y * scaled_y <= radius_squared) break;
			half_width--;
		}
		UiFillRect(buffer, center_x - half_width, center_y + y, half_width * 2 + 1, 1, color);
	}
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

IWRAM_CODE ARM_CODE static void UiDrawRoundedOutline(volatile u16 *buffer, s16 x, s16 y, s16 width, s16 height, u8 color) {
	UiFillRect(buffer, x + 1, y, width - 2, 1, color);
	UiFillRect(buffer, x + 1, y + height - 1, width - 2, 1, color);
	UiFillRect(buffer, x, y + 1, 1, height - 2, color);
	UiFillRect(buffer, x + width - 1, y + 1, 1, height - 2, color);
}

IWRAM_CODE ARM_CODE static void UiDrawSoftRoundedOutline(volatile u16 *buffer, s16 x, s16 y, s16 width, s16 height, u8 color) {
	UiFillRect(buffer, x + 2, y, width - 4, 1, color);
	UiFillRect(buffer, x + 1, y + 1, width - 2, 1, color);
	UiFillRect(buffer, x, y + 2, 1, height - 4, color);
	UiFillRect(buffer, x + width - 1, y + 2, 1, height - 4, color);
	UiFillRect(buffer, x + 1, y + height - 2, width - 2, 1, color);
	UiFillRect(buffer, x + 2, y + height - 1, width - 4, 1, color);
}

IWRAM_CODE ARM_CODE static void UiDrawAntialiasedCircle(
	volatile u16 *buffer,
	s16 center_x,
	s16 center_y,
	u8 radius,
	u8 color,
	u8 soft_color
) {
	UiFillToolbarCircle(buffer, center_x, center_y, radius, soft_color);
	UiFillCircle(buffer, center_x, center_y, radius, color);
}

IWRAM_CODE ARM_CODE static void UiDrawAntialiasedRing(
	volatile u16 *buffer,
	s16 center_x,
	s16 center_y,
	u8 radius,
	u8 color,
	u8 soft_color
) {
	UiDrawAntialiasedCircle(buffer, center_x, center_y, radius, color, soft_color);
	UiFillToolbarCircle(buffer, center_x, center_y, radius - 1, UI_COLOR_SURFACE);
}

IWRAM_CODE ARM_CODE static void UiDrawAntialiasedRoundedOutline(
	volatile u16 *buffer,
	s16 x,
	s16 y,
	s16 width,
	s16 height,
	u8 color,
	u8 soft_color
) {
	UiDrawRoundedOutline(buffer, x, y, width, height, soft_color);
	UiDrawSoftRoundedOutline(buffer, x, y, width, height, color);
}

IWRAM_CODE ARM_CODE static void UiTintTextRect(volatile u16 *buffer, s16 x, s16 y, s16 width, s16 height, u8 color) {
	for (s16 row = y; row < y + height; row++) {
		for (s16 col = x; col < x + width; col++) {
			u16 offset = (row * SCREEN_WIDTH + col) >> 1;
			u16 pair = buffer[offset];
			u8 pixel = (col & 1) ? pair >> 8 : pair & 0xFF;
			if (pixel < 250 || pixel > UI_COLOR_TEXT_DARK) continue;
			if (col & 1) {
				buffer[offset] = (pair & 0x00FF) | ((u16)color << 8);
			} else {
				buffer[offset] = (pair & 0xFF00) | color;
			}
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

static BOOL UiIsCjk(u16 ch) {
	return (ch >= 0x3400 && ch <= 0x4DBF)
		|| (ch >= 0x4E00 && ch <= 0x9FFF)
		|| (ch >= 0xF900 && ch <= 0xFAFF);
}

static BOOL UiIsTitleSpace(u16 ch) {
	return ch == 0x0020 || ch == 0x0009 || ch == 0x000A || ch == 0x000D || ch == 0x3000;
}

static void UiBuildCjkCardTitle(const ItemConfig *item, UiCardTitleLayout *layout, u8 max_lines) {
	u16 compact[0x30];
	u8 compact_length = 0;
	for (u8 i = 0; i < item->title_length && i < 0x30; i++) {
		if (!UiIsTitleSpace(item->title[i])) compact[compact_length++] = item->title[i];
	}
	if (compact_length == 0) return;

	u8 enlarged_width = 0;
	for (u8 i = 0; i < compact_length && i < 2; i++) {
		enlarged_width += GetFontCharacterWidth(compact[i], font) * 2;
	}
	if (compact_length <= 2
		&& GetFontHeight() * 2 <= UI_CARD_TEXT_HEIGHT
		&& enlarged_width <= UI_CARD_TEXT_WIDTH) {
		for (u8 i = 0; i < compact_length; i++) layout->lines[0][i] = compact[i];
		layout->lengths[0] = compact_length;
		layout->line_count = 1;
		layout->scale = 2;
		return;
	}

	u8 widest_character = 1;
	for (u8 i = 0; i < compact_length; i++) {
		u8 width = GetFontCharacterWidth(compact[i], font);
		if (width > widest_character) widest_character = width;
	}
	u8 characters_per_line = UI_CARD_TEXT_WIDTH / widest_character;
	if (characters_per_line == 0) characters_per_line = 1;
	if (characters_per_line > UI_CARD_TEXT_LINE_CAPACITY) {
		characters_per_line = UI_CARD_TEXT_LINE_CAPACITY;
	}

	u8 capacity = characters_per_line * max_lines;
	BOOL truncated = compact_length > capacity;
	u8 output_length = truncated ? capacity : compact_length;
	u8 line_count = (output_length + characters_per_line - 1) / characters_per_line;
	u8 position = 0;
	for (u8 line = 0; line < line_count; line++) {
		u8 remaining = output_length - position;
		u8 remaining_lines = line_count - line;
		u8 line_length = (remaining + remaining_lines - 1) / remaining_lines;
		for (u8 column = 0; column < line_length; column++, position++) {
			layout->lines[line][column] = truncated && position == output_length - 1
				? 0x2026
				: compact[position];
			layout->lengths[line]++;
		}
	}
	layout->line_count = line_count;
	layout->scale = 1;
}

static u8 UiMeasureCardLine(const u16 *text, u8 length) {
	u8 width = 0;
	for (u8 i = 0; i < length; i++) width += GetFontCharacterWidth(text[i], font);
	return width;
}

static void UiAppendCardEllipsis(UiCardTitleLayout *layout, u8 line) {
	u8 *length = &layout->lengths[line];
	while (*length > 0 && UiMeasureCardLine(layout->lines[line], *length) + GetFontCharacterWidth(0x2026, font) > UI_CARD_TEXT_WIDTH) {
		(*length)--;
	}
	if (*length < UI_CARD_TEXT_LINE_CAPACITY) layout->lines[line][(*length)++] = 0x2026;
}

static void UiBuildLatinCardTitle(const ItemConfig *item, UiCardTitleLayout *layout, u8 max_lines) {
	u8 position = 0;
	while (position < item->title_length && UiIsTitleSpace(item->title[position])) position++;

	while (position < item->title_length && layout->line_count < max_lines) {
		u8 line = layout->line_count;
		u8 start = position;
		u8 scan = position;
		u8 last_space = 0xFF;
		u8 width = 0;

		while (scan < item->title_length) {
			u16 ch = item->title[scan];
			if (UiIsTitleSpace(ch)) last_space = scan;
			u8 advance = GetFontCharacterWidth(ch, font);
			if (width + advance > UI_CARD_TEXT_WIDTH) break;
			width += advance;
			scan++;
		}

		u8 end = scan;
		position = scan;
		if (scan < item->title_length && last_space != 0xFF && last_space >= start) {
			end = last_space;
			position = last_space + 1;
		} else {
			if (end == start) end++;
			position = end;
		}
		while (end > start && UiIsTitleSpace(item->title[end - 1])) end--;

		for (u8 i = start; i < end && layout->lengths[line] < UI_CARD_TEXT_LINE_CAPACITY; i++) {
			layout->lines[line][layout->lengths[line]++] = item->title[i];
		}
		layout->line_count++;
		while (position < item->title_length && UiIsTitleSpace(item->title[position])) position++;
	}

	if (position < item->title_length && layout->line_count > 0) {
		UiAppendCardEllipsis(layout, layout->line_count - 1);
	}
	layout->scale = 1;
}

static void UiBuildCardTitleCache(UiCardTitleCache *cache, u32 key, const ItemConfig *item, BOOL light_text) {
	UiCardTitleLayout layout = { 0 };
	memset(sCardTitleBitmap, 0, sizeof(sCardTitleBitmap));
	cache->key = key;
	cache->pixel_count = 0;
	cache->valid = TRUE;
	u8 font_height = GetFontHeight();
	if (font_height == 0) return;
	u8 max_lines = UI_CARD_TEXT_HEIGHT / font_height;
	if (max_lines > UI_CARD_TEXT_MAX_LINES) max_lines = UI_CARD_TEXT_MAX_LINES;
	if (max_lines == 0) max_lines = 1;

	BOOL has_cjk = FALSE;
	for (u8 i = 0; i < item->title_length && i < 0x30; i++) {
		if (UiIsCjk(item->title[i])) {
			has_cjk = TRUE;
			break;
		}
	}
	if (has_cjk) {
		UiBuildCjkCardTitle(item, &layout, max_lines);
	} else {
		UiBuildLatinCardTitle(item, &layout, max_lines);
	}
	if (layout.line_count == 0) return;

	u8 line_height = font_height * layout.scale;
	u8 total_height = line_height * layout.line_count;
	u8 text_x = (UI_CARD_WIDTH - UI_CARD_TEXT_WIDTH) / 2;
	u8 text_y = (UI_CARD_HEIGHT - total_height) / 2;
	for (u8 line = 0; line < layout.line_count; line++) {
		DrawTextLineToBitmap(
			text_x,
			text_y + line * line_height,
			UI_CARD_TEXT_WIDTH,
			layout.lines[line],
			layout.lengths[line],
			font,
			sCardTitleBitmap,
			UI_CARD_WIDTH,
			UI_CARD_HEIGHT,
			light_text,
			layout.scale
		);
	}

	for (u16 position = 0; position < UI_CARD_PIXEL_COUNT; position++) {
		u8 color = sCardTitleBitmap[position];
		if (color == 0) continue;
		u8 x = position % UI_CARD_WIDTH;
		u8 y = position / UI_CARD_WIDTH;
		cache->positions[cache->pixel_count] = x | ((u16)y << 8);
		cache->colors[cache->pixel_count] = color;
		cache->pixel_count++;
	}
}

IWRAM_CODE ARM_CODE static UiCardTitleCache *UiGetCardTitleCache(u32 key, const ItemConfig *item, BOOL light_text) {
	UiCardTitleCache *oldest = &sCardTitleCache[0];
	for (u8 slot = 0; slot < UI_CARD_TITLE_CACHE_SLOTS; slot++) {
		UiCardTitleCache *cache = &sCardTitleCache[slot];
		if (cache->valid && cache->key == key) {
			cache->stamp = ++sCardTitleCacheStamp;
			return cache;
		}
		if (!cache->valid || (oldest->valid && cache->stamp < oldest->stamp)) oldest = cache;
	}

	UiBuildCardTitleCache(oldest, key, item, light_text);
	oldest->stamp = ++sCardTitleCacheStamp;
	return oldest;
}

IWRAM_CODE ARM_CODE static void UiDrawCachedCardTitle(volatile u16 *buffer, s16 card_x, s16 card_y, const UiCardTitleCache *cache) {
	for (u16 pixel = 0; pixel < cache->pixel_count; pixel++) {
		u16 position = cache->positions[pixel];
		UiDrawPixel(
			buffer,
			card_x + (position & 0xFF),
			card_y + (position >> 8),
			cache->colors[pixel]
		);
	}
}

IWRAM_CODE ARM_CODE static void UiDrawToolbarIcon(
	volatile u16 *buffer,
	s16 center_x,
	s16 center_y,
	u8 icon,
	u8 color,
	u8 soft_color,
	BOOL focused,
	BOOL pressed
) {
	if (focused) {
		if (pressed) center_y++;
		u8 radius = pressed ? 9 : 10;
		UiFillToolbarCircle(buffer, center_x, center_y, radius, UI_COLOR_SELECTION);
		UiFillToolbarCircle(buffer, center_x, center_y, radius - 1, UI_COLOR_SURFACE);
	} else {
		UiFillToolbarCircle(buffer, center_x, center_y, 9, UI_COLOR_TOOLBAR_EDGE_SOFT);
		UiFillCircle(buffer, center_x, center_y, 9, UI_COLOR_DIVIDER);
		UiFillToolbarCircle(buffer, center_x, center_y, 8, UI_COLOR_SURFACE);
	}

	if (icon == 0) {
		// Game details: a compact, rounded information mark.
		UiDrawAntialiasedCircle(buffer, center_x, center_y, 5, color, soft_color);
		UiFillCircle(buffer, center_x, center_y - 2, 1, color);
		UiFillRect(buffer, center_x, center_y, 1, 4, color);
		UiDrawPixel(buffer, center_x - 1, center_y + 3, soft_color);
		UiDrawPixel(buffer, center_x + 1, center_y + 3, soft_color);
	} else if (icon == 1) {
		// 卡带信息：上方标签及下方接点对应卡带外形。
		UiDrawAntialiasedRoundedOutline(buffer, center_x - 5, center_y - 5, 11, 11, color, soft_color);
		UiDrawOutline(buffer, center_x - 3, center_y - 3, 7, 4, 1, color);
		UiFillRect(buffer, center_x - 3, center_y + 3, 1, 2, color);
		UiFillRect(buffer, center_x, center_y + 3, 1, 2, color);
		UiFillRect(buffer, center_x + 3, center_y + 3, 1, 2, color);
	} else if (icon == 2) {
		// Storage map: rounded blocks connected by a route.
		UiDrawAntialiasedRoundedOutline(buffer, center_x - 5, center_y - 5, 5, 5, color, soft_color);
		UiDrawAntialiasedRoundedOutline(buffer, center_x + 1, center_y + 1, 5, 5, color, soft_color);
		UiFillRect(buffer, center_x - 1, center_y - 3, 3, 1, soft_color);
		UiFillRect(buffer, center_x + 1, center_y - 3, 1, 5, color);
		UiFillRect(buffer, center_x + 1, center_y + 1, 3, 1, soft_color);
	} else if (icon == 3) {
		// Controller test: a rounded pad with a D-pad and face buttons.
		UiDrawAntialiasedRoundedOutline(buffer, center_x - 6, center_y - 3, 13, 7, color, soft_color);
		UiFillRect(buffer, center_x - 4, center_y, 5, 1, color);
		UiFillRect(buffer, center_x - 2, center_y - 2, 1, 5, color);
		UiDrawPixel(buffer, center_x + 3, center_y - 1, color);
		UiDrawPixel(buffer, center_x + 5, center_y + 1, soft_color);
	} else if (icon == 4) {
		// Settings gear.
		UiDrawPixel(buffer, center_x, center_y - 5, soft_color);
		UiDrawPixel(buffer, center_x, center_y + 5, soft_color);
		UiDrawPixel(buffer, center_x - 5, center_y, soft_color);
		UiDrawPixel(buffer, center_x + 5, center_y, soft_color);
		UiDrawPixel(buffer, center_x, center_y - 4, color);
		UiDrawPixel(buffer, center_x, center_y + 4, color);
		UiDrawPixel(buffer, center_x - 4, center_y, color);
		UiDrawPixel(buffer, center_x + 4, center_y, color);
		UiDrawPixel(buffer, center_x - 3, center_y - 3, soft_color);
		UiDrawPixel(buffer, center_x + 3, center_y - 3, soft_color);
		UiDrawPixel(buffer, center_x - 3, center_y + 3, soft_color);
		UiDrawPixel(buffer, center_x + 3, center_y + 3, soft_color);
		UiDrawAntialiasedRing(buffer, center_x, center_y, 4, color, soft_color);
		UiDrawAntialiasedRing(buffer, center_x, center_y, 2, color, soft_color);
	} else {
		// Power button.
		UiDrawAntialiasedRing(buffer, center_x, center_y, 4, color, soft_color);
		UiFillRect(buffer, center_x - 2, center_y - 5, 5, 4, UI_COLOR_SURFACE);
		UiFillRect(buffer, center_x - 1, center_y - 5, 3, 4, soft_color);
		UiFillRect(buffer, center_x, center_y - 5, 1, 5, color);
	}
}

static void UiRenderSystemToolbarArtwork(volatile u16 *buffer, BOOL has_save) {
	(void)has_save;
	for (u8 icon = 0; icon < UI_TOOLBAR_ICON_COUNT; icon++) {
		UiDrawToolbarIcon(
			buffer,
			UI_TOOLBAR_ICON_FIRST_X + icon * UI_TOOLBAR_ICON_PITCH,
			121,
			icon,
			toolbar_icon_colors[icon],
			toolbar_icon_soft_colors[icon],
			FALSE,
			FALSE
		);
	}
}

IWRAM_CODE ARM_CODE static void UiDrawSystemToolbar(
	volatile u16 *buffer,
	BOOL has_save,
	BOOL focused,
	u8 selected,
	BOOL pressed
) {
	CpuFastSet(
		sToolbarCache[has_save ? 1 : 0],
		(void *)(buffer + UI_TOOLBAR_CACHE_Y * SCREEN_WIDTH / 2),
		COPY32 | UI_TOOLBAR_CACHE_WORDS
	);
	if (focused && selected < UI_TOOLBAR_ICON_COUNT) {
		UiDrawToolbarIcon(
			buffer,
			UI_TOOLBAR_ICON_FIRST_X + selected * UI_TOOLBAR_ICON_PITCH,
			121,
			selected,
			toolbar_icon_colors[selected],
			toolbar_icon_soft_colors[selected],
			TRUE,
			pressed
		);
	}
}

IWRAM_CODE ARM_CODE static void UiDrawHeaderIdentity(volatile u16 *buffer) {
	UiFillCircle(buffer, 15, 13, 10, UI_COLOR_SHADOW);
	UiFillCircle(buffer, 15, 13, 9, UI_COLOR_SURFACE);

	// A compact cheese wedge with a warm rind and three visible holes.
	UiFillRect(buffer, 17, 6, 3, 1, UI_COLOR_CHEESE_EDGE);
	UiFillRect(buffer, 14, 7, 7, 1, UI_COLOR_CHEESE_EDGE);
	UiFillRect(buffer, 12, 8, 10, 1, UI_COLOR_CHEESE_EDGE);
	UiFillRect(buffer, 10, 9, 13, 1, UI_COLOR_CHEESE_EDGE);
	UiFillRect(buffer, 8, 10, 16, 7, UI_COLOR_CHEESE_EDGE);
	UiFillRect(buffer, 9, 17, 14, 1, UI_COLOR_CHEESE_EDGE);
	UiFillRect(buffer, 11, 18, 10, 1, UI_COLOR_CHEESE_EDGE);
	UiFillRect(buffer, 17, 7, 2, 1, UI_COLOR_CHEESE);
	UiFillRect(buffer, 14, 8, 6, 1, UI_COLOR_CHEESE);
	UiFillRect(buffer, 12, 9, 9, 1, UI_COLOR_CHEESE);
	UiFillRect(buffer, 10, 10, 12, 1, UI_COLOR_CHEESE);
	UiFillRect(buffer, 9, 11, 14, 5, UI_COLOR_CHEESE);
	UiFillRect(buffer, 10, 16, 12, 1, UI_COLOR_CHEESE);
	UiFillRect(buffer, 12, 17, 8, 1, UI_COLOR_CHEESE);
	UiFillRect(buffer, 15, 9, 2, 1, UI_COLOR_CHEESE_HOLE);
	UiFillRect(buffer, 11, 12, 2, 2, UI_COLOR_CHEESE_HOLE);
	UiFillRect(buffer, 18, 14, 2, 2, UI_COLOR_CHEESE_HOLE);
}

IWRAM_CODE ARM_CODE static void UiDrawFooterConsole(volatile u16 *buffer) {
	UiDrawOutline(buffer, 10, 149, 14, 8, 1, UI_COLOR_TEXT_DARK);
	UiDrawRoundedOutline(buffer, 7, 149, 3, 8, UI_COLOR_TEXT_DARK);
	UiDrawRoundedOutline(buffer, 24, 149, 3, 8, UI_COLOR_TEXT_DARK);
	UiFillRect(buffer, 8, 151, 1, 1, UI_COLOR_TEXT_DARK);
	UiFillRect(buffer, 25, 151, 1, 1, UI_COLOR_TEXT_DARK);
	UiFillRect(buffer, 25, 154, 1, 1, UI_COLOR_TEXT_DARK);
}

IWRAM_CODE ARM_CODE static void UiDrawFooterButton(volatile u16 *buffer, s16 center_x, char label) {
	UiFillCircle(buffer, center_x, 153, 4, UI_COLOR_TEXT_DARK);
	if (label == 'A') {
		UiFillRect(buffer, center_x - 1, 151, 3, 1, UI_COLOR_SURFACE);
		UiFillRect(buffer, center_x - 2, 152, 1, 3, UI_COLOR_SURFACE);
		UiFillRect(buffer, center_x + 2, 152, 1, 3, UI_COLOR_SURFACE);
		UiFillRect(buffer, center_x - 1, 153, 3, 1, UI_COLOR_SURFACE);
	} else {
		UiFillRect(buffer, center_x - 2, 150, 1, 6, UI_COLOR_SURFACE);
		UiFillRect(buffer, center_x - 1, 151, 3, 1, UI_COLOR_SURFACE);
		UiFillRect(buffer, center_x - 1, 153, 3, 1, UI_COLOR_SURFACE);
		UiFillRect(buffer, center_x - 1, 155, 3, 1, UI_COLOR_SURFACE);
		UiFillRect(buffer, center_x + 2, 152, 1, 1, UI_COLOR_SURFACE);
		UiFillRect(buffer, center_x + 2, 154, 1, 1, UI_COLOR_SURFACE);
	}
}

static BOOL UiTextIsSystemReady(const UiMenuView *view) {
	u16 ready[32];
	u8 length = Utf8ToUnicode(UiLanguageText("系统就绪", "SYSTEM READY"), ready, 32);
	return view->status_text != NULL && view->status_length == length &&
		memcmp(view->status_text, ready, (u32)length * sizeof(u16)) == 0;
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

IWRAM_CODE ARM_CODE static void UiDrawCard(volatile u16 *buffer, s16 x, u16 item_index, u32 cache_key, const ItemConfig *item, BOOL selected, u8 lift, u8 expansion, u8 focus_color) {
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
		const u8 *cover = UiGetItemCover(item, (u16)(cache_key / ITEM_CONFIG_SIZE));
		if (cover != NULL) {
			UiBlitCover(buffer, x, UI_CARD_Y - lift, cover);
			cover_drawn = TRUE;
		}
	}

	if (!cover_drawn && card_x >= 0 && card_x + card_width <= SCREEN_WIDTH) {
		BOOL light_text = (item_index % 7) != 1 && (item_index % 7) != 2;
		UiCardTitleCache *cache = UiGetCardTitleCache(cache_key, item, light_text);
		UiDrawCachedCardTitle(buffer, x, UI_CARD_Y - lift, cache);
	}

	if (selected) {
		(void)focus_color;
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
	BG_PALETTE[UI_COLOR_CHEESE_EDGE] = RGB555(0xD9, 0x91, 0x19);
	BG_PALETTE[UI_COLOR_CHEESE] = RGB555(0xFF, 0xD9, 0x4A);
	BG_PALETTE[UI_COLOR_CHEESE_HOLE] = RGB555(0xC9, 0x78, 0x16);
	BG_PALETTE[UI_COLOR_TOOLBAR_EDGE_SOFT] = RGB555(0xEA, 0xEC, 0xF0);
	BG_PALETTE[UI_COLOR_ICON_RED_SOFT] = RGB555(0xF3, 0xA2, 0xAA);
	BG_PALETTE[UI_COLOR_ICON_YELLOW_SOFT] = RGB555(0xF1, 0xD8, 0x9A);
	BG_PALETTE[UI_COLOR_ICON_BLUE_SOFT] = RGB555(0x86, 0xC8, 0xE4);
	BG_PALETTE[UI_COLOR_ICON_DARK_SOFT] = RGB555(0x8D, 0x8F, 0x93);

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
	UiRenderSystemToolbarArtwork(UI_PAGE_0, FALSE);
	CpuFastSet(
		(void *)(UI_PAGE_0 + UI_TOOLBAR_CACHE_Y * SCREEN_WIDTH / 2),
		sToolbarCache[0],
		COPY32 | UI_TOOLBAR_CACHE_WORDS
	);
	UiClear(UI_PAGE_0, UI_COLOR_BACKGROUND);
	UiRenderSystemToolbarArtwork(UI_PAGE_0, TRUE);
	CpuFastSet(
		(void *)(UI_PAGE_0 + UI_TOOLBAR_CACHE_Y * SCREEN_WIDTH / 2),
		sToolbarCache[1],
		COPY32 | UI_TOOLBAR_CACHE_WORDS
	);
	UiClear(UI_PAGE_0, UI_COLOR_BACKGROUND);
	memset(sCardTitleCache, 0, sizeof(sCardTitleCache));
	sCardTitleCacheStamp = 0;
}

IWRAM_CODE ARM_CODE void UiRenderMenu(const UiMenuView *view) {
	volatile u16 *buffer = UiBackBuffer();
	UiClear(buffer, UI_COLOR_BACKGROUND);
	UiFillRect(buffer, 7, 143, SCREEN_WIDTH - 14, 1, UI_COLOR_DIVIDER);

	LoadFont(0);
	UiDrawHeaderIdentity(buffer);

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
	DrawText(9, 5, ALIGN_RIGHT, position_unicode, strlen(position_ascii), font, buffer, FALSE);

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
		u8 title_align = ALIGN_CENTER;
		// Edge titles align toward the screen margin; middle titles follow the cover center.
		if (title_left < UI_CARD_X) {
			title_left = UI_CARD_X;
			title_align = ALIGN_LEFT;
		} else if (title_left > SCREEN_WIDTH - UI_CARD_X - UI_TITLE_VIEW_WIDTH) {
			title_left = SCREEN_WIDTH - UI_CARD_X - UI_TITLE_VIEW_WIDTH;
			title_align = ALIGN_RIGHT;
		}
		s16 title_right = title_left + UI_TITLE_VIEW_WIDTH;
		LoadFont(selected_item.font);
		u16 title_width = MeasureTextWidth(selected_item.title, selected_item.title_length, font);
		if (title_width <= UI_TITLE_VIEW_WIDTH) {
			s16 title_x = title_left + (UI_TITLE_VIEW_WIDTH - title_width) / 2;
			if (title_align == ALIGN_LEFT) title_x = title_left;
			else if (title_align == ALIGN_RIGHT) title_x = title_right - title_width;
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

		// Draw the focused cover last so its enlarged edge and focus ring stay above its neighbors.
		for (u8 layer = 0; layer < 2; layer++) {
			for (u16 item_index = first_index; item_index < final_index; item_index++) {
				s16 x = UI_CARD_X + (s32)item_index * UI_CARD_PITCH - scroll_x;
				if (x >= SCREEN_WIDTH || x + UI_CARD_WIDTH <= 0) continue;

				BOOL selected = item_index == view->selected_index;
				BOOL card_focused = selected && !view->toolbar_focused;
				if ((layer == 0 && selected) || (layer == 1 && !selected)) continue;
				u8 lift = 0;
				u8 expansion = 0;
				u8 focus_color = UI_COLOR_SELECTION;
				if (card_focused) {
					u8 step = view->motion != NULL ? view->motion->step : UI_MOTION_FINAL_STEP;
					lift = step == 0 ? 1 : (step == 1 ? 2 : 3);
					expansion = step >= 2 ? 1 : 0;
				} else if (view->motion != NULL && view->motion->active && item_index == view->motion->previous_selected_index) {
					lift = view->motion->step == 0 ? 2 : (view->motion->step == 1 ? 1 : 0);
				}
				if (card_focused && view->feedback != NULL && view->feedback->active) {
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
				LoadFont(card_item.font);
				u32 cache_key = view->itemlist_offset + ITEM_CONFIG_SIZE * item_index;
				UiDrawCard(buffer, x, item_index, cache_key, &card_item, card_focused, lift, expansion, focus_color);
			}
		}
		UiDrawSystemToolbar(
			buffer,
			selected_item.save_type != SRAM_NONE,
			view->toolbar_focused,
			view->toolbar_selected,
			view->toolbar_pressed
		);

		LoadFont(0);
		UiDrawFooterConsole(buffer);
		if (selected_item.save_type != SRAM_NONE) {
			char save_ascii[32];
			u16 save_unicode[16] = { 0 };
			snprintf(save_ascii, sizeof(save_ascii), UiLanguageText("存档 %02u", "SAVE %02u"), (unsigned int)selected_item.save_index + 1);
			u8 save_length = Utf8ToUnicode(save_ascii, save_unicode, 16);
			DrawText(49, 145, ALIGN_LEFT, save_unicode, save_length, font, buffer, FALSE);
		}
	} else {
		if (view->status_tone == UI_STATUS_ERROR && view->status_text != NULL && view->status_length > 0) {
			DrawText(11, 27, ALIGN_LEFT, (u16 *)view->status_text, view->status_length, font, buffer, FALSE);
		} else {
			DrawText(11, 27, ALIGN_LEFT, sUiEnglish ? u"NO GAMES" : u"暂无游戏", sUiEnglish ? 8 : 4, font, buffer, FALSE);
		}
	}

	if (view->status_tone == UI_STATUS_NORMAL && UiTextIsSystemReady(view)) {
		LoadFont(0);
		UiDrawFooterButton(buffer, 185, 'A');
		DrawText(192, 145, ALIGN_LEFT, sUiEnglish ? u"START" : u"开始", sUiEnglish ? 5 : 2, font, buffer, FALSE);
	} else if (view->status_text != NULL && view->status_length > 0) {
		u8 tone_color = UI_COLOR_READY;
		if (view->status_tone == UI_STATUS_BUSY) tone_color = UI_COLOR_BUSY;
		if (view->status_tone == UI_STATUS_ERROR) tone_color = UI_COLOR_ERROR;
		UiFillCircle(buffer, 226, 153, 2, tone_color);
		LoadFont(0);
		DrawText(18, 145, ALIGN_RIGHT, (u16 *)view->status_text, view->status_length, font, buffer, FALSE);
	}
}

void UiPresent(void) {
	REG_DISPCNT ^= 0x0010;
}

static void UiDrawUtf8(volatile u16 *buffer, u8 x, u8 y, u8 align, const char *text) {
	u16 unicode[64];
	u8 length = Utf8ToUnicode(text, unicode, 64);
	DrawText(x, y, align, unicode, length, font, buffer, FALSE);
}

static void UiDrawUtf8Light(volatile u16 *buffer, u8 x, u8 y, u8 align, const char *text) {
	u16 unicode[64];
	u8 length = Utf8ToUnicode(text, unicode, 64);
	DrawText(x, y, align, unicode, length, font, buffer, TRUE);
}

static void UiDrawToolHeader(volatile u16 *buffer, const char *title, u8 accent) {
	UiFillRect(buffer, 0, 0, SCREEN_WIDTH, 3, accent);
	UiFillCircle(buffer, 16, 17, 10, UI_COLOR_SHADOW);
	UiFillCircle(buffer, 16, 17, 9, UI_COLOR_SURFACE);
	UiFillRect(buffer, 10, 16, 11, 2, accent);
	UiFillRect(buffer, 10, 15, 2, 4, accent);
	UiFillRect(buffer, 12, 14, 2, 2, accent);
	UiFillRect(buffer, 12, 18, 2, 2, accent);
	LoadFont(0);
	UiDrawUtf8(buffer, 34, 10, ALIGN_LEFT, title);
	UiFillRect(buffer, 8, 34, SCREEN_WIDTH - 16, 1, UI_COLOR_DIVIDER);
}

static void UiDrawToolFooter(volatile u16 *buffer, const char *action) {
	UiFillRect(buffer, 7, 143, SCREEN_WIDTH - 14, 1, UI_COLOR_DIVIDER);
	LoadFont(0);
	UiDrawFooterButton(buffer, action != NULL ? 130 : 174, 'B');
	UiDrawUtf8(buffer, action != NULL ? 137 : 181, 145, ALIGN_LEFT, UiLanguageText("返回", "BACK"));
	if (action != NULL) {
		UiDrawFooterButton(buffer, 185, 'A');
		UiDrawUtf8(buffer, 192, 145, ALIGN_LEFT, action);
	}
}

static void UiDrawToolCover(volatile u16 *buffer, const ItemConfig *item, u16 entry_slot, s16 x, s16 y) {
	UiFillRect(buffer, x + 2, y + 3, UI_CARD_WIDTH, UI_CARD_HEIGHT, UI_COLOR_SHADOW);
	UiFillRect(buffer, x, y, UI_CARD_WIDTH, UI_CARD_HEIGHT, UI_COLOR_CARD_FIRST + 5);
	if (item != NULL) {
		const u8 *cover = UiGetItemCover(item, entry_slot);
		if (cover != NULL) UiBlitCover(buffer, x, y, cover);
	}
}

static const char *UiSaveTypeText(SAVE_TYPE type) {
	if (type == SRAM_256K) return "SRAM 32K";
	if (type == SRAM_512K) return "SRAM 64K";
	if (type == SRAM_1M) return "SRAM 128K";
	return UiLanguageText("无存档", "NO SAVE");
}

static void UiDrawDetailsPage(volatile u16 *buffer, const UiToolView *view) {
	char value[32];
	UiDrawToolHeader(buffer, UiLanguageText("游戏详情", "GAME DETAILS"), UI_COLOR_ERROR);
	UiDrawToolCover(buffer, view->item, view->entry_slot, 14, 49);
	if (view->item != NULL) {
		LoadFont(view->item->font);
		DrawText(82, 43, ALIGN_LEFT, (u16 *)view->item->title, view->item->title_length, font, buffer, FALSE);
		LoadFont(0);
		snprintf(value, sizeof(value), UiLanguageText("游戏 %03u / %03u", "GAME %03u / %03u"), (unsigned int)view->selected_index + 1, (unsigned int)view->roms_total);
		UiDrawUtf8(buffer, 82, 66, ALIGN_LEFT, value);
		snprintf(value, sizeof(value), UiLanguageText("容量 %u MB", "ROM %u MB"), (unsigned int)view->item->rom_size / 2);
		UiDrawUtf8(buffer, 82, 82, ALIGN_LEFT, value);
		UiDrawUtf8(buffer, 82, 98, ALIGN_LEFT, UiSaveTypeText(view->item->save_type));
		if (view->item->save_type != SRAM_NONE) {
			snprintf(value, sizeof(value), UiLanguageText("存档位 %02u", "SLOT %02u"), (unsigned int)view->item->save_index + 1);
			UiDrawUtf8(buffer, 82, 114, ALIGN_LEFT, value);
		}
	}
	UiDrawToolFooter(buffer, UiLanguageText("开始", "PLAY"));
}

static const char *UiCapacitySourceText(CartridgeCapacitySource source);

static const char *UiNorModelText(const CartridgeProfile *cartridge) {
	if (cartridge == NULL) return UiLanguageText("未知", "UNKNOWN");
	if (cartridge->flash_type == 1) return "6600M0U0BE";
	if (cartridge->flash_type == 2) return "MSP55 / S29GL512";
	if (cartridge->flash_type == 3) return "MSP54";
	if (cartridge->flash_type == 4) {
		const char *model = NorModelName(cartridge->nor_id, cartridge->nor_device_id2,
			cartridge->nor_device_id3, cartridge->nor_buffer_bytes);
		if (model != NULL) return model;
	}
	return UiLanguageText("未知", "UNKNOWN");
}

static void UiDrawCardInfoLine(volatile u16 *buffer, u8 row, const char *label, const char *value) {
	u8 y = 39 + row * 13;
	UiDrawUtf8(buffer, 10, y, ALIGN_LEFT, label);
	UiDrawUtf8(buffer, 84, y, ALIGN_LEFT, value);
}

static void UiDrawCartridgeInfoPage(volatile u16 *buffer, const UiToolView *view) {
	char value[48];
	const CartridgeProfile *cartridge = view->cartridge;
	UiDrawToolHeader(buffer, UiLanguageText("卡带信息", "CARTRIDGE INFO"), UI_COLOR_BUSY);
	BOOL chimera_config = cartridge != NULL && cartridge->wl_mapping_valid &&
		(cartridge->mapping_flags & WL_MAPPING_FLAG_CHIMERA) != 0;
	UiDrawCardInfoLine(buffer, 0, UiLanguageText("卡带类型", "CARD TYPE"),
		chimera_config ? UiLanguageText("奇美拉(配置)", "CHIMERA (CONFIG)") : UiLanguageText("未标注", "UNSPECIFIED"));
	UiDrawCardInfoLine(buffer, 1, UiLanguageText("NOR型号", "NOR MODEL"), UiNorModelText(cartridge));
	if (cartridge != NULL && cartridge->nor_id != 0) {
		if (cartridge->nor_device_id3 == 0x2201 && (cartridge->nor_device_id2 & 0xFF00) == 0x2200)
			snprintf(value, sizeof(value), "%04X/%04X/%04X/%04X", (unsigned int)(cartridge->nor_id & 0xFFFF),
				(unsigned int)(cartridge->nor_id >> 16), (unsigned int)cartridge->nor_device_id2,
				(unsigned int)cartridge->nor_device_id3);
		else snprintf(value, sizeof(value), "%08lX", (unsigned long)cartridge->nor_id);
	}
	else snprintf(value, sizeof(value), "%s", UiLanguageText("未知", "UNKNOWN"));
	UiDrawCardInfoLine(buffer, 2, UiLanguageText("原始ID", "RAW ID"), value);
	if (cartridge != NULL && cartridge->probe_capacity_bytes != 0)
		snprintf(value, sizeof(value), "%uM / %s", (unsigned int)(cartridge->probe_capacity_bytes >> 20),
			UiCapacitySourceText(cartridge->probe_capacity_source));
	else snprintf(value, sizeof(value), "%s", UiLanguageText("未知", "UNKNOWN"));
	UiDrawCardInfoLine(buffer, 3, UiLanguageText("探测容量", "PROBED"), value);
	if (cartridge != NULL && cartridge->wl_mapping_valid)
		snprintf(value, sizeof(value), "%uM / CRC", (unsigned int)(cartridge->device_capacity_bytes >> 20));
	else snprintf(value, sizeof(value), "%s", UiLanguageText("无映射表", "NO MAP"));
	UiDrawCardInfoLine(buffer, 4, UiLanguageText("映射容量", "MAP SIZE"), value);
	if (cartridge != NULL && cartridge->flash_sector_size != 0)
		snprintf(value, sizeof(value), "%u KiB", (unsigned int)(cartridge->flash_sector_size >> 10));
	else snprintf(value, sizeof(value), "%s", UiLanguageText("未知", "UNKNOWN"));
	UiDrawCardInfoLine(buffer, 5, UiLanguageText("菜单扇区", "MENU SECTOR"), value);
	const char *layout = "UNKNOWN";
	if (cartridge != NULL) {
		if (cartridge->backend == CART_BACKEND_UNIVERSAL) layout = "UMMF + WLMI v2";
		else if (cartridge->backend == CART_BACKEND_WL) layout = "WLMI v2";
		else if (cartridge->backend == CART_BACKEND_LEGACY) layout = "Legacy";
	}
	UiDrawCardInfoLine(buffer, 6, UiLanguageText("目录格式", "LAYOUT"), layout);
	UiDrawToolFooter(buffer, NULL);
	UiDrawUtf8(buffer, 8, 145, ALIGN_LEFT, UiLanguageText("未检测片数", "COUNT NOT PROBED"));
}

static void UiDrawMapCell(volatile u16 *buffer, s16 x, s16 y, s16 width, s16 height, u8 color) {
	UiFillRect(buffer, x, y, width, height, color);
	if (color == UI_COLOR_SURFACE)
		UiDrawOutline(buffer, x, y, width, height, 1, UI_COLOR_DIVIDER);
}

static void UiDrawMapLegend(volatile u16 *buffer, s16 x, s16 y, u8 color, const char *label) {
	s16 marker_y = y + (GetFontHeight() - 7) / 2;
	UiDrawMapCell(buffer, x, marker_y, 7, 7, color);
	UiDrawUtf8(buffer, x + 10, y - 2, ALIGN_LEFT, label);
}

static const char *UiCapacitySourceText(CartridgeCapacitySource source) {
	if (source == CART_CAPACITY_CFI) return "CFI";
	if (source == CART_CAPACITY_ID) return "ID";
	if (source == CART_CAPACITY_MAPPING) return UiLanguageText("映射表", "MAP");
	return UiLanguageText("未知", "UNKNOWN");
}

u8 UiMapMaxScrollRow(u16 bucket_count) {
	u8 columns = bucket_count <= 64 ? 16 : 32;
	u8 rows = (bucket_count + columns - 1) / columns;
	return rows > UI_MAP_VISIBLE_ROWS ? rows - UI_MAP_VISIBLE_ROWS : 0;
}

static void UiDrawRomMapPage(volatile u16 *buffer, const UiToolView *view) {
	char value[96];
	u16 game_count = 0;
	u16 free_count = 0;
	u16 system_count = 0;
	u32 capacity_mib = view->cartridge != NULL ? view->cartridge->device_capacity_bytes >> 20 : 0;
	CartridgeCapacitySource capacity_source = view->cartridge != NULL
		? view->cartridge->capacity_source : CART_CAPACITY_UNKNOWN;

	UiDrawToolHeader(buffer, UiLanguageText("空间映射", "STORAGE MAP"), UI_COLOR_SELECTION);
	LoadFont(0);

	if (view->bucket_owners == NULL || view->bucket_count == 0) {
		if (capacity_mib != 0)
			snprintf(value, sizeof(value), UiLanguageText("容量 %uM  来源 %s", "MAX %uM  SOURCE %s"), (unsigned int)capacity_mib, UiCapacitySourceText(capacity_source));
		else
			snprintf(value, sizeof(value), "%s", UiLanguageText("容量未知，无法查询", "MAX UNKNOWN  PROBE UNAVAILABLE"));
		UiDrawUtf8(buffer, 8, 57, ALIGN_CENTER, value);
		UiDrawUtf8(buffer, 8, 84, ALIGN_CENTER, UiLanguageText("无法显示空间映射", "ALLOCATION MAP UNAVAILABLE"));
		UiDrawUtf8(buffer, 8, 103, ALIGN_CENTER, UiLanguageText("缺少有效目录或映射表", "NO VERIFIED METADATA"));
		UiDrawToolFooter(buffer, NULL);
		return;
	}

	if (capacity_mib == 0) capacity_mib = (u32)view->bucket_count * 4;
	snprintf(value, sizeof(value), "%u MiB", (unsigned int)capacity_mib);
	UiDrawUtf8(buffer, 8, 7, ALIGN_RIGHT, value);
	UiDrawUtf8(buffer, 8, 21, ALIGN_RIGHT, UiLanguageText("每格 4MiB", "4MiB/CELL"));
	UiDrawMapLegend(buffer, 16, 44, UI_COLOR_BUSY, UiLanguageText("系统", "SYSTEM"));
	UiDrawMapLegend(buffer, 90, 44, UI_COLOR_READY, UiLanguageText("游戏", "GAMES"));
	UiDrawMapLegend(buffer, 168, 44, UI_COLOR_SURFACE, UiLanguageText("空闲", "FREE"));

	u8 columns = view->bucket_count <= 64 ? 16 : 32;
	u8 pitch = 192 / columns;
	u8 max_scroll_row = UiMapMaxScrollRow(view->bucket_count);
	u8 first_row = view->map_scroll_row > max_scroll_row ? max_scroll_row : view->map_scroll_row;
	for (u16 bucket = 0; bucket < view->bucket_count; bucket++) {
		u16 owner = view->bucket_owners[bucket];
		if (owner == 0xFFFF) free_count++;
		else if (owner == 0xFFFE) system_count++;
		else game_count++;
		u8 row = bucket / columns;
		if (row < first_row || row >= first_row + UI_MAP_VISIBLE_ROWS) continue;
		u8 column = bucket % columns;
		s16 x = 34 + column * pitch;
		s16 y = 65 + (row - first_row) * 12;
		if (column == 0) {
			u16 row_end = bucket + columns;
			if (row_end > view->bucket_count) row_end = view->bucket_count;
			snprintf(value, sizeof(value), "%uM", (unsigned int)row_end * 4);
			UiDrawUtf8(buffer, 7, y - 1, ALIGN_LEFT, value);
		}
		if (owner == 0xFFFF) {
			UiDrawMapCell(buffer, x, y, pitch - 1, 9, UI_COLOR_SURFACE);
		} else {
			u8 color = owner == 0xFFFE ? UI_COLOR_BUSY
				: UI_COLOR_READY;
			UiDrawMapCell(buffer, x, y, pitch - 1, 9, color);
		}
	}

	snprintf(value, sizeof(value), UiLanguageText("系统 %uM  游戏 %uM  空闲 %uM", "SYS %uM  GAMES %uM  FREE %uM"),
		(unsigned int)system_count * 4, (unsigned int)game_count * 4, (unsigned int)free_count * 4);
	UiDrawUtf8(buffer, 8, 115, ALIGN_CENTER, value);
	UiDrawUtf8(buffer, 8, 130, ALIGN_CENTER, UiLanguageText("空闲表示目录未分配", "FREE = UNALLOCATED"));
	UiDrawToolFooter(buffer, NULL);
	if (max_scroll_row != 0) {
		u8 rows = max_scroll_row + UI_MAP_VISIBLE_ROWS;
		u8 thumb_height = 45 * UI_MAP_VISIBLE_ROWS / rows;
		UiFillRect(buffer, 231, 65, 2, 45, UI_COLOR_DIVIDER);
		UiFillRect(buffer, 231, 65 + (45 - thumb_height) * first_row / max_scroll_row,
			2, thumb_height, UI_COLOR_SELECTION);
		UiDrawUtf8(buffer, 8, 145, ALIGN_LEFT, UiLanguageText("上下 滚动", "UP/DOWN SCROLL"));
	}
}

static void UiDrawKeyCap(volatile u16 *buffer, s16 x, s16 y, s16 width, s16 height, const char *label, BOOL held) {
	u8 edge = held ? UI_COLOR_SELECTION : UI_COLOR_DIVIDER;
	u8 fill = held ? UI_COLOR_SELECTION : UI_COLOR_SURFACE;
	UiFillRect(buffer, x + 1, y + 2, width, height, UI_COLOR_SHADOW);
	UiFillRect(buffer, x, y, width, height, edge);
	UiFillRect(buffer, x + 2, y + 2, width - 4, height - 4, fill);
	LoadFont(0);
	UiDrawUtf8(buffer, x + 5, y + 5, ALIGN_LEFT, label);
}

static void UiDrawControllerPage(volatile u16 *buffer, const UiToolView *view) {
	u16 keys = view->held_keys;
	UiDrawToolHeader(buffer, UiLanguageText("按键测试", "CONTROLLER"), UI_COLOR_READY);
	UiDrawKeyCap(buffer, 21, 58, 22, 22, "U", (keys & KEY_UP) != 0);
	UiDrawKeyCap(buffer, 21, 102, 22, 22, "D", (keys & KEY_DOWN) != 0);
	UiDrawKeyCap(buffer, 0, 80, 22, 22, "L", (keys & KEY_LEFT) != 0);
	UiDrawKeyCap(buffer, 42, 80, 22, 22, "R", (keys & KEY_RIGHT) != 0);
	UiDrawKeyCap(buffer, 179, 70, 25, 25, "B", (keys & KEY_B) != 0);
	UiDrawKeyCap(buffer, 207, 54, 25, 25, "A", (keys & KEY_A) != 0);
	UiDrawKeyCap(buffer, 74, 49, 35, 18, "L", (keys & KEY_L) != 0);
	UiDrawKeyCap(buffer, 131, 49, 35, 18, "R", (keys & KEY_R) != 0);
	UiDrawKeyCap(buffer, 78, 104, 39, 18, "SEL", (keys & KEY_SELECT) != 0);
	UiDrawKeyCap(buffer, 124, 104, 39, 18, "START", (keys & KEY_START) != 0);
	UiFillRect(buffer, 7, 143, SCREEN_WIDTH - 14, 1, UI_COLOR_DIVIDER);
	LoadFont(0);
	UiDrawUtf8(buffer, 8, 145, ALIGN_CENTER, UiLanguageText("L + R  返回", "L + R  BACK"));
}

static void UiDrawToggle(volatile u16 *buffer, s16 x, s16 y, BOOL enabled) {
	u8 color = enabled ? UI_COLOR_READY : UI_COLOR_DIVIDER;
	UiFillCircle(buffer, x + 5, y + 5, 5, color);
	UiFillCircle(buffer, x + 15, y + 5, 5, color);
	UiFillRect(buffer, x + 5, y, 11, 11, color);
	UiFillCircle(buffer, enabled ? x + 15 : x + 5, y + 5, 4, UI_COLOR_SURFACE);
}

static void UiDrawSettingsPage(volatile u16 *buffer, const UiToolView *view) {
	static const char *chinese_labels[MENU_SETTINGS_COUNT] = { "操作音效", "切换动画", "循环选择", "记住上次游戏", "界面语言" };
	static const char *english_labels[MENU_SETTINGS_COUNT] = { "SOUND EFFECTS", "MOVE ANIMATION", "WRAP AROUND", "RESUME LAST GAME", "LANGUAGE" };
	static const u8 masks[MENU_SETTINGS_COUNT] = { MENU_SETTING_SOUND, MENU_SETTING_MOTION, MENU_SETTING_WRAP, MENU_SETTING_RESUME, MENU_SETTING_ENGLISH };
	UiDrawToolHeader(buffer, UiLanguageText("系统设置", "SYSTEM SETTINGS"), UI_COLOR_TEXT_DARK);
	for (u8 row = 0; row < MENU_SETTINGS_COUNT; row++) {
		s16 y = 40 + row * 20;
		if (view->row == row) {
			UiFillRect(buffer, 10, y, SCREEN_WIDTH - 20, 19, UI_COLOR_SELECTION);
			UiFillRect(buffer, 12, y + 2, SCREEN_WIDTH - 24, 15, UI_COLOR_SURFACE);
		}
		LoadFont(0);
		UiDrawUtf8(buffer, 20, y + 3, ALIGN_LEFT, UiLanguageText(chinese_labels[row], english_labels[row]));
		if (row == MENU_SETTINGS_COUNT - 1)
			UiDrawUtf8(buffer, 158, y + 3, ALIGN_LEFT, UiLanguageText("中文", "English"));
		else
			UiDrawToggle(buffer, 198, y + 4, (view->settings_flags & masks[row]) != 0);
	}
	UiDrawToolFooter(buffer, UiLanguageText("切换", "SET"));
}

static void UiDrawSleepPage(volatile u16 *buffer) {
	UiClear(buffer, UI_COLOR_TEXT_DARK);
	UiFillCircle(buffer, 120, 70, 24, UI_COLOR_DIVIDER);
	UiFillCircle(buffer, 130, 62, 24, UI_COLOR_TEXT_DARK);
	LoadFont(0);
	UiDrawUtf8Light(buffer, 8, 108, ALIGN_CENTER, UiLanguageText("休眠模式", "SLEEP MODE"));
	UiDrawUtf8Light(buffer, 8, 128, ALIGN_CENTER, UiLanguageText("A 休眠   B 返回", "A SLEEP   B BACK"));
}

void UiRenderToolPage(const UiToolView *view) {
	volatile u16 *buffer = UiBackBuffer();
	UiClear(buffer, UI_COLOR_BACKGROUND);
	if (view->page == UI_TOOL_DETAILS) UiDrawDetailsPage(buffer, view);
	else if (view->page == UI_TOOL_CARTRIDGE_INFO) UiDrawCartridgeInfoPage(buffer, view);
	else if (view->page == UI_TOOL_MAP) UiDrawRomMapPage(buffer, view);
	else if (view->page == UI_TOOL_CONTROLLER) UiDrawControllerPage(buffer, view);
	else if (view->page == UI_TOOL_SETTINGS) UiDrawSettingsPage(buffer, view);
	else if (view->page == UI_TOOL_SLEEP) UiDrawSleepPage(buffer);
}
