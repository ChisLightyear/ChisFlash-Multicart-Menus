/*
GBA Multi Game Menu
Author: Lesserkuma (github.com/lesserkuma)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"

FontSpecs sFontSpecs;
NFTR_Header sNFTR_Header;
FINF_Header sFINF_Header;
CGLP_Header sCGLP_Header;
CWDH_Header sCWDH_Header;
CMAP_Header sCMAP_Header;

u16 FallbackCharacter;
s8 FontMarginTop;
s8 FontMarginBottom;

u8 last_font = -1;
const u8* font;

static inline void DrawFontPixel(volatile u16 *buffer, u8 row, u8 col, u8 color) {
	u16 offset = (row * SCREEN_WIDTH + col) >> 1;
	u16 pixel = buffer[offset];
	if (col & 1) {
		buffer[offset] = (color << 8) | (pixel & 0x00FF);
	} else {
		buffer[offset] = (pixel & 0xFF00) | color;
	}
}

void LoadFont(u8 index) {
	if (index != last_font) {
		font = font_nftr;
		FallbackCharacter = 0x2753;
		FontMarginTop = 0;
		FontMarginBottom = 0;
#ifdef FONT_NTR_IPL
		if (index == 1) {
			font = NTR_IPL_font_s_nftr;
			FallbackCharacter = 0xE011;
			FontMarginTop = 2;
			FontMarginBottom = 3;
		}
#endif
#ifdef FONT_TBF1
		if (index == 2) {
			font = TBF1_s_nftr;
			FallbackCharacter = 0xE011;
			FontMarginTop = 1;
			FontMarginBottom = 1;
		}
#endif
#ifdef FONT_TBF1_CN
		if (index == 3) {
			font = TBF1_cn_s_nftr;
			FallbackCharacter = 0xE011;
			FontMarginTop = 2;
			FontMarginBottom = 2;
		}
#endif
#ifdef FONT_TBF1_KR
		if (index == 4) {
			font = TBF1_kr_s_nftr;
			FallbackCharacter = 0xE011;
			FontMarginTop = 2;
			FontMarginBottom = 2;
		}
#endif
#ifdef FONT_TWL_IRAJ_1
		if (index == 5) {
			font = TWL_IRAJ_1_nftr;
			FallbackCharacter = 0xFF1F;
			FontMarginTop = 4;
			FontMarginBottom = 6;
		}
#endif
		LoadNFTR(font);
		last_font = index;
	}
}

void LoadNFTR(const u8* nftr_data) {
	u32 pos = 0;
	memcpy(&sNFTR_Header, nftr_data+pos, sizeof(sNFTR_Header));
	pos += sNFTR_Header.size;
	memcpy(&sFINF_Header, nftr_data+pos, sizeof(sFINF_Header));
	pos += sFINF_Header.size;
	memcpy(&sCGLP_Header, nftr_data+pos, sizeof(sCGLP_Header));
	sFontSpecs.cglp_offset = pos;
	pos = sFINF_Header.offset_CWDH - 8;
	memcpy(&sCWDH_Header, nftr_data+pos, sizeof(sCWDH_Header));
	sFontSpecs.cwdh_offset = pos + 16;
	pos = sFINF_Header.offset_CMAP - 8;
	memcpy(&sCMAP_Header, nftr_data+pos, sizeof(sCMAP_Header));
	sFontSpecs.cmap_offset = pos;
	sFontSpecs.nftr_version = sNFTR_Header.version;
	sFontSpecs.max_width = sCGLP_Header.max_width;
	sFontSpecs.max_height = sCGLP_Header.max_height;
	sFontSpecs.bytes_per_char = sCGLP_Header.bytes_per_char;
	sFontSpecs.num_of_chars = sCWDH_Header.num_of_chars;
	sFontSpecs.bpp = sCGLP_Header.bpp;
}

IWRAM_CODE ARM_CODE u16 GetFontIndex(u16 ch, const u8* nftr_data) {
	u32 pos = sFontSpecs.cmap_offset;
	while (TRUE) {
		if (pos <= 0) return 0xFFFF;
		const CMAP_Header *header = (const CMAP_Header *)(nftr_data + pos);
		pos += 20;
		
		if (ch < header->start_code || ch > header->end_code) {
			// CMAP doesn't have this character
			pos = header->next_offset - 8;
			continue;
		}
		
		if (header->type == 0) {
			u16 index_offset = nftr_data[pos+1] << 8 | nftr_data[pos];
			return ch - header->start_code + index_offset;
		} else if (header->type == 1) {
			pos += (u32)(ch - header->start_code) * sizeof(u16);
			return nftr_data[pos+1] << 8 | nftr_data[pos];
		} else if (header->type == 2) {
			u16 index_offset = nftr_data[pos+1] << 8 | nftr_data[pos];
			pos += 2;
			for (u16 i = 0; i < index_offset; i++) {
				u16 utf16le_index = nftr_data[pos+1] << 8 | nftr_data[pos];
				pos += 2;
				u16 font_index = nftr_data[pos+1] << 8 | nftr_data[pos];
				pos += 2;
				if (utf16le_index == ch) return font_index;
				utf16le_index += 1;
			}
		}
		return 0xFFFF;
	}
}

IWRAM_CODE ARM_CODE void GetFontWidths(u16 index, const u8* nftr_data, u8* a, u8* b, u8* c) {
	u32 pos = sFontSpecs.cwdh_offset;
	pos = pos + (index * 3);
	*a = nftr_data[pos++];
	*b = nftr_data[pos++];
	*c = nftr_data[pos++];
}

IWRAM_CODE ARM_CODE u8 GetFontCharacterWidth(u16 ch, const u8* nftr_data) {
	u16 index = GetFontIndex(ch, nftr_data);
	if (index == 0xFFFF) index = GetFontIndex(FallbackCharacter, nftr_data);
	if (index == 0xFFFF) return sFontSpecs.max_width;

	u8 a = 0;
	u8 b = 0;
	u8 c = 0;
	GetFontWidths(index, nftr_data, &a, &b, &c);
	if (a >= sFontSpecs.max_width || ch == 0x0020 || ch == 0x3000) a = 0;

	u8 width = c - a;
	if (b == 0) width = c;
	if (sFontSpecs.nftr_version == 1) {
		if (width == 0) width = sFontSpecs.max_width;
		width++;
	}
	return width;
}

IWRAM_CODE ARM_CODE u16 MeasureTextWidth(const u16* text, u8 length, const u8* nftr_data) {
	u16 width = 0;
	for (u8 i = 0; i < length; i++) {
		if (text[i] == 0x0000 || text[i] == 0xFFFF) break;
		width += GetFontCharacterWidth(text[i], nftr_data);
	}
	return width;
}

u8 GetFontHeight(void) {
	return sFontSpecs.max_height;
}

void AsciiToUnicode(char* text, u16* output) {
	for (u8 i = 0; i < 64; i++) {
		if (text[i] == 0) break;
		output[i] = text[i];
	}
}

u8 Utf8ToUnicode(const char *text, u16 *output, u8 capacity) {
	if (text == NULL || output == NULL || capacity == 0) return 0;
	const u8 *cursor = (const u8 *)text;
	u8 length = 0;
	while (*cursor != 0 && length + 1 < capacity) {
		u16 character = '?';
		u8 consumed = 1;
		if (cursor[0] < 0x80) {
			character = cursor[0];
		} else if (cursor[0] >= 0xC2 && cursor[0] <= 0xDF &&
			(cursor[1] & 0xC0) == 0x80) {
			character = ((cursor[0] & 0x1F) << 6) | (cursor[1] & 0x3F);
			consumed = 2;
		} else if (cursor[0] >= 0xE0 && cursor[0] <= 0xEF &&
			cursor[1] != 0 && (cursor[1] & 0xC0) == 0x80 &&
			(cursor[2] & 0xC0) == 0x80) {
			u16 decoded = ((cursor[0] & 0x0F) << 12) | ((cursor[1] & 0x3F) << 6) | (cursor[2] & 0x3F);
			if (decoded >= 0x800 && (decoded < 0xD800 || decoded > 0xDFFF)) {
				character = decoded;
				consumed = 3;
			}
		} else if (cursor[0] >= 0xF0 && cursor[0] <= 0xF4 &&
			cursor[1] != 0 && (cursor[1] & 0xC0) == 0x80 &&
			cursor[2] != 0 && (cursor[2] & 0xC0) == 0x80 &&
			(cursor[3] & 0xC0) == 0x80) {
			consumed = 4;
		}
		output[length++] = character;
		cursor += consumed;
	}
	output[length] = 0;
	return length;
}

IWRAM_CODE ARM_CODE void DrawText(u8 px, u8 py, u8 align, u16* text, u8 length, const u8* nftr_data, volatile void* vram, BOOL highlighted) {
	u16 glyph_indices[256];
	u8 glyph_positions[256];
	u8 glyph_count = 0;
	u8 pos_left = 0;
	u8 color_modifier = highlighted ? 10 : 0;
	volatile u16 *buffer = (volatile u16 *)vram;

	py += FontMarginTop;

	for (u8 i = 0; i < length; i++) {
		u8 a = 0;
		u8 b = 0;
		u8 c = 0;
		u8 glyph_left;
		u8 glyph_width;
		u16 ch = text[i];
		u16 index;
		if (ch == 0x0000 || ch == 0xFFFF) {
			break;
		}
		
		BOOL last_ch = FALSE;
		while (1) {
			index = GetFontIndex(ch, nftr_data);
			if (index == 0xFFFF) { // character not found
				ch = FallbackCharacter;
				continue;
			}
			if (pos_left + sFontSpecs.max_width + (sFontSpecs.max_width >> 1) >= SCREEN_WIDTH - SCREEN_MARGIN_RIGHT - px) {
				if (ch != 0x2026) {
					ch = 0x2026; // ...
					last_ch = TRUE;
					continue;
				}
			}
			GetFontWidths(index, nftr_data, &a, &b, &c);
			
			glyph_left = a;
			if (glyph_left >= sFontSpecs.max_width) glyph_left = 0;
			if (ch == 32) { // space
				glyph_left = 0;
			}
			
			glyph_width = c - glyph_left;
			break;
		}
		if (pos_left + glyph_width >= SCREEN_WIDTH - px) {
			break;
		}

		if (b == 0) {
			glyph_width = c;
		}
		if (sFontSpecs.nftr_version == 1) {
			if (glyph_width == 0) glyph_width = sFontSpecs.max_width;
			glyph_width += 1;
		}

		glyph_indices[glyph_count] = index;
		glyph_positions[glyph_count] = pos_left;
		glyph_count++;
		pos_left += glyph_width;
		if (last_ch) break;
	}

	if (align == ALIGN_CENTER) {
		px = (SCREEN_WIDTH - pos_left) >> 1;
	} else if (align == ALIGN_RIGHT) {
		px = SCREEN_WIDTH - pos_left - px + 1;
	}

	if (sFontSpecs.bpp == 1) {
		u8 color = 254 - color_modifier;
		for (u8 glyph = 0; glyph < glyph_count; glyph++) {
			const u8 *glyph_data = &nftr_data[
				sFontSpecs.cglp_offset + 0x10 + glyph_indices[glyph] * sFontSpecs.bytes_per_char
			];
			u16 pixel_index = 0;
			for (u8 row = 0; row < sFontSpecs.max_height; row++) {
				for (u8 col = 0; col < sFontSpecs.max_width; col++, pixel_index++) {
					if ((glyph_data[pixel_index >> 3] >> (7 - (pixel_index & 7))) & 1) {
						DrawFontPixel(buffer, py + row, px + glyph_positions[glyph] + col, color);
					}
				}
			}
		}
	} else if (sFontSpecs.bpp == 2) {
		for (u8 glyph = 0; glyph < glyph_count; glyph++) {
			const u8 *glyph_data = &nftr_data[
				sFontSpecs.cglp_offset + 0x10 + glyph_indices[glyph] * sFontSpecs.bytes_per_char
			];
			u16 pixel_index = 0;
			for (u8 row = 0; row < sFontSpecs.max_height; row++) {
				for (u8 col = 0; col < sFontSpecs.max_width; col++, pixel_index++) {
					u8 packed = (glyph_data[pixel_index >> 2] >> (6 - ((pixel_index & 3) << 1))) & 3;
					u8 pixel = ((packed & 2) >> 1) | ((packed & 1) << 1);
					if (pixel != 0) {
						DrawFontPixel(buffer, py + row, px + glyph_positions[glyph] + col, pixel + 250 - color_modifier);
					}
				}
			}
		}
	}
}

IWRAM_CODE ARM_CODE void DrawTextClipped(
	s16 px,
	u8 py,
	u16* text,
	u8 length,
	const u8* nftr_data,
	volatile void* vram,
	s16 clip_left,
	s16 clip_right,
	BOOL highlighted
) {
	volatile u16 *buffer = (volatile u16 *)vram;
	u8 color_modifier = highlighted ? 10 : 0;
	s16 pen_x = px;
	py += FontMarginTop;

	if (clip_left < 0) clip_left = 0;
	if (clip_right > SCREEN_WIDTH) clip_right = SCREEN_WIDTH;
	for (u8 i = 0; i < length && pen_x < clip_right; i++) {
		u16 ch = text[i];
		if (ch == 0x0000 || ch == 0xFFFF) break;
		u16 index = GetFontIndex(ch, nftr_data);
		if (index == 0xFFFF) index = GetFontIndex(FallbackCharacter, nftr_data);
		if (index == 0xFFFF) continue;
		u8 advance = GetFontCharacterWidth(ch, nftr_data);
		const u8 *glyph_data = &nftr_data[
			sFontSpecs.cglp_offset + 0x10 + index * sFontSpecs.bytes_per_char
		];
		u16 pixel_index = 0;
		for (u8 row = 0; row < sFontSpecs.max_height; row++) {
			for (u8 col = 0; col < sFontSpecs.max_width; col++, pixel_index++) {
				s16 screen_x = pen_x + col;
				if (screen_x < clip_left || screen_x >= clip_right || py + row >= SCREEN_HEIGHT) continue;
				if (sFontSpecs.bpp == 1) {
					if ((glyph_data[pixel_index >> 3] >> (7 - (pixel_index & 7))) & 1) {
						DrawFontPixel(buffer, py + row, screen_x, 254 - color_modifier);
					}
				} else if (sFontSpecs.bpp == 2) {
					u8 packed = (glyph_data[pixel_index >> 2] >> (6 - ((pixel_index & 3) << 1))) & 3;
					u8 pixel = ((packed & 2) >> 1) | ((packed & 1) << 1);
					if (pixel != 0) DrawFontPixel(buffer, py + row, screen_x, pixel + 250 - color_modifier);
				}
			}
		}
		pen_x += advance;
	}
}

IWRAM_CODE ARM_CODE void DrawTextLineToBitmap(
	u8 x,
	u8 y,
	u8 width,
	u16* text,
	u8 length,
	const u8* nftr_data,
	u8* bitmap,
	u8 bitmap_width,
	u8 bitmap_height,
	BOOL light_text,
	u8 scale
) {
	u16 glyph_indices[48];
	u8 glyph_positions[48];
	u8 glyph_count = 0;
	u8 line_width = 0;
	if (scale == 0) scale = 1;

	for (u8 i = 0; i < length && glyph_count < 48; i++) {
		u16 ch = text[i];
		if (ch == 0x0000 || ch == 0xFFFF) break;

		u16 index = GetFontIndex(ch, nftr_data);
		if (index == 0xFFFF) index = GetFontIndex(FallbackCharacter, nftr_data);
		if (index == 0xFFFF) continue;

		u8 advance = GetFontCharacterWidth(ch, nftr_data) * scale;
		if (line_width + advance > width) break;
		glyph_indices[glyph_count] = index;
		glyph_positions[glyph_count] = line_width;
		glyph_count++;
		line_width += advance;
	}

	u8 draw_x = x + (width - line_width) / 2;
	for (u8 glyph = 0; glyph < glyph_count; glyph++) {
		const u8 *glyph_data = &nftr_data[
			sFontSpecs.cglp_offset + 0x10 + glyph_indices[glyph] * sFontSpecs.bytes_per_char
		];
		u16 pixel_index = 0;
		for (u8 row = 0; row < sFontSpecs.max_height; row++) {
			for (u8 col = 0; col < sFontSpecs.max_width; col++, pixel_index++) {
				u8 color = 0;
				if (sFontSpecs.bpp == 1) {
					if ((glyph_data[pixel_index >> 3] >> (7 - (pixel_index & 7))) & 1) {
						color = light_text ? 244 : 254;
					}
				} else if (sFontSpecs.bpp == 2) {
					u8 packed = (glyph_data[pixel_index >> 2] >> (6 - ((pixel_index & 3) << 1))) & 3;
					u8 pixel = ((packed & 2) >> 1) | ((packed & 1) << 1);
					if (pixel != 0) color = light_text ? 244 : pixel + 250;
				}
				if (color == 0) continue;

				for (u8 scale_y = 0; scale_y < scale; scale_y++) {
					for (u8 scale_x = 0; scale_x < scale; scale_x++) {
						u8 pixel_x = draw_x + glyph_positions[glyph] + col * scale + scale_x;
						u8 pixel_y = y + row * scale + scale_y;
						if (pixel_x >= x && pixel_x < x + width && pixel_x < bitmap_width && pixel_y < bitmap_height) {
							bitmap[pixel_y * bitmap_width + pixel_x] = color;
						}
					}
				}
			}
		}
	}
}
