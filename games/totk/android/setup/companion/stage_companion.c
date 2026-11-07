/* setup_stage_companion: the dual-screen companion's assets (the Purah Pad pages) made on the phone from the player's own
   RomFS, the tree scripts/make_companion_assets.py writes on a PC (GENERATOR_VERSION 5): index.json, text/<lang>.json,
   icons/, ui/, decor/ (companion_decor.py), map/<Layer>/<lod>/ + overview.webp + markers.json, fonts/main.otf.
     1. text: which actor's text each unnamed pouch actor shows (pouch_name_sources), then every Mals/<lang> archive in
        parallel (the actor set selects the icons),
     2. images: decor (one job, the layout renderer), ui, font, markers, then every map tile and icon (map tiles first:
        the heaviest jobs, so the tail is short); each LOD 2 tile is read back from its encoded WebP, as the Python
        generator reopens the file, and pasted into its layer's overview, which the worker pasting a layer's last tile
        encodes,
     3. index.json and the 120 MiB check, then one sync of the whole tree (files in parallel, then the directories):
        files are written without a per-file fsync since the folder is staging that a failed or interrupted run discards.
   Workers never call into the SetupCtx: the stage thread polls cancellation and reports progress. Output goes straight
   into out_companion_dir (a staging folder the orchestrator renames into <game>/companion). */
#define _GNU_SOURCE 1
#include "cg.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GENERATOR_VERSION 5
#define SIZE_LIMIT (120ull << 20)
#define MAX_WORKERS 8   /* make_companion_assets.py --jobs default: up to 8 */
#define COMMON "UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs"
static const char *const LAYERS[3][2] = { { "G", "Ground" }, { "S", "Sky" }, { "U", "UnderGround" } };

struct CgOut { const char *dir; SeedErr err; atomic_int stop; _Atomic uint64_t bytes; };
SeedErr *cg_err(CgOut *o) { return &o->err; }
int cg_stopped(CgOut *o) { return atomic_load(&o->stop) || seed_err_set(&o->err); }

/* Plain write (no fsync: sync_tree() syncs the finished tree once); SYNC_FILE_RANGE_WRITE starts the write-back now so
   that final sync mostly waits for I/O that already ran. */
static int write_out(CgOut *o, const char *rel, const void *data, size_t n) {
    char path[SETUP_PATH], dir[SETUP_PATH];
    if (st_path(path, sizeof path, "%s/%s", o->dir, rel)) { seed_err(&o->err, "io", "Path too long: %s", rel); return -1; }
    memcpy(dir, path, sizeof dir);
    *strrchr(dir, '/') = 0;
    int fd = -1;
    if (st_mkdirs(dir) || (fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0 || st_write(fd, data, n)) {
        int e = errno;
        if (fd >= 0) { close(fd); unlink(path); }
        seed_err(&o->err, e == ENOSPC ? "no_space" : "io", "Cannot write %s: %s", path, strerror(e));
        return -1;
    }
    sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);   /* a hint; unsupported (FUSE) is fine */
    if (close(fd)) { int e = errno; unlink(path); seed_err(&o->err, e == ENOSPC ? "no_space" : "io", "Cannot write %s: %s", path, strerror(e)); return -1; }
    if (strcmp(rel, "index.json")) atomic_fetch_add(&o->bytes, n);   /* the limit counts the asset folders only */
    return 0;
}
int cg_save_webp(CgOut *o, const CgImg *im, const char *rel, int quality) {
    uint8_t *data; size_t n;
    if (cg_webp_encode(im, quality, 3, &data, &n)) { seed_err(&o->err, "internal", "WebP encoding failed for %s", rel); return -1; }
    int rc = write_out(o, rel, data, n);
    free(data);
    return rc;
}

/* ---- a string set (actor and icon names) ---- */
typedef struct { char **v; size_t cap, n; } StrSet;
static uint64_t fnv(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) h = (h ^ (uint8_t)*s++) * 1099511628211ull; return h; }
static int set_has(const StrSet *s, const char *k) {
    if (!s->cap) return 0;
    for (size_t i = fnv(k) & (s->cap - 1);; i = (i + 1) & (s->cap - 1)) { if (!s->v[i]) return 0; if (!strcmp(s->v[i], k)) return 1; }
}
static int set_add(StrSet *s, const char *k) {
    if (set_has(s, k)) return 0;
    if ((s->n + 1) * 2 > s->cap) {
        StrSet g = { calloc(s->cap ? s->cap * 2 : 1024, sizeof(char *)), s->cap ? s->cap * 2 : 1024, 0 };
        if (!g.v) return -1;
        for (size_t i = 0; i < s->cap; i++) if (s->v[i]) {
            size_t j = fnv(s->v[i]) & (g.cap - 1);
            while (g.v[j]) j = (j + 1) & (g.cap - 1);
            g.v[j] = s->v[i]; g.n++;
        }
        free(s->v); *s = g;
    }
    size_t j = fnv(k) & (s->cap - 1);
    while (s->v[j]) j = (j + 1) & (s->cap - 1);
    if (!(s->v[j] = strdup(k))) return -1;
    s->n++;
    return 0;
}
static void set_free(StrSet *s) { for (size_t i = 0; i < s->cap; i++) free(s->v[i]); free(s->v); memset(s, 0, sizeof *s); }

/* ---- stage state ---- */
typedef struct { char *src; char dst[160]; int limit, quality, overview_layer, col, row; } TexJob;
typedef struct { char *key, *value; } Pair;
typedef struct {
    SetupCtx *c; CgOut out; SeedRomfs romfs; SeedDicts *dicts; int workers;
    /* phase 1: text */
    int64_t *lang_entries; size_t nlang; char **lang_names; char ***lang_actors; size_t *lang_nactors;
    Pair *sources; size_t nsources;   /* actor -> display actor, or @<AttachmentCommonName> message label */
    /* phase 2 */
    uint8_t *common; size_t common_n; SarcMember *archive; size_t narchive;
    const uint8_t *combined; size_t combined_n; CgTex *tex; size_t ntex;
    TexJob *jobs; size_t njobs, *order;   /* order: the image phase's job sequence (map tiles first) */
    Buf ui, ui_sources, decor, palette, page_icons, decor_sources, fonts;
    int have_markers;
    CgImg *overview[3]; atomic_int overview_left[3];   /* LOD 2 tiles still to paste per layer */
    /* the parallel loop */
    int (*fn)(void *s, SeedDecoder *, size_t); size_t ntasks; _Atomic size_t next, done; int running;
    pthread_mutex_t mu; pthread_cond_t cv;
} Stage;

static int fail_oom(Stage *s) { seed_err(&s->out.err, "internal", "Out of memory"); return -1; }
/* RomFS.get(): the raw file, Zstandard-decoded with the game's dictionaries when it is a zstd frame */
static int romfs_get(Stage *s, SeedDecoder *dec, const char *path, uint8_t **data, size_t *n) {
    int64_t i = seed_romfs_find(&s->romfs, path);
    if (i < 0) { seed_err(&s->out.err, "missing_input", "The RomFS has no %s", path); return -1; }
    uint8_t *raw, *plain; size_t pn; uint64_t off, size;
    seed_romfs_entry(&s->romfs, (uint64_t)i, &off, &size);
    if (seed_romfs_read(&s->romfs, (uint64_t)i, &raw, &s->out.err)) return -1;
    if (seed_decode(dec, raw, size, &plain, &pn, &s->out.err)) { free(raw); return -1; }
    if (plain) { free(raw); *data = plain; *n = pn; }
    else { *data = raw; *n = size; }
    return 0;
}
static const SarcMember *member(const SarcMember *m, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) if (!strcmp(m[i].name, name)) return &m[i];
    return NULL;
}
static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static int ends(const char *s, const char *p) { size_t a = strlen(s), b = strlen(p); return a >= b && !strcmp(s + a - b, p); }

/* ---- the parallel loop: workers take tasks in order; the stage thread waits, polls cancel and reports ---- */
typedef struct { Stage *s; } WorkerArg;
static void *worker(void *arg) {
    Stage *s = arg;
    SeedDecoder *dec = seed_decoder_new(s->dicts);
    if (!dec) fail_oom(s);
    while (dec && !cg_stopped(&s->out)) {
        size_t i = atomic_fetch_add(&s->next, 1);
        if (i >= s->ntasks) break;
        if (s->fn(s, dec, i) && !cg_stopped(&s->out)) seed_err(&s->out.err, "internal", "The companion generator failed");
        atomic_fetch_add(&s->done, 1);
        pthread_mutex_lock(&s->mu); pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->mu);
    }
    seed_decoder_free(dec);
    pthread_mutex_lock(&s->mu); s->running--; pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->mu);
    return NULL;
}
static int run_parallel(Stage *s, int (*fn)(void *, SeedDecoder *, size_t), size_t n, uint64_t base, uint64_t total, const char *detail) {
    pthread_t th[MAX_WORKERS]; int started = 0;
    s->fn = fn; s->ntasks = n; atomic_store(&s->next, 0); atomic_store(&s->done, 0);
    int workers = (size_t)s->workers < n ? s->workers : (int)n;
    s->running = 0;
    for (int i = 0; i < workers; i++) {
        pthread_mutex_lock(&s->mu); s->running++; pthread_mutex_unlock(&s->mu);
        if (pthread_create(&th[i], NULL, worker, s)) { pthread_mutex_lock(&s->mu); s->running--; pthread_mutex_unlock(&s->mu); break; }
        started++;
    }
    if (!started && n) { seed_err(&s->out.err, "internal", "Cannot start worker threads"); return -1; }
    pthread_mutex_lock(&s->mu);
    while (s->running > 0) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&s->cv, &s->mu, &ts);
        pthread_mutex_unlock(&s->mu);
        if (setup_cancelled(s->c)) atomic_store(&s->out.stop, 1);
        setup_progress(s->c, "companion", base + atomic_load(&s->done), total, detail);
        pthread_mutex_lock(&s->mu);
    }
    pthread_mutex_unlock(&s->mu);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    return cg_stopped(&s->out) ? -1 : 0;
}

/* ---- 1. text (extract_text) ---- */
typedef struct { char *actor; const char *name, *desc; int desc_first; } Item;
static const char *const CATEGORIES[8] = { "armor", "bows", "shields", "weapons", "materials", "food", "zonai", "keyItems" };
static const char *const ABILITY[5][2] = { { "UltraHand", "Obj_UltraHand" }, { "OneTouchBond", "Obj_OneTouchBond" },
    { "CeilingClipper", "Obj_Tooreroof" }, { "ReverseRecorder", "Obj_ReverseRecorder" }, { "AutoBuilder", "Obj_AutoBuilder" } };
static const char *const SAGES[5][2] = { { "Wind", "Rito" }, { "Fire", "Goron" }, { "Water", "Zora" }, { "Electric", "Gerudo" }, { "Soul", "Zonau" } };
static void json_msgs(Buf *b, const CgMsg *m, size_t n, int skip_empty, const char *prefix, int *first) {
    for (size_t i = 0; i < n; i++) {
        if (skip_empty && !*m[i].text) continue;
        if (!*first) buf_add(b, ",", 1);
        *first = 0;
        if (prefix) { Buf k = { 0 }; buf_puts(&k, prefix); buf_puts(&k, m[i].label); buf_add(&k, "", 1); cg_json_str(b, k.oom ? "" : k.p); buf_free(&k); }
        else cg_json_str(b, m[i].label);
        buf_add(b, ":", 1); cg_json_str(b, m[i].text);
    }
}
static const Item *find_item(const Item *items, size_t n, const char *actor) {
    for (size_t i = 0; i < n; i++) if (!strcmp(items[i].actor, actor)) return &items[i];
    return NULL;
}
static int text_task(void *sp, SeedDecoder *dec, size_t li) {
    Stage *s = sp; SeedErr *err = &s->out.err;
    const char *path = seed_romfs_name(&s->romfs, (uint64_t)s->lang_entries[li]);
    uint8_t *data = NULL; size_t n; SarcMember *m = NULL; size_t nm = 0;
    CgMsg *pouch = NULL, *attachment = NULL, *common = NULL, *places = NULL;
    size_t npouch = 0, nattachment = 0, ncommon = 0, nplaces = 0;
    struct { const char *name; CgMsg *m; size_t n; } *layout = NULL; size_t nlayout = 0;
    Item *items = NULL; size_t nitems = 0;
    Buf b = { 0 }; int rc = -1;
    if (romfs_get(s, dec, path, &data, &n) || seed_sarc(data, n, &m, &nm, err)) goto done;
    const SarcMember *pm = member(m, nm, "ActorMsg/PouchContent.msbt"), *lm = member(m, nm, "LocationMsg/Location.msbt");
    const SarcMember *am = member(m, nm, "ActorMsg/Attachment.msbt"), *cm = member(m, nm, "StaticMsg/AttachmentCommonName.msbt");
    if (!pm || !lm || !am || !cm) { seed_err(err, "internal", "%s lacks pouch, attachment or location text", path); goto done; }
    if (cg_msbt(pm->data, pm->size, &pouch, &npouch, err) || cg_msbt(lm->data, lm->size, &places, &nplaces, err) ||
        cg_msbt(am->data, am->size, &attachment, &nattachment, err) || cg_msbt(cm->data, cm->size, &common, &ncommon, err)) goto done;
    size_t capacity = npouch + nattachment + s->nsources;
    if (!(items = calloc(capacity ? capacity : 1, sizeof *items)) || !(layout = calloc(nm ? nm : 1, sizeof *layout))) { fail_oom(s); goto done; }
    for (size_t i = 0; i < npouch; i++) {
        const char *label = pouch[i].label; size_t len = strlen(label); int desc;
        if (ends(label, "_Name")) { desc = 0; len -= 5; }
        else if (ends(label, "_Caption")) { desc = 1; len -= 8; }
        else continue;
        size_t j = 0;
        while (j < nitems && (strlen(items[j].actor) != len || strncmp(items[j].actor, label, len))) j++;
        if (j == nitems) {
            if (!(items[j].actor = strndup(label, len))) { fail_oom(s); goto done; }
            items[j].desc_first = desc; nitems++;
        }
        if (desc) items[j].desc = pouch[i].text; else items[j].name = pouch[i].text;
    }
    for (size_t i = 0; i < nattachment; i++) {
        if (!ends(attachment[i].label, "_Name")) continue;
        size_t len = strlen(attachment[i].label) - 5, j = 0;
        while (j < nitems && (strlen(items[j].actor) != len || strncmp(items[j].actor, attachment[i].label, len))) j++;
        if (j == nitems) {
            if (!(items[j].actor = strndup(attachment[i].label, len))) { fail_oom(s); goto done; }
            nitems++;
        }
        if (!items[j].name) items[j].name = attachment[i].text;
    }
    for (size_t i = 0; i < s->nsources; i++) {   /* items[actor] = dict(items[target]) */
        const char *source = s->sources[i].value;
        Item shared = { 0 };
        const Item *t;
        if (*source == '@') {
            char label[160];
            snprintf(label, sizeof label, "%s_Name", source + 1);
            shared.name = cg_msg_get(common, ncommon, label);
            t = &shared;
        } else t = find_item(items, nitems, source);
        if (find_item(items, nitems, s->sources[i].key) || !t || !t->name) continue;
        if (!(items[nitems].actor = strdup(s->sources[i].key))) { fail_oom(s); goto done; }
        items[nitems].name = t->name; items[nitems].desc = t->desc; items[nitems].desc_first = t->desc_first;
        nitems++;
    }
    for (size_t i = 0; i < nm; i++) {
        if (!(starts(m[i].name, "LayoutMsg/") || starts(m[i].name, "StaticMsg/")) || !ends(m[i].name, ".msbt")) continue;
        layout[nlayout].name = m[i].name;
        if (cg_msbt(m[i].data, m[i].size, &layout[nlayout].m, &layout[nlayout].n, err)) goto done;
        nlayout++;
    }
    size_t pouch00 = 0, buff = nlayout;
    while (pouch00 < nlayout && strcmp(layout[pouch00].name, "LayoutMsg/Pouch_00.msbt")) pouch00++;
    for (size_t i = 0; i < nlayout; i++) if (!strcmp(layout[i].name, "StaticMsg/Buff.msbt")) buff = i;
    if (pouch00 == nlayout) { seed_err(err, "internal", "%s has no LayoutMsg/Pouch_00.msbt", path); goto done; }
    buf_puts(&b, "{\"items\":{");
    for (size_t i = 0; i < nitems; i++) {
        if (i) buf_add(&b, ",", 1);
        cg_json_str(&b, items[i].actor); buf_add(&b, ":{", 2);
        for (int k = 0, any = 0; k < 2; k++) {
            int desc = k == 0 ? items[i].desc_first : !items[i].desc_first;
            const char *v = desc ? items[i].desc : items[i].name;
            if (!v) continue;
            if (any) buf_add(&b, ",", 1);
            buf_puts(&b, desc ? "\"desc\":" : "\"name\":"); cg_json_str(&b, v); any = 1;
        }
        buf_add(&b, "}", 1);
    }
    buf_puts(&b, "},\"ui\":{");
    for (int i = 0; i < 8; i++) {
        char label[64];
        snprintf(label, sizeof label, "Pa_SlideList_00-Pa_Category_%02d-T_Text_00", i);
        const char *v = cg_msg_get(layout[pouch00].m, layout[pouch00].n, label);
        if (!v) { seed_err(err, "internal", "%s has no %s", path, label); goto done; }
        if (i) buf_add(&b, ",", 1);
        cg_json_str(&b, CATEGORIES[i]); buf_add(&b, ":", 1); cg_json_str(&b, v);
    }
    int first = 0;
    for (size_t i = 0; i < nlayout; i++) {   /* name[:-5] + '/' + key, non-empty texts */
        char prefix[256];
        snprintf(prefix, sizeof prefix, "%.*s/", (int)strlen(layout[i].name) - 5, layout[i].name);
        json_msgs(&b, layout[i].m, layout[i].n, 1, prefix, &first);
    }
    buf_puts(&b, "},\"effects\":{");
    first = 1;
    if (buff < nlayout) json_msgs(&b, layout[buff].m, layout[buff].n, 0, NULL, &first);
    buf_puts(&b, "},\"abilities\":{");
    first = 1;
    for (int i = 0; i < 5; i++) {
        const Item *it = find_item(items, nitems, ABILITY[i][1]);
        if (!it) continue;
        if (!it->name) { seed_err(err, "internal", "%s: %s has no name", path, ABILITY[i][1]); goto done; }
        if (!first) buf_add(&b, ",", 1);
        first = 0; cg_json_str(&b, ABILITY[i][0]); buf_add(&b, ":", 1); cg_json_str(&b, it->name);
    }
    buf_puts(&b, "},\"sages\":{");
    first = 1;
    for (int i = 0; i < 5; i++) {
        char actor[64];
        snprintf(actor, sizeof actor, "Obj_SageSoul_%s", SAGES[i][1]);
        const Item *it = find_item(items, nitems, actor);
        if (!it) continue;
        if (!it->name) { seed_err(err, "internal", "%s: %s has no name", path, actor); goto done; }
        if (!first) buf_add(&b, ",", 1);
        first = 0; cg_json_str(&b, SAGES[i][0]); buf_add(&b, ":", 1); cg_json_str(&b, it->name);
    }
    buf_puts(&b, "},\"places\":{");
    first = 1;
    json_msgs(&b, places, nplaces, 0, NULL, &first);
    buf_puts(&b, "}}\n");
    if (b.oom) { fail_oom(s); goto done; }
    char rel[96];
    snprintf(rel, sizeof rel, "text/%s.json", s->lang_names[li]);
    if (write_out(&s->out, rel, b.p, b.n)) goto done;
    if (!(s->lang_actors[li] = calloc(nitems ? nitems : 1, sizeof(char *)))) { fail_oom(s); goto done; }
    for (size_t i = 0; i < nitems; i++) { s->lang_actors[li][i] = items[i].actor; items[i].actor = NULL; }
    s->lang_nactors[li] = nitems;
    rc = 0;
done:
    for (size_t i = 0; i < nitems; i++) free(items[i].actor);
    for (size_t i = 0; layout && i < nlayout; i++) cg_msbt_free(layout[i].m, layout[i].n);
    free(items); free(layout); cg_msbt_free(pouch, npouch); cg_msbt_free(places, nplaces); free(m); free(data); buf_free(&b);
    cg_msbt_free(attachment, nattachment); cg_msbt_free(common, ncommon);
    return rc;
}

/* ---- 2. images ---- */
/* UI_TEXTURES (key, texture name), then the effect stems (EFFECT_TEXTURES) */
static const char *const UI_TEXTURES[][2] = {
    { "heart", "HeartW_00^s" }, { "heart_empty", "HeartFrame_00^s" }, { "heart_yellow", "HeartW_00^s" }, { "heart_broken", "HeartBreakW_00^s" },
    { "rupee", "Nt_RupeeIconNew_00^w" }, { "poe", "NumDisplay_82x82_MinusRupee^w" },
    { "battery", "EnergyGaugeBigBaseFrame_00^s" }, { "battery_fill", "EnergyGaugeCore_00^s" }, { "stamina_ring", "BonusIcon72_Ganbari_00^s" },
    { "cat_weapons", "IconWeapon_72x72^s" }, { "cat_bows", "IconBow_72x72^s" }, { "cat_shields", "IconShield_72x72^s" },
    { "cat_armor", "IconEquipment_72x72^s" }, { "cat_materials", "IconMaterial_72x72^s" }, { "cat_food", "IconCuisine_72x72^s" },
    { "cat_zonai", "IconRecycle_72x72^s" }, { "cat_keyItems", "Nt_PauseIconL_06^s" }, { "map_player", "Nt_MapPlayerIconNew_00^s" },
    { "marker_shrine", "MapIconDungeon_L_Icon^s" }, { "marker_lightroot", "LoadArrivalPoint_00^w" }, { "marker_tower", "MapIconTower_00^s" },
    { "marker_stable", "MapIconBuildingStableInn_58x58^w" }, { "marker_town", "MapIconBuildingVillage_58x58^w" },
    { "background", "PlayerStatusBG_00^t" }, { "frame_corner", "Nt_PauseCorner_00^s" },
    { "sage_Wind", "SageSoulIcon_Wind^s" }, { "sage_Fire", "SageSoulIcon_Fire^s" }, { "sage_Water", "SageSoulIcon_Water^s" },
    { "sage_Electric", "SageSoulIcon_Thunder^s" }, { "sage_Soul", "SageSoulIcon_Soul^s" },
    { "stamp_Sword", "MapIconWeapon_72x72^w" }, { "stamp_Pot", "MapIconCuisine_72x72^w" }, { "stamp_Human", "MapIconHuman_72x72^w" },
    { "stamp_Rhombus", "MapIconSquare_72x72^w" }, { "stamp_Heart", "MapIconHeart_72x72^w" }, { "stamp_Star", "MapIconStar_72x72^w" },
    { "stamp_TreasureBox", "MapIconTreasureBox_72x72^w" }, { "stamp_Skull", "MapIconSkull_72x72^w" }, { "stamp_Leaf", "MapIconLeaf_72x72^w" },
    { "stamp_Ore", "MapIconOre_72x72^w" },
    { "pin_Red", "MapPinNewFLSmall0_00^w" }, { "pin_Blue", "MapPinNewFLSmall1_00^w" }, { "pin_Yellow", "MapPinNewFLSmall2_00^w" },
    { "pin_Green", "MapPinNewFLSmall3_00^w" }, { "pin_Purple", "MapPinNewFLSmall4_00^w" }, { "pin_LightBlue", "MapPinNewFLSmall5_00^s" } };
static const char *const EFFECTS[][2] = {
    { "AttackUp", "AttackCook" }, { "DefenseUp", "ShieldUp" }, { "AllSpeed", "GoGo" }, { "ResistCold", "Cool" }, { "ResistHot", "Hot" },
    { "ResistBurn", "Fire" }, { "ResistElectric", "Elect" }, { "QuietnessUp", "Noise" }, { "SwimSpeedUp", "Swim" },
    { "ClimbSpeedUp", "Mountain" }, { "SandMoveUp", "Sabaku" }, { "SnowMoveUp", "Sabaku" }, { "LifeMaxUp", "Heart" },
    { "StaminaRecover", "Ganbari" }, { "ExStaminaMaxUp", "GanbariEx" }, { "LifeRepair", "HeartRepair" }, { "MiasmaGuard", "MiasmaGuard" },
    { "NotSlippy", "Slip" }, { "LightEmission", "Luminous" }, { "RupeeGuard", "Rupee" }, { "DivingMobility", "Diving" },
    { "DecreaseZonauEnergy", "DecreaseZonauEnergy" }, { "AttackUpCold", "AttackCold" }, { "AttackUpHot", "AttackHot" },
    { "AttackUpThunderstorm", "AttackThun" } };
typedef struct { char key[64]; const char *tex; } UiEntry;
static int by_key(const void *a, const void *b) { return strcmp(((const UiEntry *)a)->key, ((const UiEntry *)b)->key); }

/* one texture file: the single texture of a BNTX, decoded, optionally thumbnailed */
static CgImg *single_texture(Stage *s, const uint8_t *data, size_t n, const char *path) {
    CgTex *t = NULL; size_t nt = 0; CgImg *im = NULL;
    if (cg_bntx(data, n, &t, &nt, &s->out.err)) return NULL;
    size_t unique = 0;   /* len(dict): distinct names */
    for (size_t i = 0; i < nt; i++) { size_t j = 0; while (j < i && strcmp(t[j].name, t[i].name)) j++; unique += j == i; }
    if (unique != 1) seed_err(&s->out.err, "internal", "Expected one texture: %s", path);
    else im = cg_tex_decode(data, n, cg_bntx_find(t, nt, t[0].name), &s->out.err);
    free(t);
    return im;
}
static int ui_task(Stage *s, SeedDecoder *dec) {
    UiEntry e[sizeof UI_TEXTURES / sizeof *UI_TEXTURES + sizeof EFFECTS / sizeof *EFFECTS]; size_t ne = 0;
    for (size_t i = 0; i < sizeof UI_TEXTURES / sizeof *UI_TEXTURES; i++) { snprintf(e[ne].key, sizeof e[ne].key, "%s", UI_TEXTURES[i][0]); e[ne++].tex = UI_TEXTURES[i][1]; }
    for (size_t i = 0; i < sizeof EFFECTS / sizeof *EFFECTS; i++) {
        char prefix[96];
        snprintf(prefix, sizeof prefix, "BonusIcon72_%s_00^", EFFECTS[i][1]);
        for (size_t k = 0; k < s->ntex; k++)
            if (starts(s->tex[k].name, prefix)) { snprintf(e[ne].key, sizeof e[ne].key, "effect_%s", EFFECTS[i][0]); e[ne++].tex = s->tex[k].name; break; }
    }
    qsort(e, ne, sizeof *e, by_key);
    int first = 1;
    buf_puts(&s->ui, "{"); buf_puts(&s->ui_sources, "{");
    for (size_t i = 0; i < ne; i++) {
        const CgTex *t = cg_bntx_find(s->tex, s->ntex, e[i].tex);
        if (!t) continue;
        CgImg *im = cg_tex_decode(s->combined, s->combined_n, t, &s->out.err);
        if (!im) return -1;
        if (!strcmp(e[i].key, "marker_stable") || !strcmp(e[i].key, "marker_town")) {
            /* black ink on a white badge: the ink silhouette as a white mask */
            CgImg *l = cg_convert(im, CG_L);
            if (!l) { cg_free(im); return fail_oom(s); }
            cg_invert(l);
            for (size_t p = 0; p < (size_t)im->w * im->h; p++) {
                im->p[p * 4 + 3] = (uint8_t)(l->p[p] * im->p[p * 4 + 3] / 255);
                memset(im->p + p * 4, 255, 3);
            }
            cg_free(l);
        }
        char rel[96];
        snprintf(rel, sizeof rel, "ui/%.63s.webp", e[i].key);
        int rc = cg_save_webp(&s->out, im, rel, 85);
        cg_free(im);
        if (rc) return -1;
        if (!first) { buf_add(&s->ui, ",", 1); buf_add(&s->ui_sources, ",", 1); }
        first = 0;
        cg_json_str(&s->ui, e[i].key); buf_add(&s->ui, ":", 1); cg_json_str(&s->ui, rel);
        cg_json_str(&s->ui_sources, e[i].key); buf_add(&s->ui_sources, ":", 1); cg_json_str(&s->ui_sources, e[i].tex);
    }
    for (int i = 0; i < 5; i++) {
        char path[128], rel[96], key[64];
        snprintf(path, sizeof path, "UI/Tex/Icon/%s.bntx.zs", ABILITY[i][1]);
        snprintf(rel, sizeof rel, "ui/ability_%s.webp", ABILITY[i][0]);
        snprintf(key, sizeof key, "ability_%s", ABILITY[i][0]);
        uint8_t *data; size_t n;
        if (romfs_get(s, dec, path, &data, &n)) return -1;
        CgTex *t = NULL; size_t nt = 0; CgImg *im = NULL, *th = NULL;
        if (!cg_bntx(data, n, &t, &nt, &s->out.err) && nt) im = cg_tex_decode(data, n, cg_bntx_find(t, nt, t[0].name), &s->out.err);
        else if (!nt) seed_err(&s->out.err, "internal", "%s has no texture", path);
        free(t);
        if (im && !(th = cg_thumbnail(im, 128, CG_LANCZOS))) fail_oom(s);
        int rc = th ? cg_save_webp(&s->out, th, rel, 85) : -1;
        cg_free(im); cg_free(th); free(data);
        if (rc) return -1;
        if (!first) { buf_add(&s->ui, ",", 1); buf_add(&s->ui_sources, ",", 1); }
        first = 0;
        cg_json_str(&s->ui, key); buf_add(&s->ui, ":", 1); cg_json_str(&s->ui, rel);
        cg_json_str(&s->ui_sources, key); buf_add(&s->ui_sources, ":", 1); cg_json_str(&s->ui_sources, path);
    }
    buf_puts(&s->ui, "}"); buf_puts(&s->ui_sources, "}");
    return 0;
}
static int font_task(Stage *s, SeedDecoder *dec) {
    uint8_t *data, *font = NULL; size_t n, fn; SarcMember *m = NULL; size_t nm; int otf, rc = -1;
    if (romfs_get(s, dec, "Font/Font.Nin_NX_NVN.bfarc.zs", &data, &n)) return -1;
    if (!seed_sarc(data, n, &m, &nm, &s->out.err)) {
        const SarcMember *f = member(m, nm, "scft/ninP_RodinNTLG-M_DH_003_subset.bfotf");
        if (!f) seed_err(&s->out.err, "internal", "The game font archive has no Rodin font");
        else if (!cg_font(f->data, f->size, &font, &fn, &otf, &s->out.err)) {
            const char *rel = otf ? "fonts/main.otf" : "fonts/main.ttf";
            if (!write_out(&s->out, rel, font, fn)) { buf_printf(&s->fonts, "{\"main\":\"%s\"}", rel); rc = 0; }
        }
    }
    free(font); free(m); free(data);
    return rc;
}
/* extract_markers(): authored location-area display positions; Sky is decided by LocationMarker instance IDs */
static int markers_task(Stage *s, SeedDecoder *dec) {
    StrSet sky = { 0 }; Buf b = { 0 }; int rc = -1, count = 0;
    for (uint64_t i = 0; i < s->romfs.count; i++) {
        const char *path = seed_romfs_name(&s->romfs, i);
        if (!starts(path, "Banc/MainField/Sky/") || !ends(path, ".byml.zs")) continue;
        uint8_t *data; size_t n;
        if (romfs_get(s, dec, path, &data, &n)) goto done;
        if (!memmem(data, n, "Dungeon", 7)) { free(data); continue; }
        CgVal *root = cg_byml(data, n, &s->out.err);
        const CgVal *actors = root ? cg_get(root, "Actors") : NULL;
        if (root && root->type != CGV_DICT) seed_err(&s->out.err, "internal", "%s: unexpected BYML root", path);
        for (size_t k = 0; actors && actors->type == CGV_ARR && k < actors->n; k++) {
            const CgVal *g = cg_get(&actors->items[k], "Gyaml"), *h = cg_get(&actors->items[k], "Hash");
            if (!g || g->type != CGV_STR || strcmp(g->s, "LocationMarker")) continue;
            char id[64];
            if (!h || cg_val_str(h, id, sizeof id)) { seed_err(&s->out.err, "internal", "%s: LocationMarker without a hash", path); break; }
            if (set_add(&sky, id)) { fail_oom(s); break; }
        }
        cg_val_free(root); free(data);
        if (seed_err_set(&s->out.err)) goto done;
    }
    static const char *const FIELDS[2][2] = { { "MainField", NULL }, { "MinusField", "UnderGround" } };
    static const char *const GROUPS[5][2] = { { "Shrine", "shrine" }, { "CheckPoint", "lightroot" }, { "Tower", "tower" }, { "Stable", "stable" }, { "City", "town" } };
    buf_puts(&b, "[");
    for (int f = 0; f < 2; f++) {
        char path[128];
        snprintf(path, sizeof path, "Banc/%s/LocationArea/%s.locationarea.byml.zs", FIELDS[f][0], FIELDS[f][0]);
        if (seed_romfs_find(&s->romfs, path) < 0) continue;
        uint8_t *data; size_t n;
        if (romfs_get(s, dec, path, &data, &n)) goto done;
        CgVal *table = cg_byml(data, n, &s->out.err);
        for (int g = 0; table && g < 5; g++) {
            const CgVal *group = cg_get(table, GROUPS[g][0]);
            for (size_t e = 0; group && group->type == CGV_ARR && e < group->n; e++) {
                const CgVal *entry = &group->items[e], *name = cg_get(entry, "LocationName"), *trans = cg_get(entry, "Trans"), *ids = cg_get(entry, "InstanceID");
                if (!name || name->type != CGV_STR) { seed_err(&s->out.err, "internal", "%s: location without a name", path); break; }
                const char *base = strrchr(name->s, '/'); base = base ? base + 1 : name->s;
                size_t len = strcspn(base, ".");
                for (size_t i = 0; trans && trans->type == CGV_ARR && i < trans->n; i++) {
                    const CgVal *pos = &trans->items[i];
                    if (pos->type != CGV_ARR || pos->n != 3) continue;
                    const CgVal *inst = ids && ids->type == CGV_ARR && i < ids->n ? &ids->items[i] : NULL;
                    const char *layer = FIELDS[f][1] ? FIELDS[f][1] : inst && inst->type == CGV_STR && set_has(&sky, inst->s) ? "Sky" : "Ground";
                    buf_add(&b, count ? ",{" : "{", count ? 2 : 1);
                    char id[384], label[256];
                    snprintf(label, sizeof label, "%.*s", (int)len, base);
                    snprintf(id, sizeof id, "%s/%s/%zu", FIELDS[f][0], label, i);
                    buf_puts(&b, "\"id\":"); cg_json_str(&b, id);
                    buf_printf(&b, ",\"type\":\"%s\",\"label\":", GROUPS[g][1]);
                    cg_json_str(&b, label);
                    buf_puts(&b, ",\"x\":"); cg_json_val(&b, &pos->items[0]);
                    buf_puts(&b, ",\"y\":"); cg_json_val(&b, &pos->items[1]);
                    buf_puts(&b, ",\"z\":"); cg_json_val(&b, &pos->items[2]);
                    buf_printf(&b, ",\"layer\":\"%s\"}", layer);
                    count++;
                }
            }
        }
        cg_val_free(table); free(data);
        if (seed_err_set(&s->out.err)) goto done;
    }
    buf_puts(&b, "]\n");
    if (b.oom) { fail_oom(s); goto done; }
    if (count && write_out(&s->out, "map/markers.json", b.p, b.n)) goto done;
    s->have_markers = count > 0;
    rc = 0;
done:
    set_free(&sky); buf_free(&b);
    return rc;
}
static int texture_task(Stage *s, SeedDecoder *dec, const TexJob *j) {
    uint8_t *data, *webp = NULL; size_t n, wn; int rc = -1;
    if (romfs_get(s, dec, j->src, &data, &n)) return -1;
    CgImg *im = single_texture(s, data, n, j->src), *th = NULL, *tile = NULL, *small = NULL;
    free(data);
    if (!im) return -1;
    if (j->limit && !(th = cg_thumbnail(im, j->limit, CG_LANCZOS))) { fail_oom(s); goto done; }
    if (cg_webp_encode(th ? th : im, j->quality, 3, &webp, &wn)) { seed_err(&s->out.err, "internal", "WebP encoding failed for %s", j->dst); goto done; }
    if (write_out(&s->out, j->dst, webp, wn)) goto done;
    if (j->overview_layer >= 0) {   /* map_overviews(): the saved tile reopened, resized to 200 px, pasted at col, row */
        if (!(tile = cg_webp_decode(webp, wn)) || !(small = cg_resize(tile, 200, 200, CG_LANCZOS, NULL))) { fail_oom(s); goto done; }
        CgImg *ov = s->overview[j->overview_layer];
        for (int y = 0; y < 200; y++) memcpy(cg_px(ov, j->col * 200, j->row * 200 + y), cg_px(small, 0, y), 200 * 4);
        if (atomic_fetch_sub(&s->overview_left[j->overview_layer], 1) == 1) {   /* the layer's last tile: all pasted */
            char rel[64];
            snprintf(rel, sizeof rel, "map/%s/overview.webp", LAYERS[j->overview_layer][1]);
            if (cg_save_webp(&s->out, ov, rel, 80)) goto done;
        }
    }
    rc = 0;
done:
    free(webp); cg_free(im); cg_free(th); cg_free(tile); cg_free(small);
    return rc;
}
/* phase 2 task list: 0 decor, 1 ui, 2 font, 3 markers, then the texture jobs in s->order */
#define FIXED_TASKS 4
static int image_task(void *sp, SeedDecoder *dec, size_t i) {
    Stage *s = sp;
    switch (i) {
    case 0: {
        CgDecorIn in = { s->combined, s->combined_n, s->tex, s->ntex, s->archive, s->narchive };
        return cg_decor(&s->out, &in, &s->decor, &s->palette, &s->page_icons, &s->decor_sources);
    }
    case 1: return ui_task(s, dec);
    case 2: return font_task(s, dec);
    case 3: return markers_task(s, dec);
    default: return texture_task(s, dec, &s->jobs[s->order[i - FIXED_TASKS]]);
    }
}

/* icon_jobs(): PouchContent actors (dye colours are aliases), recipe variants and SpObj parts */
static int add_job(Stage *s, size_t *cap, const char *src, const char *dst, int limit, int quality, int layer, int col, int row) {
    if (s->njobs == *cap) {
        TexJob *g = realloc(s->jobs, (*cap = *cap ? *cap * 2 : 1024) * sizeof *g);
        if (!g) return fail_oom(s);
        s->jobs = g;
    }
    TexJob *j = &s->jobs[s->njobs];
    if (!(j->src = strdup(src))) return fail_oom(s);
    snprintf(j->dst, sizeof j->dst, "%s", dst);
    j->limit = limit; j->quality = quality; j->overview_layer = layer; j->col = col; j->row = row;
    s->njobs++;
    return 0;
}
static int pair_set(Pair **v, size_t *n, size_t *cap, const char *key, const char *value) {
    for (size_t i = 0; i < *n; i++) if (!strcmp((*v)[i].key, key)) { char *nv = strdup(value); if (!nv) return -1; free((*v)[i].value); (*v)[i].value = nv; return 0; }
    if (*n == *cap) { Pair *g = realloc(*v, (*cap = *cap ? *cap * 2 : 256) * sizeof *g); if (!g) return -1; *v = g; }
    if (!((*v)[*n].key = strdup(key)) || !((*v)[*n].value = strdup(value))) return -1;
    (*n)++;
    return 0;
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* pouch_name_sources(): actors without their own pouch/attachment name -> a display actor, or an
   @AttachmentCommonName label. Fuse-only objects take their material names from ActorMsg/Attachment or
   AttachmentActorInfo.AttachmentCommonName -> StaticMsg/AttachmentCommonName, not the weapon-title adjectives.
   ActorInfo ActorName aliases and the first named $parent handle actor variants in both tables. */
static int rsdb_rows(Stage *s, SeedDecoder *dec, const char *table, uint8_t **data, CgVal **rows) {
    char prefix[64]; const char *path = NULL; size_t found = 0, n;
    snprintf(prefix, sizeof prefix, "RSDB/%s.Product.", table);
    for (uint64_t i = 0; i < s->romfs.count; i++) {
        const char *p = seed_romfs_name(&s->romfs, i);
        if (starts(p, prefix) && ends(p, ".rstbl.byml.zs")) { path = p; found++; }
    }
    if (found != 1) { seed_err(&s->out.err, "internal", "Expected one RSDB %s table, found %zu", table, found); return -1; }
    if (romfs_get(s, dec, path, data, &n) || !(*rows = cg_byml(*data, n, &s->out.err))) return -1;
    if ((*rows)->type != CGV_ARR) { seed_err(&s->out.err, "internal", "%s is not a row list", path); return -1; }
    return 0;
}
static const char *row_str(const CgVal *row, const char *key) {
    const CgVal *v = cg_get(row, key);
    return v && v->type == CGV_STR ? v->s : NULL;
}
/* Work/.../<stem>.<kind>.gyml -> <stem> */
static int path_stem(const char *path, char *out, size_t cap) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t len = strcspn(b, ".");
    if (len >= cap) return -1;
    memcpy(out, b, len); out[len] = 0;
    return 0;
}
/* One candidate of base_target(): 1 = it is the target, 0 = keep looking */
static int candidate(const char *name, const char *actor, const StrSet *named, char *target, size_t cap) {
    if (!strcmp(name, actor) || !set_has(named, name)) return 0;
    snprintf(target, cap, "%s", name);
    return 1;
}
/* Follow the $parent chain from pack member `from` (prefix + stem + suffix name the parent's member); 1 = found */
static int follow_parents(Stage *s, const SarcMember *m, size_t nm, const SarcMember *from, const char *prefix, const char *suffix,
                          const char *actor, const StrSet *named, char *target, size_t cap) {
    for (int depth = 0; from && depth < 64; depth++) {
        CgVal *v = cg_byml(from->data, from->size, &s->out.err);
        if (!v) return -1;
        const char *parent = row_str(v, "$parent");
        char name[128], next[256];
        int ok = parent && *parent && !path_stem(parent, name, sizeof name);
        cg_val_free(v);
        if (!ok) return 0;
        if (candidate(name, actor, named, target, cap)) return 1;
        snprintf(next, sizeof next, "%s%s%s", prefix, name, suffix);
        from = member(m, nm, next);
    }
    return 0;
}
/* base_actors(): <actor>'s ActorParam $parent chain, then its GameParameterTable and that table's $parent chain; the
   first of them with a name goes to target (left empty when none has one) */
static int base_target(Stage *s, SeedDecoder *dec, const char *actor, const StrSet *named, char *target, size_t cap) {
    char path[192], name[128];
    uint8_t *data = NULL; size_t n, nm = 0; SarcMember *m = NULL; CgVal *root = NULL; int rc = -1;
    target[0] = 0;
    snprintf(path, sizeof path, "Pack/Actor/%s.pack.zs", actor);
    if (seed_romfs_find(&s->romfs, path) < 0) return 0;
    if (romfs_get(s, dec, path, &data, &n) || seed_sarc(data, n, &m, &nm, &s->out.err)) goto done;
    snprintf(path, sizeof path, "Actor/%s.engine__actor__ActorParam.bgyml", actor);
    const SarcMember *param = member(m, nm, path);
    if (!param) { rc = 0; goto done; }
    int found = follow_parents(s, m, nm, param, "Actor/", ".engine__actor__ActorParam.bgyml", actor, named, target, cap);
    if (found) { rc = found < 0 ? -1 : 0; goto done; }
    if (!(root = cg_byml(param->data, param->size, &s->out.err))) goto done;
    const char *table = row_str(cg_get(root, "Components"), "GameParameterTableRef");
    rc = 0;
    if (!table || !*table) goto done;
    while (*table == '?') table++;
    if (path_stem(table, name, sizeof name) || candidate(name, actor, named, target, cap)) goto done;
    if (follow_parents(s, m, nm, member(m, nm, table), "GameParameter/GameParameterTable/", ".engine__actor__GameParameterTable.bgyml",
                       actor, named, target, cap) < 0) rc = -1;
done:
    cg_val_free(root); free(m); free(data);
    return rc;
}
typedef struct { const char *row, *name; } ActorName;
static int by_row(const void *a, const void *b) { return strcmp(((const ActorName *)a)->row, ((const ActorName *)b)->row); }
static int pouch_name_sources(Stage *s, SeedDecoder *dec) {
    uint8_t *mals = NULL, *info_data = NULL, *pouch_data = NULL, *attach_data = NULL;
    size_t n, nm = 0, nmsg = 0, ncommon = 0, cap = 0, ninfo = 0;
    SarcMember *m = NULL; CgMsg *msg = NULL, *common = NULL;
    CgVal *info = NULL, *pouch = NULL, *attach = NULL; ActorName *names = NULL;
    StrSet named = { 0 }; int rc = -1;
    if (!s->nlang) return 0;
    if (romfs_get(s, dec, seed_romfs_name(&s->romfs, (uint64_t)s->lang_entries[0]), &mals, &n) || seed_sarc(mals, n, &m, &nm, &s->out.err)) goto done;
    for (int table = 0; table < 2; table++) {
        const SarcMember *pm = member(m, nm, table ? "ActorMsg/Attachment.msbt" : "ActorMsg/PouchContent.msbt");
        if (!pm) { seed_err(&s->out.err, "internal", "%s lacks pouch or attachment text", s->lang_names[0]); goto done; }
        if (cg_msbt(pm->data, pm->size, &msg, &nmsg, &s->out.err)) goto done;
        for (size_t i = 0; i < nmsg; i++) {
            size_t len = strlen(msg[i].label);
            if (!ends(msg[i].label, "_Name")) continue;
            msg[i].label[len - 5] = 0;
            if (set_add(&named, msg[i].label)) { fail_oom(s); goto done; }
        }
        cg_msbt_free(msg, nmsg); msg = NULL; nmsg = 0;
    }
    const SarcMember *cm = member(m, nm, "StaticMsg/AttachmentCommonName.msbt");
    if (!cm) { seed_err(&s->out.err, "internal", "%s lacks common attachment text", s->lang_names[0]); goto done; }
    if (cg_msbt(cm->data, cm->size, &common, &ncommon, &s->out.err) ||
        rsdb_rows(s, dec, "ActorInfo", &info_data, &info) || rsdb_rows(s, dec, "PouchActorInfo", &pouch_data, &pouch) ||
        rsdb_rows(s, dec, "AttachmentActorInfo", &attach_data, &attach)) goto done;
    for (size_t i = 0; i < attach->n; i++) {
        const char *actor = row_str(&attach->items[i], "__RowId"), *name = row_str(&attach->items[i], "AttachmentCommonName");
        if (!actor || !name || set_has(&named, actor)) continue;
        char label[160], target[160];
        snprintf(label, sizeof label, "%s_Name", name);
        if (!cg_msg_get(common, ncommon, label)) continue;
        snprintf(target, sizeof target, "@%s", name);
        if (pair_set(&s->sources, &s->nsources, &cap, actor, target)) { fail_oom(s); goto done; }
    }
    for (size_t i = 0; i < s->nsources; i++) if (set_add(&named, s->sources[i].key)) { fail_oom(s); goto done; }
    if (!(names = malloc((info->n ? info->n : 1) * sizeof *names))) { fail_oom(s); goto done; }
    for (size_t i = 0; i < info->n; i++) {
        const char *row = row_str(&info->items[i], "__RowId");
        if (row) { names[ninfo].row = row; names[ninfo++].name = row_str(&info->items[i], "ActorName"); }
    }
    qsort(names, ninfo, sizeof *names, by_row);
    for (size_t i = 0; i < pouch->n + attach->n; i++) {
        const CgVal *row = i < pouch->n ? &pouch->items[i] : &attach->items[i - pouch->n];
        const char *actor = row_str(row, "__RowId");
        if (!actor || set_has(&named, actor)) continue;
        int seen = 0;
        for (size_t k = 0; k < s->nsources && !seen; k++) seen = !strcmp(s->sources[k].key, actor);
        if (seen) continue;
        ActorName key = { actor, NULL }, *hit = bsearch(&key, names, ninfo, sizeof *names, by_row);
        const char *alias = hit ? hit->name : NULL;
        char target[128];
        if (alias && *alias && strcmp(alias, actor) && set_has(&named, alias)) snprintf(target, sizeof target, "%s", alias);
        else if (base_target(s, dec, actor, &named, target, sizeof target)) goto done;
        if (*target && pair_set(&s->sources, &s->nsources, &cap, actor, target)) { fail_oom(s); goto done; }
    }
    rc = 0;
done:
    free(names); cg_val_free(info); cg_val_free(pouch); cg_val_free(attach); free(info_data); free(pouch_data); free(attach_data);
    cg_msbt_free(msg, nmsg); cg_msbt_free(common, ncommon); free(m); free(mals); set_free(&named);
    return rc;
}
static int icon_jobs(Stage *s, SeedDecoder *dec, const StrSet *actors, size_t *cap, Buf *icons_json, Buf *aliases_json) {
    uint8_t *data; size_t n; int rc = -1;
    StrSet colors = { 0 }, names = { 0 }; Pair *aliases = NULL; size_t nal = 0, capal = 0; char **sorted = NULL;
    if (romfs_get(s, dec, "UI/PouchSortTable/PouchSortTable.game__ui__PouchSortTable.bgyml", &data, &n)) return -1;
    CgVal *sort = cg_byml(data, n, &s->out.err);
    const CgVal *dye = sort ? cg_get(sort, "ArmorDyeColor") : NULL;
    if (sort && (!dye || dye->type != CGV_ARR)) seed_err(&s->out.err, "internal", "PouchSortTable has no ArmorDyeColor");
    for (size_t i = 0; dye && dye->type == CGV_ARR && i < dye->n; i++)
        if (dye->items[i].type == CGV_STR && strcmp(dye->items[i].s, "None") && set_add(&colors, dye->items[i].s)) { fail_oom(s); break; }
    cg_val_free(sort); free(data);
    if (seed_err_set(&s->out.err)) goto done;
    for (uint64_t i = 0; i < s->romfs.count; i++) {
        const char *path = seed_romfs_name(&s->romfs, i);
        if (!(starts(path, "UI/Tex/Icon/") || starts(path, "UI/Tex/SpecialParts/")) || !ends(path, ".bntx.zs")) continue;
        const char *base = strrchr(path, '/') + 1;
        char actor[128], dst[160];
        if (strlen(base) - 8 >= sizeof actor) { seed_err(&s->out.err, "internal", "Icon name too long: %s", path); goto done; }
        snprintf(actor, sizeof actor, "%.*s", (int)strlen(base) - 8, base);
        if (set_has(&names, actor)) continue;
        const char *us = strrchr(actor, '_');
        if (starts(actor, "Armor_") && us && set_has(&colors, us + 1)) {
            char basename[128];
            snprintf(basename, sizeof basename, "%.*s", (int)(us - actor), actor);
            if (set_has(actors, basename) && pair_set(&aliases, &nal, &capal, actor, basename)) { fail_oom(s); goto done; }
            continue;
        }
        if (!set_has(actors, actor) && !starts(actor, "Item_Cook_") && !starts(actor, "SpObj_")) continue;
        snprintf(dst, sizeof dst, "icons/%s.webp", actor);
        if (set_add(&names, actor) || add_job(s, cap, path, dst, 128, 85, -1, 0, 0)) { fail_oom(s); goto done; }
    }
    if (set_has(&names, "Item_Cook_C_17_StaminaRecover") && pair_set(&aliases, &nal, &capal, "Item_Cook_C_17", "Item_Cook_C_17_StaminaRecover")) { fail_oom(s); goto done; }
    if (!set_has(&names, "Obj_Battery_Get_Capacity03") && seed_romfs_find(&s->romfs, "UI/Tex/Icon/Obj_Battery_Get_Capacity03.bntx.zs") >= 0) {
        if (set_add(&names, "Obj_Battery_Get_Capacity03") ||
            add_job(s, cap, "UI/Tex/Icon/Obj_Battery_Get_Capacity03.bntx.zs", "icons/Obj_Battery_Get_Capacity03.webp", 128, 85, -1, 0, 0) ||
            pair_set(&aliases, &nal, &capal, "Obj_Battery_Get", "Obj_Battery_Get_Capacity03")) { fail_oom(s); goto done; }
    }
    /* pouch actors that show another actor's text and have no icon of their own show that actor's icon */
    for (size_t i = 0; i < s->nsources; i++)
        if (!set_has(&names, s->sources[i].key) && pair_set(&aliases, &nal, &capal, s->sources[i].key, s->sources[i].value)) { fail_oom(s); goto done; }
    if (!(sorted = malloc((names.n ? names.n : 1) * sizeof *sorted))) { fail_oom(s); goto done; }
    size_t k = 0;
    for (size_t i = 0; i < names.cap; i++) if (names.v[i]) sorted[k++] = names.v[i];
    qsort(sorted, k, sizeof *sorted, cmp_str);
    buf_puts(icons_json, "[");
    for (size_t i = 0; i < k; i++) { if (i) buf_add(icons_json, ",", 1); cg_json_str(icons_json, sorted[i]); }
    buf_puts(icons_json, "]");
    buf_puts(aliases_json, "{");
    for (size_t i = 0, first = 1; i < nal; i++) {
        if (!set_has(&names, aliases[i].value)) continue;
        if (!first) buf_add(aliases_json, ",", 1);
        first = 0;
        cg_json_str(aliases_json, aliases[i].key); buf_add(aliases_json, ":", 1); cg_json_str(aliases_json, aliases[i].value);
    }
    buf_puts(aliases_json, "}");
    rc = 0;
done:
    for (size_t i = 0; i < nal; i++) { free(aliases[i].key); free(aliases[i].value); }
    free(aliases); free(sorted); set_free(&colors); set_free(&names);
    return rc;
}
/* map_jobs(): UI/Map/MainField/<G|S|U>_<col>-<row>_<01|02>[_<T|F...>][_<A|B>(_<A|B>)*].bntx.zs; per tile all-T first, then
   the longest A/B chain, then the larger chain / variant string */
typedef struct { int found; int t; size_t elen; char event[64], variant[64]; const char *path; } MapPick;
static int map_jobs(Stage *s, size_t *cap) {
    MapPick *pick = calloc(3 * 12 * 10 * 2, sizeof *pick);
    if (!pick) return fail_oom(s);
    int rc = -1;
    for (uint64_t i = 0; i < s->romfs.count; i++) {
        const char *path = seed_romfs_name(&s->romfs, i), *p;
        if (!starts(path, "UI/Map/MainField/")) continue;
        p = path + 17;
        int layer = *p == 'G' ? 0 : *p == 'S' ? 1 : *p == 'U' ? 2 : -1;
#define DIG(c) ((c) >= '0' && (c) <= '9')
        if (layer < 0 || p[1] != '_' || !DIG(p[2]) || !DIG(p[3]) || p[4] != '-' || !DIG(p[5]) || !DIG(p[6]) || p[7] != '_' ||
            p[8] != '0' || (p[9] != '1' && p[9] != '2')) continue;
        int col = (p[2] - '0') * 10 + (p[3] - '0'), row = (p[5] - '0') * 10 + (p[6] - '0'), lod = p[9] - '0';
        p += 10;
        char variant[64] = "", event[64] = "";
        if (p[0] == '_' && (p[1] == 'T' || p[1] == 'F')) {
            size_t k = strspn(p + 1, "TF");
            if (k >= sizeof variant) continue;
            memcpy(variant, p + 1, k); variant[k] = 0; p += 1 + k;
        }
        if (p[0] == '_' && (p[1] == 'A' || p[1] == 'B')) {
            const char *e = p + 2;
            while (e[0] == '_' && (e[1] == 'A' || e[1] == 'B')) e += 2;
            size_t k = (size_t)(e - (p + 1));
            if (k >= sizeof event) continue;
            memcpy(event, p + 1, k); event[k] = 0; p = e;
        }
        if (strcmp(p, ".bntx.zs")) continue;
#undef DIG
        if (col >= 12 || row >= 10) { seed_err(&s->out.err, "internal", "Unexpected MainField map tile %s", path); goto done; }
        int t = 0;
        for (const char *v = variant; *v; v++) t += *v == 'T';
        MapPick *m = &pick[((layer * 12 + col) * 10 + row) * 2 + lod - 1];
        int better = !m->found;
        if (!better) {   /* rank tuple: (T count, len(event), event, variant) */
            int c = t != m->t ? (t > m->t ? 1 : -1) : strlen(event) != m->elen ? (strlen(event) > m->elen ? 1 : -1) : strcmp(event, m->event);
            if (!c) c = strcmp(variant, m->variant);
            better = c > 0;
        }
        if (better) { m->found = 1; m->t = t; m->elen = strlen(event); strcpy(m->event, event); strcpy(m->variant, variant); m->path = path; }
    }
    for (int k = 0; k < 3 * 12 * 10 * 2; k++)
        if (!pick[k].found) {
            int lod = k % 2 + 1, row = k / 2 % 10, col = k / 20 % 12, layer = k / 240;
            seed_err(&s->out.err, "internal", "Incomplete MainField map: missing %s %d,%d LOD %d", LAYERS[layer][0], col, row, lod);
            goto done;
        }
    for (int layer = 0; layer < 3; layer++)   /* sorted by (layer letter, col, row, lod): G, S, U */
        for (int col = 0; col < 12; col++)
            for (int row = 0; row < 10; row++)
                for (int lod = 1; lod <= 2; lod++) {
                    char dst[128];
                    snprintf(dst, sizeof dst, "map/%s/%d/%d_%d.webp", LAYERS[layer][1], lod, col, row);
                    if (add_job(s, cap, pick[((layer * 12 + col) * 10 + row) * 2 + lod - 1].path, dst, 0, 80, lod == 2 ? layer : -1, col, row)) goto done;
                }
    rc = 0;
done:
    free(pick);
    return rc;
}

/* The tree's one sync before the orchestrator commits it: every file (fsync in parallel; their write-back already started
   in write_out), then every directory, children before parents. */
typedef struct { char **v; size_t n, cap; } Paths;
static int paths_add(Paths *p, const char *path) {
    if (p->n == p->cap) {
        char **g = realloc(p->v, (p->cap = p->cap ? p->cap * 2 : 256) * sizeof *g);
        if (!g) return -1;
        p->v = g;
    }
    return (p->v[p->n] = strdup(path)) ? (int)(p->n++, 0) : -1;
}
static void paths_free(Paths *p) { for (size_t i = 0; i < p->n; i++) free(p->v[i]); free(p->v); }
static int collect(const char *path, Paths *files, Paths *dirs) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e; int rc = 0;
    while (!rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[SETUP_PATH]; struct stat st;
        if (st_path(p, sizeof p, "%s/%s", path, e->d_name) || lstat(p, &st)) rc = -1;
        else if (S_ISDIR(st.st_mode)) rc = collect(p, files, dirs);
        else if (paths_add(files, p)) { errno = ENOMEM; rc = -1; }
    }
    closedir(d);
    if (!rc && paths_add(dirs, path)) { errno = ENOMEM; rc = -1; }
    return rc;
}
typedef struct { const Paths *files; _Atomic size_t next; atomic_int err; } SyncJob;
static void *sync_worker(void *arg) {
    SyncJob *j = arg;
    for (size_t i; !atomic_load(&j->err) && (i = atomic_fetch_add(&j->next, 1)) < j->files->n;) {
        int fd = open(j->files->v[i], O_RDONLY | O_CLOEXEC);
        if (fd < 0 || fsync(fd)) { int e = errno ? errno : EIO; if (fd >= 0) close(fd); atomic_store(&j->err, e); break; }
        close(fd);
    }
    return NULL;
}
static int sync_tree(const char *root, int workers) {
    Paths files = { 0 }, dirs = { 0 }; int rc = collect(root, &files, &dirs);
    if (!rc) {
        SyncJob j = { &files, 0, 0 }; pthread_t th[MAX_WORKERS]; int started = 0;
        for (; started < workers - 1 && (size_t)started + 1 < files.n; started++) if (pthread_create(&th[started], NULL, sync_worker, &j)) break;
        sync_worker(&j);
        for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
        if (atomic_load(&j.err)) { errno = atomic_load(&j.err); rc = -1; }
    }
    for (size_t i = 0; !rc && i < dirs.n; i++) if (st_fsync_dir(dirs.v[i])) rc = -1;
    int e = errno;
    paths_free(&files); paths_free(&dirs);
    errno = e;
    return rc;
}

int setup_stage_companion(SetupCtx *c, const char *romfs_dir, const char *out_dir) {
    Stage *s = calloc(1, sizeof *s);
    if (!s) { setup_fail(c, "internal", "Out of memory"); return -1; }
    s->c = c; s->out.dir = out_dir;
    pthread_mutex_init(&s->out.err.mu, NULL); pthread_mutex_init(&s->mu, NULL); pthread_cond_init(&s->cv, NULL);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    s->workers = ncpu < 1 ? 1 : ncpu > MAX_WORKERS ? MAX_WORKERS : (int)ncpu;
    SeedDecoder *dec = NULL; StrSet actors = { 0 };
    Buf langs = { 0 }, icons = { 0 }, aliases = { 0 }, index = { 0 };
    size_t cap = 0; int rc = -1;

    if (st_rmtree(out_dir) || st_mkdirs(out_dir)) { setup_fail(c, errno == ENOSPC ? "no_space" : "io", "Cannot create %s: %s", out_dir, strerror(errno)); goto out; }
    setup_progress(c, "companion", 0, 0, "reading the game archives");
    if (seed_romfs_open(&s->romfs, romfs_dir, &s->out.err) || !(s->dicts = seed_dicts_load(&s->romfs, &s->out.err))) goto out;
    if (!(dec = seed_decoder_new(s->dicts))) { fail_oom(s); goto out; }

    /* 1. text */
    for (uint64_t i = 0; i < s->romfs.count; i++) {
        const char *p = seed_romfs_name(&s->romfs, i);
        if (!starts(p, "Mals/") || !ends(p, ".sarc.zs")) continue;
        int64_t *g = realloc(s->lang_entries, (s->nlang + 1) * sizeof *g);
        char **names = g ? realloc(s->lang_names, (s->nlang + 1) * sizeof *names) : NULL;
        if (g) s->lang_entries = g;
        if (names) s->lang_names = names;
        if (!g || !names) { fail_oom(s); goto out; }
        const char *base = p + 5;
        if (!(s->lang_names[s->nlang] = strndup(base, strcspn(base, ".")))) { fail_oom(s); goto out; }
        s->lang_entries[s->nlang++] = (int64_t)i;
    }
    if (!(s->lang_actors = calloc(s->nlang ? s->nlang : 1, sizeof *s->lang_actors)) ||
        !(s->lang_nactors = calloc(s->nlang ? s->nlang : 1, sizeof *s->lang_nactors))) { fail_oom(s); goto out; }
    if (pouch_name_sources(s, dec) || run_parallel(s, text_task, s->nlang, 0, 0, "text")) goto out;
    buf_puts(&langs, "[");
    for (size_t l = 0; l < s->nlang; l++) {
        if (l) buf_add(&langs, ",", 1);
        cg_json_str(&langs, s->lang_names[l]);
        for (size_t a = 0; a < s->lang_nactors[l]; a++) if (set_add(&actors, s->lang_actors[l][a])) { fail_oom(s); goto out; }
    }
    buf_puts(&langs, "]");
    if (!s->nlang || !actors.n) { seed_err(&s->out.err, "missing_input", "No Mals pouch text found"); goto out; }

    /* 2. images */
    if (icon_jobs(s, dec, &actors, &cap, &icons, &aliases) || map_jobs(s, &cap)) goto out;
    if (romfs_get(s, dec, COMMON, &s->common, &s->common_n) || seed_sarc(s->common, s->common_n, &s->archive, &s->narchive, &s->out.err)) goto out;
    const SarcMember *combined = member(s->archive, s->narchive, "timg/__Combined.bntx");
    if (!combined) { seed_err(&s->out.err, "internal", "%s has no timg/__Combined.bntx", COMMON); goto out; }
    s->combined = combined->data; s->combined_n = combined->size;
    if (cg_bntx(s->combined, s->combined_n, &s->tex, &s->ntex, &s->out.err)) goto out;
    for (int l = 0; l < 3; l++) if (!(s->overview[l] = cg_new(CG_RGB, 2400, 2000))) { fail_oom(s); goto out; }
    if (!(s->order = malloc((s->njobs ? s->njobs : 1) * sizeof *s->order))) { fail_oom(s); goto out; }
    {   /* map tiles (decode + a large encode each) before the icons; the output does not depend on the order */
        size_t k = 0;
        for (size_t j = 0; j < s->njobs; j++) if (starts(s->jobs[j].dst, "map/")) s->order[k++] = j;
        for (size_t j = 0; j < s->njobs; j++) if (!starts(s->jobs[j].dst, "map/")) s->order[k++] = j;
        for (size_t j = 0; j < s->njobs; j++) if (s->jobs[j].overview_layer >= 0) atomic_fetch_add(&s->overview_left[s->jobs[j].overview_layer], 1);
    }
    uint64_t total = s->nlang + FIXED_TASKS + s->njobs + 1;   /* + the final sync */
    if (run_parallel(s, image_task, FIXED_TASKS + s->njobs, s->nlang, total, "images")) goto out;
    for (int l = 0; l < 3; l++)
        if (atomic_load(&s->overview_left[l])) { seed_err(&s->out.err, "internal", "Map overview %s incomplete", LAYERS[l][1]); goto out; }

    /* 3. index.json, the size limit */
    buf_printf(&index, "{\"schema\":1,\"generator\":%d,\"gameVersion\":\"1.4.3\",\"languages\":", GENERATOR_VERSION);
    buf_add(&index, langs.p, langs.n);
    buf_puts(&index, ",\"defaultLanguage\":\"USen\",\"icons\":"); buf_add(&index, icons.p, icons.n);
    buf_puts(&index, ",\"iconAliases\":"); buf_add(&index, aliases.p, aliases.n);
    buf_puts(&index, ",\"ui\":"); buf_add(&index, s->ui.p, s->ui.n);
    buf_puts(&index, ",\"uiSources\":"); buf_add(&index, s->ui_sources.p, s->ui_sources.n);
    buf_puts(&index, ",\"decor\":"); buf_add(&index, s->decor.p, s->decor.n);
    buf_puts(&index, ",\"palette\":"); buf_add(&index, s->palette.p, s->palette.n);
    buf_puts(&index, ",\"pageIcons\":"); buf_add(&index, s->page_icons.p, s->page_icons.n);
    buf_puts(&index, ",\"decorSources\":"); buf_add(&index, s->decor_sources.p, s->decor_sources.n);
    buf_puts(&index, ",\"map\":{\"world\":{\"minX\":-6000,\"maxX\":6000,\"minZ\":-5000,\"maxZ\":5000},\"cols\":12,\"rows\":10,"
                     "\"layers\":[\"Ground\",\"Sky\",\"UnderGround\"],\"lods\":[{\"level\":1,\"tilePx\":840},{\"level\":2,\"tilePx\":300}],"
                     "\"variant\":\"All-T revealed terrain; longest A/B suffix chain selects complete composites; prefer B over A [INFERENCE: post-event]\","
                     "\"orientation\":\"Native column/row order; tile 0,0 = minX,minZ; +Z down; static alignment checked at Lookout Landing, Kakariko, Hateno and Tarrey Town\"},"
                     "\"fonts\":");
    buf_add(&index, s->fonts.p, s->fonts.n);
    if (s->have_markers) buf_puts(&index, ",\"markers\":\"map/markers.json\"");
    buf_puts(&index, "}\n");
    if (index.oom || s->ui.oom || s->decor.oom || s->palette.oom || s->page_icons.oom || s->decor_sources.oom || icons.oom || aliases.oom || langs.oom) { fail_oom(s); goto out; }
    if (atomic_load(&s->out.bytes) > SIZE_LIMIT) {
        seed_err(&s->out.err, "internal", "Companion assets exceed the 120 MiB size limit (%llu bytes)", (unsigned long long)atomic_load(&s->out.bytes));
        goto out;
    }
    if (write_out(&s->out, "index.json", index.p, index.n)) goto out;
    setup_progress(c, "companion", total - 1, total, "writing to storage");
    if (setup_cancelled(c)) { atomic_store(&s->out.stop, 1); goto out; }
    if (sync_tree(out_dir, s->workers)) { seed_err(&s->out.err, errno == ENOSPC ? "no_space" : "io", "Cannot sync %s: %s", out_dir, strerror(errno)); goto out; }
    setup_progress(c, "companion", total, total, "done");
    rc = 0;
out:
    if (rc && !c->failed) {
        if (seed_err_set(&s->out.err)) { setup_fail(c, s->out.err.code, "%s", s->out.err.msg); rc = -1; }
        else if (atomic_load(&s->out.stop) || setup_cancelled(c)) rc = SETUP_CANCELLED;
        else { setup_fail(c, "internal", "The companion generator failed"); rc = -1; }
    }
    for (size_t i = 0; i < s->nsources; i++) { free(s->sources[i].key); free(s->sources[i].value); }
    free(s->sources);
    for (size_t l = 0; l < s->nlang; l++) {
        free(s->lang_names[l]);
        for (size_t a = 0; s->lang_actors && a < s->lang_nactors[l]; a++) free(s->lang_actors[l][a]);
        if (s->lang_actors) free(s->lang_actors[l]);
    }
    free(s->lang_names); free(s->lang_entries); free(s->lang_actors); free(s->lang_nactors);
    for (size_t j = 0; j < s->njobs; j++) free(s->jobs[j].src);
    free(s->jobs); free(s->order); free(s->tex); free(s->archive); free(s->common);
    for (int l = 0; l < 3; l++) cg_free(s->overview[l]);
    buf_free(&s->ui); buf_free(&s->ui_sources); buf_free(&s->decor); buf_free(&s->palette); buf_free(&s->page_icons);
    buf_free(&s->decor_sources); buf_free(&s->fonts); buf_free(&langs); buf_free(&icons); buf_free(&aliases); buf_free(&index);
    set_free(&actors); seed_decoder_free(dec); seed_dicts_free(s->dicts); seed_romfs_close(&s->romfs);
    pthread_mutex_destroy(&s->mu); pthread_cond_destroy(&s->cv);
    free(s);
    return rc;
}
