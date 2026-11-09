#include <gba_dma.h>
#include <gba_interrupt.h>
#include <gba_sound.h>
#include <gba_timers.h>
#include <string.h>

#include "audio.h"
#include "ui_back_pcm.h"
#include "ui_boundary_pcm.h"
#include "ui_confirm_pcm.h"
#include "ui_error_pcm.h"
#include "ui_move_pcm.h"
#include "ui_move_repeat_pcm.h"

#define AUDIO_SAMPLE_RATE_RELOAD 0xFC00
#define AUDIO_BUFFER_SAMPLES 4096
#define AUDIO_END_PADDING 64
#define AUDIO_START_DELAY_FRAMES 1
#define AUDIO_VOLUME_DIVISOR 512
#define MOVE_CHAIN_WINDOW_FRAMES 18
#define MOVE_FIRST_REPEAT_VOLUME 82
#define MOVE_FAST_REPEAT_VOLUME 56

typedef struct AudioSample_
{
	const u8 *data;
	u16 length;
} AudioSample;

static const AudioSample sEffects[UI_SOUND_COUNT] = {
	{ ui_move_pcm,     ui_move_pcm_size },
	{ ui_confirm_pcm,  ui_confirm_pcm_size },
	{ ui_back_pcm,     ui_back_pcm_size },
	{ ui_boundary_pcm, ui_boundary_pcm_size },
	{ ui_error_pcm,    ui_error_pcm_size }
};

static const AudioSample sMoveRepeatEffect =
	{ ui_move_repeat_pcm, ui_move_repeat_pcm_size };

EWRAM_BSS static s8 sPlaybackBuffer[AUDIO_BUFFER_SAMPLES] ALIGN(4);
static volatile BOOL sAudioReady;
static const AudioSample *sPendingSample;
static u8 sPendingVolume;
static u8 sPendingFrames;
static u8 sMoveIdleFrames;
static u8 sMoveChainDepth;

static void AudioStopPlayback(void)
{
	REG_TM0CNT_H = 0;
	REG_TM1CNT_H = 0;
	REG_DMA1CNT = 0;
	REG_DMA2CNT = 0;
	REG_SOUNDCNT_H = SNDA_VOL_100 | SNDA_R_ENABLE | SNDA_L_ENABLE | SNDA_RESET_FIFO;
	REG_IF = IRQ_TIMER1;
}

static void AudioTimerInterrupt(void)
{
	AudioStopPlayback();
}

static void AudioStart(const AudioSample *sample, u8 volume)
{
	u16 ime = REG_IME;
	REG_IME = 0;
	AudioStopPlayback();

	u32 length = sample->length;
	if (length > AUDIO_BUFFER_SAMPLES - AUDIO_END_PADDING)
		length = AUDIO_BUFFER_SAMPLES - AUDIO_END_PADDING;
	for (u32 i = 0; i < length; i++)
	{
		s16 value = (s8)sample->data[i];
		sPlaybackBuffer[i] = (s8)((value * volume) / AUDIO_VOLUME_DIVISOR);
	}
	memset(sPlaybackBuffer + length, 0, AUDIO_END_PADDING);

	u16 playback_length = (u16)(length + AUDIO_END_PADDING);
	REG_SOUNDCNT_X = SNDSTAT_ENABLE;
	REG_SOUNDCNT_H = SNDA_VOL_100 | SNDA_R_ENABLE | SNDA_L_ENABLE | SNDA_RESET_FIFO;
	REG_DMA1SAD = (u32)sPlaybackBuffer;
	REG_DMA1DAD = (u32)&REG_FIFO_A;
	REG_DMA1CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;
	REG_TM1CNT_L = (u16)(0x10000 - playback_length);
	REG_TM1CNT_H = TIMER_COUNT | TIMER_IRQ | TIMER_START;
	REG_TM0CNT_L = AUDIO_SAMPLE_RATE_RELOAD;
	REG_TM0CNT_H = TIMER_START;
	REG_IME = ime;
}

static void AudioQueue(const AudioSample *sample, u8 volume)
{
	u16 ime = REG_IME;
	REG_IME = 0;
	AudioStopPlayback();
	sPendingSample = sample;
	sPendingVolume = volume;
	sPendingFrames = AUDIO_START_DELAY_FRAMES;
	REG_IME = ime;
}

void AudioInitialize(void)
{
	AudioStopPlayback();
	REG_SOUNDCNT_X = SNDSTAT_ENABLE;
	irqSet(IRQ_TIMER1, AudioTimerInterrupt);
	irqEnable(IRQ_TIMER1);
	sPendingSample = NULL;
	sPendingFrames = 0;
	sMoveIdleFrames = 0xFF;
	sMoveChainDepth = 0;
	sAudioReady = TRUE;
}

void AudioShutdown(void)
{
	if (!sAudioReady) return;
	sAudioReady = FALSE;
	sPendingSample = NULL;
	sPendingFrames = 0;
	irqDisable(IRQ_TIMER1);
	AudioStopPlayback();
	REG_SOUNDCNT_H = 0;
	REG_SOUNDCNT_X = 0;
}

void AudioVBlank(void)
{
}

void AudioFrame(void)
{
	if (!sAudioReady) return;
	if (sMoveIdleFrames != 0xFF) sMoveIdleFrames++;
	if (sPendingSample != NULL && sPendingFrames > 0)
	{
		sPendingFrames--;
		if (sPendingFrames == 0)
		{
			const AudioSample *sample = sPendingSample;
			sPendingSample = NULL;
			AudioStart(sample, sPendingVolume);
		}
	}
}

void AudioPlay(UiSound sound)
{
	if (!sAudioReady || sound >= UI_SOUND_COUNT) return;
	sMoveIdleFrames = 0xFF;
	sMoveChainDepth = 0;
	AudioQueue(&sEffects[sound], 128);
}

void AudioPlayMove(BOOL repeated)
{
	if (!sAudioReady) return;

	BOOL chained = repeated || sMoveIdleFrames <= MOVE_CHAIN_WINDOW_FRAMES;
	u8 volume = 128;
	if (chained)
	{
		if (sMoveChainDepth < 2) sMoveChainDepth++;
		volume = sMoveChainDepth == 1
			? MOVE_FIRST_REPEAT_VOLUME
			: MOVE_FAST_REPEAT_VOLUME;
	}
	else
	{
		sMoveChainDepth = 0;
	}

	sMoveIdleFrames = 0;
	AudioQueue(chained ? &sMoveRepeatEffect : &sEffects[UI_SOUND_MOVE], volume);
}
