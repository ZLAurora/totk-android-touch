/* Fresh-process comparisons: no Vulkan objects or caches shared across modes. Every invocation runs
   TOTK_VK_REC_ONE_PRIMARY=0/1; binary fixtures compare every output byte between those launch arms.
   Run vkrectest for identity/boundaries/mixed sections (including deferred payload/storage/cancel)/split passes,
   --job-reuse for the 4096-pass storage oracle/leases/failures, --lag-guards for backend copy/guard liveness plus
   private uniforms, and --launch-policy for hardware-free framegen/statistics startup decisions. Pixel fixtures need the completed draw seed. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>

static int run_arm(const char *binary, const char *mode, const char *output, const char *threads, int widen, int one_primary) {
    int fault = !strncmp(mode, "--rec-fail-", 11), reuse = !strcmp(mode, "--rec-job-reuse") ||
        !strcmp(mode, "--rec-vma-fallback") || !strcmp(mode, "--rec-storage") ||
        !strcmp(mode, "--rec-leases") || !strcmp(mode, "--rec-leases-unlimited") || !strcmp(mode, "--rec-storage-warm");
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 0; }
    if (!pid) {
        setenv("TOTK_VK_REC_THREADS", threads, 1);
        setenv("TOTK_VK_COMPILE_THREADS", "0", 1); /* a cold cache must not intentionally omit asynchronous pipelines */
        setenv("TOTK_VK_REC_ONE_PRIMARY", one_primary ? "1" : "0", 1);
        if (strcmp(mode, "--rec-identity")) { /* deterministic worker gates and same-chunk memo reuse */
            setenv("TOTK_VK_REC_INLINE_DRAWS", "0", 1); setenv("TOTK_VK_REC_CHUNK_DRAWS", "128", 1);
            setenv("TOTK_VK_REC_LEAD_DRAWS", "32", 1);
        }
        if (widen) setenv("TOTK_VK_TEST_WIDEN_U8", "1", 1); else unsetenv("TOTK_VK_TEST_WIDEN_U8");
        unsetenv("TOTK_VK_OCCLUSION"); unsetenv("TOTK_VK_PASS_DUMP"); unsetenv("TOTK_VK_PASS_PROFILE"); unsetenv("TOTK_VK_PASSPROBE"); unsetenv("TOTK_VK_GPU_PROFILE");
        unsetenv("TOTK_VK_PASS_MERGE");
        if (!strcmp(mode, "--one-primary-launch")) {
            execl(binary, binary, mode, (char *)NULL); perror(binary); _exit(127);
        }
        setenv("TOTK_FRAMEGEN", "off", 1); unsetenv("TOTK_VK_GPU_PROFILE_STATS");
        if (!strcmp(mode, "--xlate-stats")) {
            setenv("TOTK_VK_XLATE_STATS", "1", 1);
            if (!freopen(output, "w", stdout) || dup2(STDOUT_FILENO, STDERR_FILENO) < 0) _exit(127);
            execl(binary, binary, mode, (char *)NULL); perror(binary); _exit(127);
        }
        if (!strcmp(mode, "--lag-uniform") || !strcmp(mode, "--lag-guards")) {
            setenv("TOTK_FRAME_LAG", "1", 1); setenv("TOTK_VK_REC_ASYNC_SUBMIT", "1", 1);
            setenv("TOTK_VK_OFF", "block", 1);
            unsetenv("TOTK_VK_LAG_CAPTURE"); unsetenv("TOTK_VK_LAG_MODE"); unsetenv("TOTK_VK_UB_RENAME");
            execl(binary, binary, mode, (char *)NULL); perror(binary); _exit(127);
        }
        if (fault || reuse) {
            setenv("TOTK_FRAME_LAG", "1", 1); setenv("TOTK_VK_CHUNK_DRAWS", "600", 1);
            setenv("TOTK_VK_REC_ASYNC_SUBMIT", "1", 1); setenv("TOTK_VK_OFF", "block", 1);
            unsetenv("TOTK_VK_LAG_CAPTURE"); unsetenv("TOTK_VK_LAG_MODE"); unsetenv("TOTK_VK_UB_RENAME");
            if (fault && !freopen(output, "w", stderr)) _exit(127);
            execl(binary, binary, mode, (char *)NULL); perror(binary); _exit(127);
        }
        execl(binary, binary, mode, output, (char *)NULL);
        perror(binary); _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) { perror("waitpid"); return 0; }
    if (fault) {
        const char *expected = !strcmp(mode, "--rec-fail-info") ? "host allocation failed in vk_record_descriptors: j->info" :
            !strcmp(mode, "--rec-fail-calloc") ? "host allocation failed" :
            !strcmp(mode, "--rec-fail-map") ? "vkMapMemory" :
            !strcmp(mode, "--rec-fail-allocate") ? "vkAllocateMemory" :
            !strcmp(mode, "--rec-fail-slab") ? "slab mmap failed" : "vkEndCommandBuffer(j->cb) failed: -5";
        FILE *f = fopen(output, "r"); char line[4096]; int diagnosed = 0, continued = 0;
        if (f) {
            while (fgets(line, sizeof line, f)) {
                diagnosed |= strstr(line, expected) != NULL;
                continued |= strstr(line, "unexpectedly continued") != NULL;
            }
            fclose(f);
        }
        int ok = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT && diagnosed && !continued;
        printf("%s: %s aborts at the failing operation with its diagnostic\n", ok ? "ok" : "FAIL", mode);
        return ok;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
static int xlate_log(const char *path) {
    FILE *file = fopen(path, "r"); if (!file) return 0;
    char line[8192]; int summary = 0, recording = 0, uniforms = 0, stages = 0, textures = 0, types = 0, coverage = 0;
    int ok = 1;
    while (fgets(line, sizeof line, file)) {
        char *p;
        if ((p = strstr(line, "[xlate-summary]"))) {
            unsigned long long frame, frames, submissions, draws, unique, decisions, known, reads, total, overflow; int final;
            int n = sscanf(p, "[xlate-summary] frame=%llu final=%d frames=%llu submissions=%llu draws=%llu uniform_unique=%llu uniform_decisions=%llu uniform_known=%llu read_unique=%llu read_decisions=%llu overflow=%llu",
                &frame, &final, &frames, &submissions, &draws, &unique, &decisions, &known, &reads, &total, &overflow);
            ok &= n == 11 && frame == 300 && !final && frames == 300 && submissions >= 4 && draws == 5 &&
                decisions == 10 && unique <= known && known <= decisions && reads <= total && total >= 20 && !overflow;
            summary++;
        }
        if ((p = strstr(line, "[xlate-freeze]"))) {
            unsigned long long frame, requests, hits, misses, direct; int stage;
            int n = sscanf(p, "[xlate-freeze] frame=%llu stage=%d requests=%llu hits=%llu misses=%llu direct=%llu",
                &frame, &stage, &requests, &hits, &misses, &direct);
            ok &= n == 6 && frame == 300 && stage >= 0 && stage < 2 && requests == 5 && hits + misses + direct == requests;
            stages++;
        }
        if ((p = strstr(line, "[xlate-uniform]"))) {
            uniforms++;
            for (char *token = strtok(p, " \n"); token; token = strtok(NULL, " \n")) {
                if (!strncmp(token, "changed_", 8) || !strncmp(token, "address_changed_", 16)) {
                    char *equal = strchr(token, '='); char *end; double fraction = equal ? strtod(equal + 1, &end) : -1;
                    ok &= equal && end != equal + 1 && !*end && fraction >= 0 && fraction <= 1;
                }
            }
        }
        if ((p = strstr(line, "xlate recordings: frame="))) {
            unsigned long long frame, frames, submissions, recordings, exact, abstract, draws, draw_exact, draw_abstract; int partial;
            int n = sscanf(p, "xlate recordings: frame=%llu frames=%llu partial=%d submissions=%llu recordings=%llu repeated_exact=%llu repeated_abstract=%llu draws=%llu repeated_draws_exact=%llu repeated_draws_abstract=%llu",
                &frame, &frames, &partial, &submissions, &recordings, &exact, &abstract, &draws, &draw_exact, &draw_abstract);
            ok &= n == 10 && frame == 300 && frames == 300 && !partial && submissions >= 4 && recordings == 5 &&
                exact == 2 && abstract == 3 && draws == 5 && draw_exact == 2 && draw_abstract == 3;
            recording++;
        }
        if ((p = strstr(line, "xlate recording coverage:"))) {
            unsigned long long exact, abstract, draws_exact, draws_abstract, ops, bytes, unknown, incomplete, overflow_exact, overflow_abstract;
            int n = sscanf(p, "xlate recording coverage: exact=%llu abstract=%llu draws_exact=%llu draws_abstract=%llu ops=%llu bytes=%llu unknown_ops=%llu incomplete=%llu overflow_exact=%llu overflow_abstract=%llu",
                &exact, &abstract, &draws_exact, &draws_abstract, &ops, &bytes, &unknown, &incomplete, &overflow_exact, &overflow_abstract);
            ok &= n == 10 && exact == 5 && abstract == 5 && draws_exact == 5 && draws_abstract == 5 &&
                ops > 5 && bytes > 0 && !unknown && !incomplete && !overflow_exact && !overflow_abstract;
            coverage++;
        }
        if ((p = strstr(line, "[xlate-textures]"))) {
            unsigned long long frame, uses, peak, max, unknown; double distinct;
            int n = sscanf(p, "[xlate-textures] frame=%llu uses=%llu distinct_per_frame=%lf peak_distinct=%llu max_id=%llu view_unknown=%llu",
                &frame, &uses, &distinct, &peak, &max, &unknown);
            ok &= n == 6 && frame == 300 && uses == 5 && peak == 1 && max == 1 && distinct > 0 && distinct <= 1 && !unknown;
            textures++;
        }
        if ((p = strstr(line, "[xlate-texture-types]"))) {
            unsigned long long frame, targets, views, ids, overflow;
            int n = sscanf(p, "[xlate-texture-types] frame=%llu target_mask=0x%llx view_mask=0x%llx ids=%llu overflow=%llu",
                &frame, &targets, &views, &ids, &overflow);
            ok &= n == 5 && targets && views == 2 && ids == 1 && !overflow; types++;
        }
    }
    ok &= !ferror(file) && summary == 1 && recording == 1 && coverage == 1 && uniforms == 2 && stages == 2 && textures == 1 && types == 1;
    fclose(file); printf("%s: translation diagnostic lines, ratios, previous-frame repetition and texture view histogram\n", ok ? "ok" : "FAIL");
    return ok;
}
static int identical(const char *a, const char *b, uint64_t magic) {
    FILE *left = fopen(a, "rb"), *right = fopen(b, "rb");
    int ok = left && right; size_t total = 0; uint8_t l[4096], r[4096];
    while (ok) {
        size_t nl = fread(l, 1, sizeof l, left), nr = fread(r, 1, sizeof r, right);
        if (nl != nr || memcmp(l, r, nl)) { fprintf(stderr, "identity differs at byte block %zu\n", total); ok = 0; break; }
        total += nl;
        if (!nl) {
            if (magic < (1u<<20)) { ok = !ferror(left) && !ferror(right) && total && (!magic || total == magic); break; } /* raw fixture size, or any nonempty exact output */
            uint64_t footer[2];
            ok = !ferror(left) && !ferror(right) && total > sizeof footer && !fseek(left, -(long)sizeof footer, SEEK_END)
                && fread(footer, sizeof footer, 1, left) == 1 && footer[0] == magic && footer[1] == 12;
            break;
        }
    }
    if (left) fclose(left); if (right) fclose(right);
    printf("%s: %zu byte identity including rendered pixels, actual passes/draws, ordered samples and timestamp assertions\n", ok ? "ok" : "FAIL", total);
    return ok;
}
/* Every fixture invocation owns both launch arms, so specialized matrices cannot accidentally omit exact1.
   Compare the complete binary payload, never stdout timings or the inherently variable timestamp values. */
static int run(const char *binary, const char *mode, const char *output, const char *threads, int widen) {
    int payload = !strcmp(mode, "--rec-identity") || !strcmp(mode, "--rec-boundaries") ||
        !strcmp(mode, "--rec-submit") || !strcmp(mode, "--rec-sections") ||
        !strncmp(mode, "--split-", 8) || !strcmp(mode, "--load-elide") || !strcmp(mode, "--feedback-narrow");
    char enabled[PATH_MAX];
    if (payload && snprintf(enabled, sizeof enabled, "%s-one-primary", output) >= (int)sizeof enabled) return 0;
    int off = run_arm(binary, mode, output, threads, widen, 0);
    if (off && !strcmp(mode, "--xlate-stats")) off = xlate_log(output);
    int on = run_arm(binary, mode, payload ? enabled : output, threads, widen, 1);
    int ok = off && on;
    if (payload) {
        printf("%s workers=%s widening=%d one-primary off vs on: ", mode, threads, widen);
        if (ok) ok = identical(output, enabled, 0);
        unlink(enabled);
    }
    return ok;
}
int main(int argc, char **argv) {
    char binary[PATH_MAX]; ssize_t len = readlink("/proc/self/exe", binary, sizeof binary - 1);
    if (len < 0) { perror("/proc/self/exe"); return 1; } binary[len] = 0;
    char *base = strrchr(binary, '/'); if (!base) return 1; strcpy(base + 1, "vkdrawtest");
    char dir[] = "./vkrecord-XXXXXX"; if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    if (argc == 2 && !strcmp(argv[1], "--launch-policy")) {
        const char *framegen[] = {"off", "1", "interp", "hybrid", "locked", "adaptive"};
        const char *workers[] = {"0", "2"}; int ok = 1;
        for (unsigned f = 0; f < sizeof framegen / sizeof *framegen; f++) for (int stats = 0; stats < 2; stats++) {
            setenv("TOTK_FRAMEGEN", framegen[f], 1); setenv("TOTK_VK_GPU_PROFILE_STATS", stats ? "1" : "0", 1);
            for (unsigned w = 0; w < sizeof workers / sizeof *workers; w++)
                ok &= run(binary, "--one-primary-launch", NULL, workers[w], 0);
        }
        rmdir(dir); return !ok;
    }
    if (argc==2 && !strcmp(argv[1],"--alpha-clear")) {
        int ok=1;
        for (unsigned workers=0;workers<2;workers++) {
            ok &= run(binary,"--alpha-test","unused",workers?"2":"0",0);
            ok &= run(binary,"--clear-on-read","unused",workers?"2":"0",0);
        }
        rmdir(dir); return !ok;
    }
    if (argc == 2 && !strcmp(argv[1], "--lag-guards")) {
        char backend[PATH_MAX]; strcpy(backend, binary); strcpy(strrchr(backend, '/') + 1, "vktest");
        const char *workers[] = {"0", "2", "3"}; int ok = 1;
        for (unsigned i = 0; i < sizeof workers / sizeof *workers; i++) {
            ok &= run(backend, "--lag-guards", NULL, workers[i], 0);
            ok &= run(binary, "--lag-uniform", NULL, workers[i], 0);
        }
        rmdir(dir); return !ok;
    }
    if (argc == 2 && (!strcmp(argv[1],"--load-elide") || !strcmp(argv[1],"--feedback-narrow"))) {
        int feedback=!strcmp(argv[1],"--feedback-narrow");
        char reference[PATH_MAX]; snprintf(reference,sizeof reference,"%s/reference.bin",dir);
        int ok = 1;
        for (int enabled = 0; enabled < 2; enabled++) for (int worker = 0; worker < 2; worker++) {
            char path[PATH_MAX]; snprintf(path,sizeof path,"%s/pixels-%d-%d.bin",dir,enabled,worker);
            setenv(feedback ? "TOTK_VK_FEEDBACK_NARROW" : "TOTK_VK_LOAD_ELIDE",enabled ? "1" : "0",1);
            ok &= run(binary,argv[1],path,worker ? "2" : "0",0);
            if (!enabled && !worker) { if (rename(path,reference)) ok = 0; }
            else { ok &= identical(reference,path,(feedback ? 7 : 9)*64*64*4); unlink(path); }
        }
        unlink(reference); rmdir(dir); return !ok;
    }
    if (argc == 2 && !strcmp(argv[1], "--job-reuse")) {
        int ok = 1; const char *workers[] = {"0", "2", "3", "4"};
        for (int i = 0; i < 4; i++) {
            ok &= run(binary, "--rec-job-reuse", NULL, workers[i], 0);
            ok &= run(binary, "--rec-vma-fallback", NULL, workers[i], 0);
            ok &= run(binary, "--rec-storage", NULL, workers[i], 0);
            ok &= run(binary, "--rec-leases", NULL, workers[i], 0);
            ok &= run(binary, "--rec-leases-unlimited", NULL, workers[i], 0);
            ok &= run(binary, "--rec-storage-warm", NULL, workers[i], 0);
        }
        const char *failures[] = {"--rec-fail-info", "--rec-fail-calloc", "--rec-fail-slab", "--rec-fail-map", "--rec-fail-allocate", "--rec-fail-end"};
        for (unsigned i = 0; i < sizeof failures / sizeof *failures; i++) {
            char log[PATH_MAX]; snprintf(log, sizeof log, "%s/failure-%u.log", dir, i);
            ok &= run(binary, failures[i], log, "2", 0); unlink(log);
        }
        rmdir(dir); return !ok;
    }
    int split_only = argc == 2 && !strcmp(argv[1], "--split-only");
    if (argc != 1 && !split_only) { rmdir(dir); return 2; }
    const char *threads[] = {"0", "2", "3"}, *modes[] = {"--rec-identity", "--rec-boundaries", "--rec-submit"};
    const uint64_t magic[] = {0x5245434944454e33ull, 0x524543424e445933ull, 0x524543424e445933ull}; int ok = 1;
    if (!split_only) {
    char sections_reference[PATH_MAX]; snprintf(sections_reference, sizeof sections_reference, "%s/sections-reference.bin", dir);
    for (unsigned i = 0; i < sizeof threads / sizeof *threads; i++) {
        char output[PATH_MAX]; snprintf(output, sizeof output, "%s/sections-%s.bin", dir, threads[i]);
        ok &= run(binary, "--rec-sections", output, threads[i], 0);
        if (!i) { if (rename(output, sections_reference)) ok = 0; }
        else { ok &= identical(sections_reference, output, 0); unlink(output); }
    }
    unlink(sections_reference);
    for (int i = 0; i < 2; i++) {
        char log[PATH_MAX]; snprintf(log, sizeof log, "%s/xlate-%d.log", dir, i);
        ok &= run(binary, "--xlate-stats", log, i ? "2" : "0", 0);
        ok &= xlate_log(log); unlink(log);
    }
    /* Opt-in hot-path switches: the same bytes as the default inline recorder, in fresh processes at 0/2/3 workers.
       TOTK_VK_REC_WAKE_BATCH with batched worker wakes and with workers polling the queue before parking (the boundary
       stream's worker gates run under both); TOTK_VK_TEX_MEMO with every texture/sampler handle changing per draw. */
    const char *arms[][2] = {{"TOTK_VK_REC_WAKE_BATCH", "1"}, {"TOTK_VK_REC_WAKE_BATCH", "200"}, {"TOTK_VK_TEX_MEMO", "1"}};
    for (int mode = 0; mode < 3; mode++) {
        char reference[PATH_MAX], output[PATH_MAX];
        snprintf(reference, sizeof reference, "%s/arm-m%d-reference.bin", dir, mode);
        if (!run(binary, modes[mode], reference, "0", 0)) { fprintf(stderr, "%s failed inline (arm reference)\n", modes[mode]); ok = 0; }
        for (unsigned arm = 0; arm < sizeof arms / sizeof *arms; arm++) for (int i = 0; i < 3; i++) {
            snprintf(output, sizeof output, "%s/arm-m%d-%u-%s.bin", dir, mode, arm, threads[i]);
            setenv(arms[arm][0], arms[arm][1], 1);
            int ran = run(binary, modes[mode], output, threads[i], 0);
            unsetenv(arms[arm][0]);
            if (!ran) { fprintf(stderr, "%s failed with %s workers, %s=%s\n", modes[mode], threads[i], arms[arm][0], arms[arm][1]); ok = 0; }
            printf("%s threads=0 vs %s, %s=%s: ", modes[mode], threads[i], arms[arm][0], arms[arm][1]);
            ok &= identical(reference, output, magic[mode]); unlink(output);
        }
        unlink(reference);
    }
    for (int mode = 0; mode < 3; mode++) {
        char output[2][3][PATH_MAX];
        for (int widen = 0; widen < 2; widen++) {
            for (int i = 0; i < 3; i++) {
                snprintf(output[widen][i], sizeof output[widen][i], "%s/m%d-w%d-threads-%s.bin", dir, mode, widen, threads[i]);
                if (!run(binary, modes[mode], output[widen][i], threads[i], widen)) {
                    fprintf(stderr, "%s failed with %s workers, widening=%d\n", modes[mode], threads[i], widen); ok = 0;
                }
            }
            for (int i = 1; i < 3; i++) {
                printf("%s threads=0 vs %s, uint8 widening=%d: ", modes[mode], threads[i], widen);
                ok &= identical(output[widen][0], output[widen][i], magic[mode]);
            }
        }
        printf("%s native vs forced uint8 widening: ", modes[mode]);
        ok &= identical(output[0][0], output[1][0], magic[mode]);
        for (int widen = 0; widen < 2; widen++) for (int i = 0; i < 3; i++) unlink(output[widen][i]);
    }
    }
    /* Pin both LRZ arms and the absent/default arm: lavapipe has no LRZ, so the child checks exact decisions as
       well as near/far color and depth pixels. --split-only runs this matrix in isolation (including under TSan). */
    const char *split_modes[] = {"--split-pass", "--split-depth", "--split-depth-only", "--split-depth-mask"};
    for (unsigned mode = 0; mode < 4; mode++) for (int safe = 0; safe < 3; safe++) for (int arm = 0; arm < 4; arm++) {
        if (safe < 2) setenv("TOTK_VK_SPLIT_LRZ_SAFE", safe ? "1" : "0", 1); else unsetenv("TOTK_VK_SPLIT_LRZ_SAFE");
        const char *split[] = {NULL, "64", "1"}, *workers = arm == 3 ? "2" : threads[arm];
        if (arm == 3) setenv("TOTK_FRAME_LAG", "1", 1);
        char output[3][PATH_MAX];
        for (int s = 0; s < 3; s++) {
            snprintf(output[s], sizeof output[s], "%s/split-%u-%d-%d-%d.bin", dir, mode, safe, arm, s);
            if (split[s]) setenv("TOTK_VK_SPLIT_PASS_DRAWS", split[s], 1); else unsetenv("TOTK_VK_SPLIT_PASS_DRAWS");
            if (!run(binary, split_modes[mode], output[s], workers, 0)) {
                fprintf(stderr, "%s failed with %s workers, split=%s LRZ_SAFE=%d%s\n", split_modes[mode], workers,
                    split[s] ? split[s] : "off", safe, arm == 3 ? ", lag" : "");
                ok = 0;
            }
        }
        unsetenv("TOTK_VK_SPLIT_PASS_DRAWS"); unsetenv("TOTK_FRAME_LAG");
        for (int s = 1; s < 3; s++) {
            printf("%s threads=%s%s LRZ_SAFE=%d off vs every %s draws: ", split_modes[mode], workers, arm == 3 ? " lag" : "", safe, split[s]);
            ok &= identical(output[0], output[s], 0x53504c4954504153ull);
        }
        for (int s = 0; s < 3; s++) unlink(output[s]);
    }
    unsetenv("TOTK_VK_SPLIT_LRZ_SAFE");
    rmdir(dir); return !ok;
}
