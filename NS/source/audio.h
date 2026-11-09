#ifndef AUDIO_H_
#define AUDIO_H_

#include "main.h"

typedef enum UiSound_
{
	UI_SOUND_MOVE = 0,
	UI_SOUND_CONFIRM,
	UI_SOUND_BACK,
	UI_SOUND_BOUNDARY,
	UI_SOUND_ERROR,
	UI_SOUND_COUNT
} UiSound;

void AudioInitialize(void);
void AudioShutdown(void);
void AudioVBlank(void);
void AudioFrame(void);
void AudioPlay(UiSound sound);
void AudioPlayMove(BOOL repeated);
void AudioSetEnabled(BOOL enabled);
BOOL AudioIsEnabled(void);

#endif
