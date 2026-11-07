/* Importer core internals shared by the setup library's own files (not part of the stage ABI in setup.h). */
#ifndef TOTK_SETUP_INTERNAL_H
#define TOTK_SETUP_INTERNAL_H
#include "setup.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/stat.h>

#define SETUP_PATH 1024
#define SETUP_MAX_ROMFS_PARTS 64
#define SETUP_MAX_EXEFS 8
#define SETUP_MAX_FONTS 6

/* ---- SHA-256 (sha256.c; ARMv8 SHA2 instructions when the CPU has them) ---- */
typedef struct { uint32_t h[8]; uint64_t bytes; uint8_t buf[64]; size_t used; } Sha256;
void sha256_init(Sha256 *);
void sha256_update(Sha256 *, const void *, size_t);
void sha256_final(Sha256 *, uint8_t out[32]);
void sha256_hex(const uint8_t d[32], char out[65]);
void sha256_buf_hex(const void *, size_t, char out[65]);

/* ---- AES-128 (aes.c): only what the NCA header / ticket / key-area checks need ---- */
typedef struct { uint32_t rk[44]; } Aes128;
void aes128_init_enc(Aes128 *, const uint8_t key[16]);
void aes128_init_dec(Aes128 *, const uint8_t key[16]);
void aes128_encrypt(const Aes128 *, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const Aes128 *, const uint8_t in[16], uint8_t out[16]);
void aes128_ecb_decrypt(const uint8_t key[16], uint8_t *data, size_t len);
/* Nintendo AES-XTS: 0x200-byte sectors, tweak = big-endian sector number. key = data key || tweak key. */
void aes128_xts_decrypt(const uint8_t key[32], uint64_t first_sector, uint8_t *data, size_t len);
/* AES-CTR with a 16-byte big-endian counter block. */
void aes128_ctr(const uint8_t key[16], const uint8_t ctr[16], uint8_t *data, size_t len);

/* ---- JSON (json.c): a small DOM reader and an output buffer ---- */
enum { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ };
typedef struct Json { int type; int b; double num; int is_int; int64_t i; char *s; size_t len;
                      struct Json *items; char **keys; size_t n; } Json;
Json *json_parse(const char *text, size_t len, char *err, size_t errcap);
void json_free(Json *);
const Json *json_get(const Json *obj, const char *key);
const char *json_str(const Json *, const char *key);              /* NULL unless a string */
int json_int(const Json *, const char *key, int64_t *out);         /* 0 when an integer */
int json_bool(const Json *, const char *key);                      /* 1 only for true */
typedef struct { char *p; size_t n, cap; int oom; } Buf;
void buf_add(Buf *, const char *s, size_t n);
void buf_puts(Buf *, const char *s);
void buf_printf(Buf *, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_jstr(Buf *, const char *s);   /* quoted JSON string, ASCII-only output (\u escapes; invalid UTF-8 -> U+FFFD) */
void buf_free(Buf *);

/* ---- pinned values (totk_pins.h by default; the host CLI may load a test pin file) ---- */
enum { EXEFS_MAIN, EXEFS_SDK, EXEFS_SUBSDK0, EXEFS_NPDM, EXEFS_RTLD, EXEFS_COUNT };
extern const char *const setup_exefs_names[EXEFS_COUNT];
typedef struct { char title_id[17]; uint64_t version, romfs_size; char romfs_sha256[65], index_sha256[65];
                 char exefs[EXEFS_COUNT][65], rewritten[3][65], tables[65]; } SetupPins;
void setup_pins_default(SetupPins *);
int setup_pins_load(SetupPins *, const char *json_path, char *err, size_t errcap);   /* expected-v*.json format */

/* ---- request ---- */
typedef struct { int fd; uint64_t offset, size; char name[256]; int present, owned; } SetupInput;   /* owned: opened from "path" */
typedef struct {
    char game[SETUP_PATH], priv[SETUP_PATH], nobackup[SETUP_PATH], lib[SETUP_PATH];
    SetupInput romfs[SETUP_MAX_ROMFS_PARTS]; int nromfs;
    SetupInput exefs[SETUP_MAX_EXEFS]; int nexefs;
    SetupInput base, update, keys;
    SetupInput fonts[SETUP_MAX_FONTS]; int nfonts;
    int stage_mask, stages_explicit, skip_companion; /* automatic setup on a device without a built-in second screen */
    SetupInput shader_list;                 /* optional; the seed stage reads it through setup_shader_list */
    uint64_t romfs_part_bytes;              /* "romfsPartBytes": RomFS parts of this size in <game>; 0 = one romfs.bin */
} SetupRequest;
int setup_request_parse(SetupRequest *, const char *json, size_t len, char *err, size_t errcap);
void setup_request_close(SetupRequest *);   /* closes the descriptors the parser opened from "path" inputs */

/* ---- context ---- */
typedef void (*SetupProgressFn)(void *user, const char *stage, uint64_t done, uint64_t total, const char *detail);
struct SetupCtx {
    char game[SETUP_PATH], lib[SETUP_PATH], staging[SETUP_PATH], nobackup[SETUP_PATH], keydir[SETUP_PATH];
    atomic_int *cancel;
    SetupProgressFn progress; void *progress_user;
    int failed; char fail_code[32], fail_msg[1024];
    SetupPins pins;
    const SetupRequest *req;
};
void setup_ctx_init(SetupCtx *, const char *game, const char *lib, const char *nobackup, atomic_int *cancel);
void setup_clear_fail(SetupCtx *);

/* ---- files (util.c) ---- */
int st_path(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));  /* -1 if too long */
int st_pread(int fd, void *buf, size_t n, uint64_t off);        /* 0, or -1 with errno (EIO = short read) */
int st_write(int fd, const void *buf, size_t n);
int st_mkdirs(const char *path);
int st_rmtree(const char *path);                                /* missing path is fine */
int st_fsync_dir(const char *path);
int st_write_file(const char *path, const void *data, size_t n, int mode);   /* create/truncate, fsync */
int st_write_atomic(const char *path, const void *data, size_t n);          /* tmp + fsync + rename + dir fsync */
int st_commit(SetupCtx *, const char *from, const char *to);    /* rename + fsync of the target directory */
int st_exists(const char *path);
int64_t st_file_size(const char *path);                         /* -1 if missing */
int st_read_file(const char *path, char **out, size_t *len, size_t max);
uint64_t st_free_bytes(const char *path);
uint64_t st_now_ms(void);
int st_sha256_fd(SetupCtx *, int fd, uint64_t off, uint64_t len, uint8_t out[32], const char *stage, uint64_t base_done, uint64_t total);
int st_sha256_file(SetupCtx *, const char *path, char hex[65], int64_t *size);
/* Page-cache bounding for multi-GB streams. Writing or reading 17 GB with only a final fsync leaves gigabytes of cache
   behind; Android's low-memory killer counts that pressure and kills the setup process as soon as its window is not on
   screen (seen on the AYN Thor with the display off). Writers call st_cache_written after each write; every 128 MiB
   the finished window is written back and dropped while the next one fills. Readers drop what they have consumed. */
typedef struct { uint64_t mark, prev_start, prev_end; } StCache;
void st_cache_written(int fd, StCache *w, uint64_t written_end);   /* written_end: file offset written so far */
void st_cache_flush_range(int fd, uint64_t off, uint64_t len);     /* write back (waiting) and drop one range */
void st_cache_drop(int fd, uint64_t off, uint64_t len);            /* drop clean cached pages of a range just read */
/* setup_fail for an errno failure, with a short operation description. */
int st_io_fail(SetupCtx *, const char *what, const char *path);

/* ---- the RomFS image on disk (romfs_image.c): <dir>/romfs.bin, or equal-size parts <dir>/romfs.bin.000 ... (the last one
   shorter) as runtime/romfs.c reads them. FAT32 cannot hold the 17 GB image in one file; every setup reader takes both. */
#define ROMFS_FAT_PART_BYTES 4294901760ull   /* 0xFFFF0000: under FAT32's 4 GiB - 1 file size limit, 64 KiB aligned */
typedef struct { int fd[SETUP_MAX_ROMFS_PARTS]; int nparts; uint64_t part_size, size; } RomfsImage;   /* nparts 0: one file, fd[0] */
int romfs_image_open(RomfsImage *, const char *dir);          /* -1 with errno: ENOENT neither layout, EINVAL a bad part set */
int romfs_image_open_file(RomfsImage *, const char *path);    /* one image file of any name */
void romfs_image_close(RomfsImage *);
int romfs_image_pread(const RomfsImage *, void *buf, size_t n, uint64_t off);   /* st_pread semantics across the parts */
void romfs_image_drop(const RomfsImage *, uint64_t off, uint64_t len);         /* st_cache_drop of an image range */
int romfs_image_stat(const RomfsImage *, int part, struct stat *);
int romfs_parts_for(uint64_t size, uint64_t part_bytes);       /* 0 (one file) for part_bytes 0; -1 above SETUP_MAX_ROMFS_PARTS */
int romfs_part_name(char *out, size_t cap, const char *dir, int nparts, int part);   /* romfs.bin or romfs.bin.NNN */
/* Removes the image files in dir that are not part of the kept layout: keep_parts -1 = all of them, 0 = all but romfs.bin,
   n = all but romfs.bin.000 ... n-1 (a stale romfs.bin would shadow parts; a stale higher part would be read as one). */
int romfs_remove_stale(const char *dir, int keep_parts);
/* Writes the image into dir in the layout part_bytes asks for (0 = romfs.bin), hashing every byte. The caller fills ring
   buffers in image order (reserve: room at *p in the current buffer; commit: n bytes of it are image data); a hash thread
   and a write thread take each full buffer in turn, so read, hash and write overlap and the digest is that of exactly
   the bytes written. Parts are fsync'd and dropped from the page cache as they fill. reserve / commit / finish return
   the write thread's failure, SETUP_CANCELLED while waiting for a cancelled run, or -1; abort removes what was written.
   TOTK_SETUP_RING=<count>x<bytes> replaces the ring geometry (default 4 x 16 MiB) for host tests. */
typedef struct RomfsRing RomfsRing;
typedef struct { char dir[SETUP_PATH]; uint64_t total, part_bytes, submitted, written, in_part; int nparts, part, fd; StCache cache; Sha256 sha; RomfsRing *ring; } RomfsWriter;
int romfs_writer_open(SetupCtx *, RomfsWriter *, const char *dir, uint64_t total, uint64_t part_bytes);
int romfs_writer_reserve(SetupCtx *, RomfsWriter *, uint8_t **p, size_t *room);
int romfs_writer_commit(SetupCtx *, RomfsWriter *, size_t n);
int romfs_writer_finish(SetupCtx *, RomfsWriter *, char sha_hex[65]);
void romfs_writer_abort(RomfsWriter *);
/* The filesystem type of the volume holding path (mounts: a /proc/self/mounts-format table, NULL = that file); 0 when no
   mount matched. /storage/<uuid>/... is the removable volume /mnt/media_rw/<uuid> that Android serves through FUSE. */
int setup_volume_type(const char *mounts, const char *path, char *type, size_t cap);
/* The part size to write under path: requested, raised to parts of ROMFS_FAT_PART_BYTES on a FAT volume whatever the caller
   asked for (a single 17 GB file would fail at 4 GiB). TOTK_SETUP_MOUNTS replaces /proc/self/mounts for host tests. */
uint64_t setup_part_bytes(const char *path, uint64_t requested);

/* ---- keys (keys.c) ---- */
#define KEY_GENERATIONS 64
typedef struct { uint8_t header_key[32]; int has_header; uint64_t titlekek_mask, kak_mask;
                 uint8_t titlekek[KEY_GENERATIONS][16], kak_app[KEY_GENERATIONS][16]; } SetupKeys;
int keys_parse(SetupCtx *, const char *text, size_t len, SetupKeys *);
int keys_load_fd(SetupCtx *, const SetupInput *, SetupKeys *);
void keys_wipe(SetupKeys *);
/* hactool's key file: only header_key, titlekek_XX and key_area_key_application_XX, mode 0600. */
int keys_write_hactool(SetupCtx *, const SetupKeys *, const char *path);

/* ---- containers (container.c) ---- */
typedef struct { char name[256]; uint64_t off, size; } PEntry;
typedef struct { uint8_t rights[16], key[16]; } Ticket;
typedef struct { char name[256]; uint64_t off, size; int content_type; char title_id[17]; uint8_t hdr[0xc00]; } NcaRecord;
typedef struct { int fd; uint64_t size; int is_xci; PEntry *entries; int nentries;
                 Ticket *tickets; int ntickets; NcaRecord *ncas; int nncas; int program; int bktr_section; } Container;
/* partition table at off within [off, off+bound) of the fd; magic "PFS0" (stride 24) or "HFS0" (stride 64). */
int part_parse(SetupCtx *, int fd, uint64_t off, uint64_t bound, const char *magic, int stride, PEntry **out, int *n);
int container_open(SetupCtx *, const SetupInput *, Container *, const char *role);    /* entries + tickets only */
int container_inventory(SetupCtx *, Container *, const SetupKeys *);                  /* NCA headers */
int container_select(SetupCtx *, Container *, int update);                             /* program NCA (+BKTR section) */
int container_check_key(SetupCtx *, Container *, const SetupKeys *, const Ticket *extra, int nextra);
void container_free(Container *);

/* ---- hactool (hactool_run.c) ---- */
typedef struct { int src_fd, child_fd; } SetupFdMap;
/* follow: hactool writes its output into a FIFO (pipe: our non-blocking read end) that is drained straight into the sink
   writer, which hashes and stores every byte (one file or parts), so the merged image is never read back. */
typedef struct { int pipe, pipe_eof; uint64_t bytes; RomfsWriter *sink; } SetupFollow;
int hactool_run(SetupCtx *, const char *stage, const char *const *args, const SetupFdMap *fds, int nfds,
                const char *const *env, const char *log_path, uint64_t progress_total,
                SetupFollow *follow, char **log_text, int *exit_code);   /* 0 once hactool exited normally */
int hactool_log_ok(const char *log, int require_ivfc);   /* the extract_update_romfs.py acceptance rules */
void hactool_scrub(char *line);                          /* hex runs >= 32 -> "<hex removed>" */

/* ---- stages (stages.c / romfs_index.c / fonts.c) ---- */
int setup_exefs_from_files(SetupCtx *, const SetupRequest *, const char *out_dir);
int setup_exefs_check(SetupCtx *, const char *dir, int code_on_mismatch_unsupported);
int setup_romfs_copy(SetupCtx *, const SetupInput *parts, int nparts, const char *out_dir, uint64_t part_bytes, char sha_hex[65]);
int setup_romfs_index(SetupCtx *, const RomfsImage *, const char *out_index, char index_sha[65]);
int setup_romfs_config(SetupCtx *, const char *path, uint64_t size, const char *image_sha, const char *index_sha);
int setup_fonts_write(SetupCtx *, const SetupInput *fonts, int nfonts, const char *out_dir, const char *out_txt, char src_sha[65]);
extern const char *const setup_font_names[6];

/* container route (containers.c) */
typedef struct { SetupKeys keys; Container base, update; int have_keys, have_base, have_update; Ticket *tickets; int ntickets;
                 char update_nca[SETUP_PATH]; int update_nca_ready; } SetupContainers;
int containers_prepare(SetupCtx *, const SetupRequest *, SetupContainers *, int need_base);
int containers_exefs(SetupCtx *, SetupContainers *, const char *out_dir);
int containers_romfs(SetupCtx *, SetupContainers *, const char *out_dir, uint64_t part_bytes, char sha_hex[65]);
void containers_free(SetupContainers *);

/* ---- install.json (setup.c) ---- */
#define SETUP_ROMFS_DIR "extracted/v143/romfs"   /* the image and runtime-index.bin, relative to <game> */
enum { ST_CODE, ST_ROMFS, ST_INDEX, ST_FONTS, ST_COMPANION, ST_SHADERS, ST_COUNT };
typedef struct {
    char route[16];
    int done[ST_COUNT];
    int companion_skipped;                 /* intentionally not needed, distinct from an absent or failed stage */
    char rewritten[3][65], tables[65], romfs_sha[65], index_sha[65], font_source[256], font_sha[65];
    uint64_t romfs_size; int romfs_parts; int64_t variants;   /* romfs_parts: 0 = one romfs.bin */
    char opt_code[ST_COUNT][32], opt_error[ST_COUNT][512];
} SetupInstall;
int setup_install_load(const char *game, SetupInstall *);    /* 0 when a format-1 TotK 1.4.3 install.json was read */
int setup_install_write(SetupCtx *, const SetupInstall *);   /* <game>/install.json, atomically */
void setup_install_json(Buf *, const SetupInstall *);      /* the install.json object */
/* The runtime's RomFS verification record (runtime/romfs_proof.h) in <priv>/state for the image in romfs_dir (one file or
   parts), so the first start there does not hash an image setup has just verified; the other layout's record goes. */
void setup_proof_write(const char *priv, const char *romfs_dir, const char *sha256);
void setup_proof_remove(const char *priv);

/* ---- top level (setup.c, move.c) ---- */
/* hooks: host tests only (NULL on Android): stand-ins for the companion and shader generators, so tests can make either
   one succeed, fail or take its time (setup_cli run --fake). A NULL member runs the real generator. */
typedef struct { int (*companion)(void *user, SetupCtx *, const char *romfs_dir, const char *out_dir);
                 int (*seed)(void *user, SetupCtx *, const char *romfs_dir, const char *rewritten_dir, const char *out_dir);
                 void *user; } SetupRunHooks;
char *setup_run_json(const char *request, size_t len, SetupProgressFn, void *user, atomic_int *cancel, const SetupPins *pins_override,
                     const SetupRunHooks *hooks);
char *setup_inspect_json(const char *request, size_t len, const SetupPins *pins_override);
/* Moves the game data between volumes: {"from","to","privateDir","romfsPartBytes"} -> {"ok":true,"install":{..}} or a
   failure; the old copy is never changed. setup_remove_json deletes one folder's game data (the old copy, once the app
   has switched). hooks: host tests only (NULL on Android): phase() is called, unthrottled, as the move enters "copy",
   "romfs", "verify" and "commit" with the staging folder; free_bytes() replaces the free-space measurement. */
typedef struct { void (*phase)(void *user, const char *phase, const char *staging); uint64_t (*free_bytes)(void *user, const char *path); void *user; } SetupMoveHooks;
char *setup_move_json(const char *request, size_t len, SetupProgressFn, void *user, atomic_int *cancel, const SetupPins *pins_override,
                      const SetupMoveHooks *hooks);
char *setup_remove_json(const char *dir);
#endif
