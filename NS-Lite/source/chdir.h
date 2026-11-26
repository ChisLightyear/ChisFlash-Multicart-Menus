/*
 * Chimera game directory (CHDR): the list of games on the cart's NOR,
 * shared by the GBA menu (reads), the MCU firmware (reads and writes) and
 * the host tests.  Format: docs/directory-format.md.
 *
 * Two copies live in bucket 0 (the menu's 4 MiB), one per 128 KiB erase
 * block, at the bucket's end so the menu can use the 29 blocks before them.  A copy is valid when its commit record and both CRCs check;
 * the valid copy with the larger seq is current.  A store always writes the
 * other copy and programs its commit record last, so a power cut at any point
 * leaves the previous directory readable.
 *
 * Plain C99, little-endian targets only (ARM7TDMI, AG32 RV32, x86).
 */
#ifndef CHDIR_H_
#define CHDIR_H_

#include <stdint.h>

/* NOR 擦除块、目录写入分段与窗口大小。 */
#define CHD_NOR_BLOCK_BYTES     0x20000u    /* erase block, 128 KiB */
#define CHD_NOR_PAGE_BYTES      0x400u      /* write buffer, 512 words */
#define CHD_BUCKET_BYTES        0x400000u   /* one mapper window, 4 MiB */
#define CHD_BUCKET_COUNT        160u        /* 五片 01G，每片 32 个 4 MiB 块 */
#define CHD_FIRST_GAME_BUCKET   1u          /* bucket 0 = menu + metadata */

/* ---- placement inside bucket 0 ------------------------------------------- */
#define CHD_COPY_A_ADDR         (29u * CHD_NOR_BLOCK_BYTES)  /* 0x3A0000 */
#define CHD_COPY_B_ADDR         (30u * CHD_NOR_BLOCK_BYTES)  /* 0x3C0000 */
#define CHD_STATUS_ADDR         (31u * CHD_NOR_BLOCK_BYTES)  /* 0x3E0000, menu's last-boot record */
#define CHD_MENU_MAX_BYTES      CHD_COPY_A_ADDR              /* menu image in blocks 0-28, 3.625 MiB */
/* Layout before 2026-11-26 (menu limited to 1 MiB): the firmware moves a
 * directory it finds there to the places above (chd_read_legacy). */
#define CHD_LEGACY_COPY_A_ADDR  (8u * CHD_NOR_BLOCK_BYTES)   /* 0x100000 */
#define CHD_LEGACY_COPY_B_ADDR  (9u * CHD_NOR_BLOCK_BYTES)   /* 0x120000 */
#define CHD_LEGACY_STATUS_ADDR  (10u * CHD_NOR_BLOCK_BYTES)  /* 0x140000 */
#define CHD_LEGACY_MENU_MAX     CHD_LEGACY_COPY_A_ADDR
#define CHD_COMMIT_OFFSET       0x10000u    /* commit record, inside the copy's block */

/* ---- limits ---------------------------------------------------------------- */
#define CHD_ENTRY_COUNT         63u         /* entry i uses save slot i, forever */
#define CHD_MAX_GAME_BUCKETS    8u          /* 32 MiB, the GBA ROM space */
#define CHD_NAME_UNITS          64u         /* UTF-16 code units, no terminator */

#define CHD_MAGIC               0x52444843u /* "CHDR" */
#define CHD_COMMIT_MAGIC        0x4D434843u /* "CHCM" */
#define CHD_VERSION             2u

enum {
    CHD_SAVE_NONE = 0,
    CHD_SAVE_SRAM = 1,      /* the cart gives every game 128 KiB virtual SRAM */
};

typedef struct chd_entry {
    uint8_t  in_use;                        /* 1 = a game, 0 = free */
    uint8_t  bucket_count;                  /* 1..8 */
    uint8_t  buckets[CHD_MAX_GAME_BUCKETS]; /* 各窗口对应的片选和块号，高 die 见 upper_die_mask */
    uint8_t  save_type;                     /* CHD_SAVE_* */
    uint8_t  name_len;                      /* code units in name[] */
    uint32_t order;                         /* display order, smaller first */
    uint32_t rom_size;                      /* bytes as written */
    uint32_t rom_crc32;                     /* CRC-32 of those bytes */
    uint8_t  game_code[4];                  /* header 0xAC */
    uint8_t  upper_die_mask;                /* 各窗口的高 die 选择位 */
    uint8_t  cover_bank;                    /* 0 为无封面，A1/A2 为封面扇区 */
    uint8_t  reserved[10];
    uint16_t name[CHD_NAME_UNITS];          /* file name without ".gba" */
} chd_entry;                                /* 168 bytes */

typedef struct chd_save_owner {
    uint8_t  game_code[4];                  /* whose save is in slot i */
    uint32_t rom_crc32;
} chd_save_owner;                           /* 8 bytes; all 0xFF = no save yet */

typedef struct chd_dir {
    uint32_t magic;                         /* CHD_MAGIC */
    uint16_t version;                       /* CHD_VERSION */
    uint16_t entry_size;                    /* sizeof(chd_entry) */
    uint32_t seq;                           /* larger = newer */
    uint32_t next_order;
    uint8_t  entry_count;                   /* CHD_ENTRY_COUNT */
    uint8_t  reserved[3];
    uint32_t menu_size;                     /* bytes of the menu image at NOR 0, 0 = unknown */
    uint8_t  reserved2[8];
    chd_save_owner owners[CHD_ENTRY_COUNT];
    chd_entry entries[CHD_ENTRY_COUNT];
} chd_dir;

typedef struct chd_commit {
    uint32_t magic;                         /* CHD_COMMIT_MAGIC */
    uint32_t seq;                           /* == dir.seq */
    uint32_t body_len;                      /* sizeof(chd_dir) */
    uint32_t body_crc32;
    uint32_t reserved[3];
    uint32_t self_crc32;                    /* over the 28 bytes before it */
} chd_commit;                               /* 32 bytes */

/* NOR access, supplied by the caller.  Addresses are byte offsets from the
 * start of the NOR.  program() is only ever called on erased bytes, with an
 * even addr and len, inside one CHD_NOR_PAGE_BYTES page.  Return 0 on success. */
typedef struct chd_io {
    void *ctx;
    int (*read)(void *ctx, uint32_t addr, void *buf, uint32_t len);
    int (*erase_block)(void *ctx, uint32_t addr);
    int (*program)(void *ctx, uint32_t addr, const void *buf, uint32_t len);
} chd_io;

enum {
    CHD_OK = 0,
    CHD_ERR_NONE_VALID = -1,    /* neither copy valid: unformatted or both broken */
    CHD_ERR_IO = -2,
    CHD_ERR_VERIFY = -3,        /* stored copy did not read back valid */
    CHD_ERR_INVALID = -4,       /* directory content fails chd_dir_sane() */
};

uint32_t chd_crc32(uint32_t crc, const void *data, uint32_t len);   /* start with 0 */

/* Empty directory (seq 0, no games, no save owners). */
void chd_dir_init(chd_dir *dir);

/* Content checks shared by load and store: ids in range, no bucket owned
 * twice, counts and name lengths in range.  Returns 1 if sane. */
int chd_dir_sane(const chd_dir *dir);

/* Read and check one copy (0 = A, 1 = B). Returns CHD_OK and fills *dir. */
int chd_read_copy(const chd_io *io, int copy, chd_dir *dir);

/* Which copy is current (0/1), or -1 if neither is valid; *io_error set on
 * a read failure.  On success *scratch holds the current copy. */
int chd_find_current(const chd_io *io, chd_dir *scratch, int *io_error);

/* Load the current directory.  *copy (may be NULL) gets 0/1 for the copy
 * that was used.  CHD_ERR_NONE_VALID if neither copy checks. */
int chd_load(const chd_io *io, chd_dir *dir, int *copy);

/* The newer valid copy at the pre-2026-11-26 places (CHD_LEGACY_*), or
 * CHD_ERR_NONE_VALID / CHD_ERR_IO. */
int chd_read_legacy(const chd_io *io, chd_dir *dir);

/* Store dir as the next version: seq = current seq + 1, written to the copy
 * that is not current, commit record last, then read back.  dir->seq is
 * updated.  *copy (may be NULL) gets the copy written.
 * In chdir_write.c. */
int chd_store(const chd_io *io, chd_dir *dir, int *copy);

/* 按实际物理片选记录块号，空焊位不占容量。 */
unsigned chd_bucket(const chd_entry *entry, unsigned window);
void chd_set_bucket(chd_entry *entry, unsigned window, unsigned bucket, int upper_die);
void chd_used_buckets(const chd_dir *dir, uint32_t used[CHD_BUCKET_COUNT / 32u]);

/* First free entry index, or -1. */
int chd_free_entry(const chd_dir *dir);

#endif
