/* Chimera game directory codec.  See chdir.h and docs/directory-format.md. */
#include "chdir.h"

#include <stddef.h>
#include <string.h>

typedef char chd_assert_entry_size[(sizeof(chd_entry) == 168) ? 1 : -1];
typedef char chd_assert_owner_size[(sizeof(chd_save_owner) == 8) ? 1 : -1];
typedef char chd_assert_commit_size[(sizeof(chd_commit) == 32) ? 1 : -1];
typedef char chd_assert_menu_size_at[(offsetof(chd_dir, menu_size) == 20 && offsetof(chd_dir, owners) == 32) ? 1 : -1];
typedef char chd_assert_dir_fits[(sizeof(chd_dir) < CHD_COMMIT_OFFSET) ? 1 : -1];

static const uint32_t crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t chd_crc32(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nibble[crc & 15u];
        crc = (crc >> 4) ^ crc_nibble[crc & 15u];
    }
    return ~crc;
}

static uint32_t copy_addr(int copy)
{
    return copy ? CHD_COPY_B_ADDR : CHD_COPY_A_ADDR;
}

void chd_dir_init(chd_dir *dir)
{
    memset(dir, 0, sizeof(*dir));
    dir->magic = CHD_MAGIC;
    dir->version = CHD_VERSION;
    dir->entry_size = (uint16_t)sizeof(chd_entry);
    dir->entry_count = CHD_ENTRY_COUNT;
    memset(dir->owners, 0xFF, sizeof(dir->owners));
}

int chd_dir_sane(const chd_dir *dir)
{
    uint32_t used[CHD_BUCKET_COUNT / 32u] = {0};
    unsigned i, j;

    if (dir->magic != CHD_MAGIC || (dir->version != 1u && dir->version != CHD_VERSION) ||
        dir->entry_size != sizeof(chd_entry) || dir->entry_count != CHD_ENTRY_COUNT)
        return 0;
    for (i = 0; i < CHD_ENTRY_COUNT; i++) {
        const chd_entry *e = &dir->entries[i];
        if (e->in_use == 0)
            continue;
        if (e->in_use != 1 || e->bucket_count == 0 || e->bucket_count > CHD_MAX_GAME_BUCKETS ||
            e->name_len == 0 || e->name_len > CHD_NAME_UNITS ||
            e->rom_size == 0 || e->rom_size > (uint32_t)e->bucket_count * CHD_BUCKET_BYTES ||
            e->rom_size <= (uint32_t)(e->bucket_count - 1u) * CHD_BUCKET_BYTES)
            return 0;
        for (j = 0; j < e->bucket_count; j++) {
            unsigned b = chd_bucket(e, j);
            if (((e->upper_die_mask >> j) & 1u) && e->buckets[j] >= 32u) return 0;
            if (b < CHD_FIRST_GAME_BUCKET || b >= CHD_BUCKET_COUNT || (dir->version == 1u && b >= 32u) || (used[b / 32u] & (1u << (b % 32u))))
                return 0;
            used[b / 32u] |= 1u << (b % 32u);
        }
    }
    return 1;
}

static int read_copy_at(const chd_io *io, uint32_t base, chd_dir *dir)
{
    chd_commit c;

    if (io->read(io->ctx, base + CHD_COMMIT_OFFSET, &c, sizeof(c)))
        return CHD_ERR_IO;
    const uint32_t old_count = 31u;
    const uint32_t old_len = offsetof(chd_dir, owners) + old_count * (sizeof(chd_save_owner) + sizeof(chd_entry));
    if (c.magic != CHD_COMMIT_MAGIC ||
        chd_crc32(0, &c, (uint32_t)offsetof(chd_commit, self_crc32)) != c.self_crc32 ||
        (c.body_len != sizeof(chd_dir) && c.body_len != old_len))
        return CHD_ERR_NONE_VALID;
    if (io->read(io->ctx, base, dir, c.body_len))
        return CHD_ERR_IO;
    if (chd_crc32(0, dir, c.body_len) != c.body_crc32 || dir->seq != c.seq)
        return CHD_ERR_NONE_VALID;
    if (c.body_len == old_len) {
        if (dir->version != 1u || dir->entry_count != old_count) return CHD_ERR_NONE_VALID;
        memmove(dir->entries, dir->owners + old_count, old_count * sizeof(chd_entry));
        memset(dir->owners + old_count, 0xff, (CHD_ENTRY_COUNT - old_count) * sizeof(chd_save_owner));
        memset(dir->entries + old_count, 0, (CHD_ENTRY_COUNT - old_count) * sizeof(chd_entry));
        for (unsigned i=0;i<old_count;i++) dir->entries[i].cover_bank=0;
        dir->entry_count = CHD_ENTRY_COUNT;
    }
    if (!chd_dir_sane(dir))
        return CHD_ERR_NONE_VALID;
    dir->version = CHD_VERSION;
    return CHD_OK;
}

int chd_read_copy(const chd_io *io, int copy, chd_dir *dir)
{
    return read_copy_at(io, copy_addr(copy), dir);
}

int chd_read_legacy(const chd_io *io, chd_dir *dir)
{
    uint32_t seq_a = 0;
    int a = read_copy_at(io, CHD_LEGACY_COPY_A_ADDR, dir);
    if (a == CHD_OK) seq_a = dir->seq;
    int b = read_copy_at(io, CHD_LEGACY_COPY_B_ADDR, dir);
    if (b == CHD_OK && (a != CHD_OK || dir->seq > seq_a)) return CHD_OK;
    if (a == CHD_OK) return read_copy_at(io, CHD_LEGACY_COPY_A_ADDR, dir);
    return (a == CHD_ERR_IO || b == CHD_ERR_IO) ? CHD_ERR_IO : CHD_ERR_NONE_VALID;
}

int chd_find_current(const chd_io *io, chd_dir *scratch, int *io_error)
{
    uint32_t seq_a = 0;
    int a, b;

    *io_error = 0;
    a = chd_read_copy(io, 0, scratch);
    if (a == CHD_ERR_IO)
        *io_error = 1;
    if (a == CHD_OK)
        seq_a = scratch->seq;
    b = chd_read_copy(io, 1, scratch);
    if (b == CHD_ERR_IO)
        *io_error = 1;
    if (b == CHD_OK && (a != CHD_OK || scratch->seq > seq_a))
        return 1;           /* scratch already holds B */
    if (a == CHD_OK)
        return chd_read_copy(io, 0, scratch) == CHD_OK ? 0 : -1;
    return -1;
}

int chd_load(const chd_io *io, chd_dir *dir, int *copy)
{
    int io_error;
    int cur = chd_find_current(io, dir, &io_error);

    if (cur < 0)
        return io_error ? CHD_ERR_IO : CHD_ERR_NONE_VALID;
    if (copy)
        *copy = cur;
    return CHD_OK;
}

unsigned chd_bucket(const chd_entry *entry, unsigned window)
{
    unsigned id = entry->buckets[window];
    unsigned die = (entry->upper_die_mask >> window) & 1u;
    return id + die * 32u;
}

void chd_set_bucket(chd_entry *entry, unsigned window, unsigned bucket, int upper_die)
{
    unsigned flag = 1u << window;
    entry->buckets[window] = (uint8_t)(upper_die ? bucket - 32u : bucket);
    entry->upper_die_mask = (uint8_t)((entry->upper_die_mask & ~flag) |
                                      (upper_die ? flag : 0u));
}

void chd_used_buckets(const chd_dir *dir, uint32_t used[CHD_BUCKET_COUNT / 32u])
{
    memset(used, 0, CHD_BUCKET_COUNT / 8u);
    used[0] = 1u;
    for (unsigned i = 0; i < CHD_ENTRY_COUNT; ++i) {
        const chd_entry *e = &dir->entries[i];
        if (!e->in_use) continue;
        for (unsigned j = 0; j < e->bucket_count; ++j) {
            unsigned b = chd_bucket(e, j);
            used[b / 32u] |= 1u << (b % 32u);
        }
    }
}

int chd_free_entry(const chd_dir *dir)
{
    unsigned i;
    for (i = 0; i < CHD_ENTRY_COUNT; i++)
        if (!dir->entries[i].in_use)
            return (int)i;
    return -1;
}
