#ifndef CHD_COVER_H
#define CHD_COVER_H
#include "chdir.h"

#define COVER_SIDE 56u
#define COVER_BYTES (COVER_SIDE * COVER_SIDE)
#define COVER_FIRST_ADDR (CHD_COPY_A_ADDR - 4u * CHD_NOR_BLOCK_BYTES)
#define COVER_GROUP_SIZE 32u
#define COVER_BANK_A 0xa1u
#define COVER_BANK_B 0xa2u

static inline int chd_has_cover(const chd_entry *e)
{
    return e->in_use && (e->cover_bank == COVER_BANK_A || e->cover_bank == COVER_BANK_B);
}
static inline uint32_t chd_cover_addr(unsigned slot, unsigned bank)
{
    unsigned block = 2u * (slot / COVER_GROUP_SIZE) + (bank == COVER_BANK_B);
    return COVER_FIRST_ADDR + block * CHD_NOR_BLOCK_BYTES + (slot % COVER_GROUP_SIZE) * 4096u;
}

/* 与 NS 菜单既有的 6×6×5 调色板一致，索引 1 是白色。 */
static inline uint8_t cover_index(unsigned r, unsigned g, unsigned b)
{
    if (r >= 250 && g >= 250 && b >= 250) return 1;
    return (uint8_t)(32 + ((r * 6 / 256) * 6 + g * 6 / 256) * 5 + b * 5 / 256);
}
static inline uint16_t cover_rgb555(unsigned index)
{
    if (index == 1) return 0x7fff;
    if (index < 32 || index >= 212) return 0;
    unsigned n = index - 32;
    return ((n / 30 * 31 + 2) / 5) |
           (((n / 5 % 6 * 31 + 2) / 5) << 5) | (((n % 5 * 31 + 2) / 4) << 10);
}
#endif
