/* Delta updates: download only what changed between two archive versions.
 *
 * Both archives are split into content-defined chunks (gear rolling hash,
 * ~16 KiB average) — boundaries depend only on content, so an insertion
 * shifts the following bytes without invalidating their chunks. The delta is
 * a program that rebuilds the new archive from COPY ranges of the old one
 * plus LITERAL bytes for chunks the old archive does not contain. Applying
 * it needs nothing but the old archive we already keep in the store, and the
 * result is verified against the new archive's sha256 from the index, so a
 * delta can never install anything a full download would not.
 *
 * Archives compressed with `gzip --rsyncable` keep unchanged file regions
 * byte-identical between releases, which is what makes deltas small.
 *
 * Format (all integers little-endian):
 *   "WARPDLT1" | old_sha256[64] | new_sha256[64] | new_size u64
 *   ops: 0x01 COPY off u64, len u64 | 0x02 LIT len u64, bytes | 0x00 END
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "warp.h"

#define DELTA_MAGIC     "WARPDLT1"
#define CHUNK_MIN       (2 * 1024)
#define CHUNK_MAX       (64 * 1024)
#define CHUNK_MASK      0x0000000000003FFFULL   /* avg 16 KiB */
#define OP_END          0x00
#define OP_COPY         0x01
#define OP_LIT          0x02

/* Deterministic gear table: both sides must derive identical boundaries. */
static uint64_t gear[256];
static void gear_init(void) {
    if (gear[1]) return;
    uint64_t x = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 256; i++) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        gear[i] = x * 0x2545F4914F6CDD1DULL;
    }
}

/* Length of the chunk starting at data[0..len). */
static size_t next_chunk(const uint8_t *data, size_t len) {
    if (len <= CHUNK_MIN) return len;
    size_t limit = len < CHUNK_MAX ? len : CHUNK_MAX;
    uint64_t h = 0;
    for (size_t i = 0; i < limit; i++) {
        h = (h << 1) + gear[data[i]];
        if (i >= CHUNK_MIN && (h & CHUNK_MASK) == 0) return i + 1;
    }
    return limit;
}

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

typedef struct { uint64_t hash; uint64_t off; uint32_t len; uint32_t used; } chunk_ref_t;

typedef struct {
    uint8_t *data;
    size_t   len;
} mapped_t;

static int map_file(const char *path, mapped_t *m) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return WARP_ERR_IO;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return WARP_ERR_IO; }
    m->len = (size_t)st.st_size;
    m->data = m->len ? mmap(NULL, m->len, PROT_READ, MAP_PRIVATE, fd, 0) : NULL;
    close(fd);
    if (m->len && m->data == MAP_FAILED) return WARP_ERR_IO;
    return WARP_OK;
}

static void unmap_file(mapped_t *m) {
    if (m->data && m->len) munmap(m->data, m->len);
}

static void put_u64(FILE *f, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    fwrite(b, 1, 8, f);
}

static int get_u64(FILE *f, uint64_t *v) {
    uint8_t b[8];
    if (fread(b, 1, 8, f) != 8) return -1;
    *v = 0;
    for (int i = 0; i < 8; i++) *v |= (uint64_t)b[i] << (8 * i);
    return 0;
}

/* Open-addressing hash table over the old archive's chunks. */
static chunk_ref_t *build_table(const mapped_t *old, size_t *table_size) {
    size_t approx = old->len / CHUNK_MIN + 16;
    size_t size = 1;
    while (size < approx * 2) size <<= 1;
    chunk_ref_t *table = calloc(size, sizeof(chunk_ref_t));
    if (!table) return NULL;
    size_t off = 0;
    while (off < old->len) {
        size_t n = next_chunk(old->data + off, old->len - off);
        uint64_t h = fnv1a(old->data + off, n);
        size_t slot = h & (size - 1);
        while (table[slot].used) slot = (slot + 1) & (size - 1);
        table[slot] = (chunk_ref_t){ .hash = h, .off = off, .len = (uint32_t)n, .used = 1 };
        off += n;
    }
    *table_size = size;
    return table;
}

static const chunk_ref_t *lookup(const chunk_ref_t *table, size_t size, const mapped_t *old,
                                 const uint8_t *chunk, size_t n) {
    uint64_t h = fnv1a(chunk, n);
    size_t slot = h & (size - 1);
    while (table[slot].used) {
        const chunk_ref_t *c = &table[slot];
        if (c->hash == h && c->len == n && memcmp(old->data + c->off, chunk, n) == 0) return c;
        slot = (slot + 1) & (size - 1);
    }
    return NULL;
}

typedef struct {
    FILE    *out;
    uint64_t lit_start, lit_len;     /* pending literal run in new */
    uint64_t copy_off, copy_len;     /* pending copy run in old */
    const uint8_t *new_data;
    uint64_t literal_bytes, copied_bytes;
} emitter_t;

static void flush_lit(emitter_t *e) {
    if (!e->lit_len) return;
    fputc(OP_LIT, e->out);
    put_u64(e->out, e->lit_len);
    fwrite(e->new_data + e->lit_start, 1, e->lit_len, e->out);
    e->literal_bytes += e->lit_len;
    e->lit_len = 0;
}

static void flush_copy(emitter_t *e) {
    if (!e->copy_len) return;
    fputc(OP_COPY, e->out);
    put_u64(e->out, e->copy_off);
    put_u64(e->out, e->copy_len);
    e->copied_bytes += e->copy_len;
    e->copy_len = 0;
}

int delta_make(const char *old_path, const char *new_path, const char *out_path) {
    gear_init();
    mapped_t old = {0}, new = {0};
    if (map_file(old_path, &old) != WARP_OK) { warp_err("Cannot read %s", old_path); return WARP_ERR_IO; }
    if (map_file(new_path, &new) != WARP_OK) { warp_err("Cannot read %s", new_path); unmap_file(&old); return WARP_ERR_IO; }

    char old_sha[WARP_SHA256_HEX], new_sha[WARP_SHA256_HEX];
    warp_sha256_buf(old.data, old.len, old_sha);
    warp_sha256_buf(new.data, new.len, new_sha);

    size_t table_size = 0;
    chunk_ref_t *table = build_table(&old, &table_size);
    if (!table) { unmap_file(&old); unmap_file(&new); return WARP_ERR_IO; }

    FILE *out = fopen(out_path, "wb");
    if (!out) { free(table); unmap_file(&old); unmap_file(&new); return WARP_ERR_IO; }
    fwrite(DELTA_MAGIC, 1, 8, out);
    fwrite(old_sha, 1, 64, out);
    fwrite(new_sha, 1, 64, out);
    put_u64(out, new.len);

    emitter_t e = { .out = out, .new_data = new.data };
    size_t off = 0;
    while (off < new.len) {
        size_t n = next_chunk(new.data + off, new.len - off);
        const chunk_ref_t *c = lookup(table, table_size, &old, new.data + off, n);
        if (c) {
            flush_lit(&e);
            if (e.copy_len && e.copy_off + e.copy_len == c->off) {
                e.copy_len += n;                 /* extend contiguous copy */
            } else {
                flush_copy(&e);
                e.copy_off = c->off;
                e.copy_len = n;
            }
        } else {
            flush_copy(&e);
            if (!e.lit_len) e.lit_start = off;
            e.lit_len += n;
        }
        off += n;
    }
    flush_lit(&e);
    flush_copy(&e);
    fputc(OP_END, out);
    fclose(out);
    free(table);
    unmap_file(&old);
    unmap_file(&new);

    long dsize = file_size(out_path);
    warp_ok("Delta: %s", out_path);
    printf("  new archive:  %zu bytes\n", new.len);
    printf("  reused (old): %llu bytes, literal: %llu bytes\n",
           (unsigned long long)e.copied_bytes, (unsigned long long)e.literal_bytes);
    printf("  delta size:   %ld bytes (%.1f%% of the full download)\n\n",
           dsize, new.len ? 100.0 * (double)dsize / (double)new.len : 0.0);
    return WARP_OK;
}

/* Rebuild the new archive; `expected_new_sha` (optional) must match the
 * header. The output's real sha256 is returned in out_sha for the caller to
 * check against the index — the header is a hint, not a proof. */
int delta_apply(const char *old_path, const char *delta_path, const char *out_path,
                const char *expected_new_sha, char out_sha[WARP_SHA256_HEX]) {
    mapped_t old = {0};
    if (map_file(old_path, &old) != WARP_OK) { warp_err("Cannot read %s", old_path); return WARP_ERR_IO; }
    FILE *d = fopen(delta_path, "rb");
    if (!d) { unmap_file(&old); return WARP_ERR_IO; }

    char magic[8], old_sha[65] = {0}, new_sha[65] = {0};
    uint64_t new_size = 0;
    if (fread(magic, 1, 8, d) != 8 || memcmp(magic, DELTA_MAGIC, 8) != 0 ||
        fread(old_sha, 1, 64, d) != 64 || fread(new_sha, 1, 64, d) != 64 || get_u64(d, &new_size) != 0) {
        warp_err("Not a WARP delta file: %s", delta_path);
        fclose(d); unmap_file(&old);
        return WARP_ERR_INVAL;
    }
    char have_old[WARP_SHA256_HEX];
    warp_sha256_buf(old.data, old.len, have_old);
    if (strcmp(have_old, old_sha) != 0) {
        warp_err("Delta was made against a different old archive");
        fclose(d); unmap_file(&old);
        return WARP_ERR_HASH;
    }
    if (expected_new_sha && strcmp(expected_new_sha, new_sha) != 0) {
        warp_err("Delta targets a different new archive than the index lists");
        fclose(d); unmap_file(&old);
        return WARP_ERR_HASH;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) { fclose(d); unmap_file(&old); return WARP_ERR_IO; }

    int rc = WARP_OK;
    uint64_t written = 0;
    uint8_t *buf = malloc(CHUNK_MAX);
    for (;;) {
        int op = fgetc(d);
        if (op == EOF) { rc = WARP_ERR_INVAL; break; }
        if (op == OP_END) break;
        if (op == OP_COPY) {
            uint64_t off, len;
            if (get_u64(d, &off) || get_u64(d, &len) || off + len > old.len || off + len < off) { rc = WARP_ERR_INVAL; break; }
            if (len && fwrite(old.data + off, 1, len, out) != len) { rc = WARP_ERR_IO; break; }
            written += len;
        } else if (op == OP_LIT) {
            uint64_t len;
            if (get_u64(d, &len)) { rc = WARP_ERR_INVAL; break; }
            while (len) {
                size_t n = len < CHUNK_MAX ? (size_t)len : CHUNK_MAX;
                if (fread(buf, 1, n, d) != n || fwrite(buf, 1, n, out) != n) { rc = WARP_ERR_INVAL; break; }
                len -= n; written += n;
            }
            if (rc != WARP_OK) break;
        } else { rc = WARP_ERR_INVAL; break; }
    }
    free(buf);
    fclose(out);
    fclose(d);
    unmap_file(&old);

    if (rc == WARP_OK && written != new_size) rc = WARP_ERR_INVAL;
    if (rc != WARP_OK) {
        warp_err("Delta is corrupt or truncated");
        remove(out_path);
        return rc;
    }
    if (out_sha) warp_sha256_file(out_path, out_sha);
    return WARP_OK;
}

/* warp delta <old.warp> <new.warp> <out.warpdelta> */
int cmd_delta(int argc, char **argv) {
    if (argc < 3) { warp_err("Usage: warp delta <old.warp> <new.warp> <out.warpdelta>"); return 1; }
    return delta_make(argv[0], argv[1], argv[2]) == WARP_OK ? 0 : 1;
}

/* warp delta-apply <old.warp> <delta> <out.warp>  (debugging aid) */
int cmd_delta_apply(int argc, char **argv) {
    if (argc < 3) { warp_err("Usage: warp delta-apply <old.warp> <delta> <out.warp>"); return 1; }
    char sha[WARP_SHA256_HEX];
    if (delta_apply(argv[0], argv[1], argv[2], NULL, sha) != WARP_OK) return 1;
    warp_ok("Rebuilt %s (sha256 %s)", argv[2], sha);
    return 0;
}
