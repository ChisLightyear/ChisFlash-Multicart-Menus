#ifndef UI_H_
#define UI_H_

#include <gba_base.h>

#include "main.h"

#define UI_VISIBLE_CARD_COUNT 4

typedef enum UiStatusTone_
{
	UI_STATUS_NORMAL,
	UI_STATUS_BUSY,
	UI_STATUS_ERROR
} UiStatusTone;

typedef enum UiFeedbackType_
{
	UI_FEEDBACK_NONE = 0,
	UI_FEEDBACK_CONFIRM,
	UI_FEEDBACK_BACK,
	UI_FEEDBACK_BOUNDARY,
	UI_FEEDBACK_ERROR
} UiFeedbackType;

typedef struct UiFeedback_
{
	UiFeedbackType type;
	u8 step;
	s8 direction;
	BOOL active;
} UiFeedback;

typedef struct UiMenuMotion_
{
	s32 scroll_x;
	s32 scroll_start_x;
	s32 scroll_target_x;
	u16 previous_selected_index;
	u8 step;
	s8 direction;
	BOOL active;
} UiMenuMotion;

typedef struct UiTitleScroll_
{
	u16 frame;
	u16 offset;
} UiTitleScroll;

typedef struct UiMenuView_
{
	const u8 *itemlist;
	u16 itemlist_offset;
	u16 roms_total;
	u16 selected_index;
	const u16 *status_text;
	u8 status_length;
	UiStatusTone status_tone;
	const UiMenuMotion *motion;
	const UiFeedback *feedback;
	const UiTitleScroll *title_scroll;
} UiMenuView;

void UiInitialize(void);
void UiMotionInitialize(UiMenuMotion *motion, u16 selected_index, u16 roms_total);
void UiMotionSelect(UiMenuMotion *motion, u16 previous_index, u16 selected_index, u16 roms_total, s8 direction, BOOL page_jump);
BOOL UiMotionAdvance(UiMenuMotion *motion);
void UiTitleScrollInitialize(UiTitleScroll *scroll);
BOOL UiTitleScrollAdvance(UiTitleScroll *scroll, const ItemConfig *item);
void UiFeedbackInitialize(UiFeedback *feedback);
void UiFeedbackStart(UiFeedback *feedback, UiFeedbackType type, s8 direction);
BOOL UiFeedbackAdvance(UiFeedback *feedback);
IWRAM_CODE ARM_CODE void UiRenderMenu(const UiMenuView *view);
void UiPresent(void);

#endif
