/* Included only by vk_gpuprof.c under TOTK_VK_TESTS. Synthetic fenced leases,
 * real FIFO/consumer/parent aggregation/encoder/rotation/close. No Vulkan init. */
static double fixture_elapsed_ms(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
}
static void fixture_wait(struct timespec start, double seconds) {
    uint64_t ns = (uint64_t)(seconds * 1e9);
    start.tv_sec += ns / UINT64_C(1000000000);
    start.tv_nsec += ns % UINT64_C(1000000000);
    if (start.tv_nsec >= 1000000000) { start.tv_sec++; start.tv_nsec -= 1000000000; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &start, NULL) == EINTR) {}
}
/* A missed whole producer frame is not permission to inject a later burst at
   many times the named rate. --catch-up=1 explicitly retains that stress mode. */
static double fixture_deadline_rebase(double elapsed, double deadline, double period, unsigned catch_up) {
    return !catch_up && elapsed > deadline + period ? elapsed - deadline : 0;
}
static int fixture_report_check(int condition, const char *message) {
    if (condition) return 0;
    fprintf(stderr, "FAIL compact report: %s\n", message);
    return 1;
}
int vk_gpuprof_test_report_contract(void) {
    FILE *file = tmpfile();
    compact_pending = calloc(1, sizeof *compact_pending);
    if (!file || !compact_pending) {
        if (file) fclose(file);
        free(compact_pending); compact_pending = NULL;
        return 1;
    }
    output_file = file;
    compact.start = 1; compact.end = compact.frames = 300;
    compact.observed = compact.selected = compact.valid = 2; compact.timestamps = 4;
    for (unsigned i = 0; i < 2; i++) {
        gp_compact_entry *e = &compact.key[i * 64];
        e->id = i + 1; e->key.pipeline = i + 1;
        e->v[GC_OBSERVED].integer = e->v[GC_SELECTED].integer = e->v[GC_VALID].integer = 1;
    }
    compact_report();
    int failed = fixture_report_check(compact_pending->frames == 300 && !compact.frames,
        "freezing retains the old window and resets the active counters");
    failed += fixture_report_check(fixture_service_delay_ns(1, 1000000, 2700) == 1700000 &&
        fixture_service_delay_ns(1, 9000000, 2700) == 0,
        "CPU scaling charges the unspent thread-CPU budget, not elapsed host wall time");
    failed += fixture_report_check(fixture_deadline_rebase(3, 1, 1, 0) == 2 &&
        fixture_deadline_rebase(3, 1, 1, 1) == 0 && fixture_deadline_rebase(1.5, 1, 1, 0) == 0,
        "late producer frames rebase without dropping work unless catch-up stress is explicit");
    failed += fixture_report_check(!ftell(file) && !compact_report_cursor,
        "freezing does not serialize a row or close the snapshot");
    compact.start = 301; compact.end = 330; compact.frames = 30;
    compact.key[0].id = 99; compact.key[0].v[GC_SELECTED].integer = 999;
    failed += fixture_report_check(compact_pending->start == 1 && compact_pending->end == 300 &&
        compact_pending->frames == 300 && compact_pending->key[0].id == 1 &&
        compact_pending->key[0].v[GC_SELECTED].integer == 1,
        "active next-window mutation cannot change the frozen snapshot");
    compact_report_pump(64);
    failed += fixture_report_check(compact_report_cursor == 64 && compact_pending->frames == 300,
        "one completed-frame pump scans exactly 64 dictionary indices without closing coverage");
    fflush(file); rewind(file);
    char text[16384]; size_t n = fread(text, 1, sizeof text - 1, file); text[n] = 0;
    failed += fixture_report_check(strstr(text, "\"key_id\":1,") &&
        !strstr(text, "\"key_id\":2,") && !strstr(text, "\"type\":\"pipeline_window_coverage\""),
        "a partial scan emits only reached rows and never premature coverage");
    fseek(file, 0, SEEK_END);
    memset(&compact, 0, sizeof compact);
    background_flush(); /* the production terminal path; no renderer or GPU initialization */
    failed += fixture_report_check(compact_report_cursor == GP_COMPACT_KEYS && !compact_pending->frames,
        "terminal flush drains every remaining dictionary index");
    fflush(file); rewind(file);
    n = fread(text, 1, sizeof text - 1, file); text[n] = 0;
    const char *first = strstr(text, "\"key_id\":1,");
    const char *second = strstr(text, "\"key_id\":2,");
    const char *coverage = strstr(text, "\"type\":\"pipeline_window_coverage\"");
    failed += fixture_report_check(first && second && coverage && first < second && second < coverage &&
        !strstr(coverage + 1, "\"type\":\"pipeline_window_coverage\""),
        "terminal coverage follows all frozen rows exactly once");
    failed += fixture_report_check(!atomic_load(&output_failures), "snapshot output completes without failure");
    fclose(file); output_file = NULL;
    free(compact_pending); compact_pending = NULL;
    return failed;
}

int vk_gpuprof_test_compact_run(int argc, char **argv) {
    unsigned seconds = 600, fps = 0, pace = 0, loss = 0, limit_frames = 0;
    unsigned parts = 2, gpu_held = 0, nonblocking = 0, writer_stall_ms = 0, coupled_stall = 0;
    unsigned parent_budget = 0, spread_percent = 0, unavailable_frame = 0;
    unsigned catch_up = 0;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 == argc) return 2;
        char *end; unsigned long value = strtoul(argv[i + 1], &end, 10);
        if (!*argv[i + 1] || *end || value > 360000) return 2;
        if (!strcmp(argv[i], "--seconds") && value && value <= 3600) seconds = (unsigned)value;
        else if (!strcmp(argv[i], "--fps") && (!value || (value >= 20 && value <= 60))) fps = (unsigned)value;
        else if (!strcmp(argv[i], "--pace") && value <= 1) pace = (unsigned)value;
        else if (!strcmp(argv[i], "--loss") && value <= 1) loss = (unsigned)value;
        else if (!strcmp(argv[i], "--frames") && value) limit_frames = (unsigned)value;
        else if (!strcmp(argv[i], "--worker-scale") && value >= 1000 && value <= 10000)
            fixture_worker_scale = (unsigned)value;
        else if (!strcmp(argv[i], "--parts") && value >= 2 && value <= 32) parts = (unsigned)value;
        else if (!strcmp(argv[i], "--gpu-held") && value <= 8) gpu_held = (unsigned)value;
        else if (!strcmp(argv[i], "--nonblocking") && value <= 1) nonblocking = (unsigned)value;
        else if (!strcmp(argv[i], "--writer-stall-ms") && value <= 10000) writer_stall_ms = (unsigned)value;
        else if (!strcmp(argv[i], "--coupled-stall") && value <= 1) coupled_stall = (unsigned)value;
        else if (!strcmp(argv[i], "--sample-budget") && value <= 16) parent_budget = (unsigned)value;
        else if (!strcmp(argv[i], "--spread-percent") && value <= 100) spread_percent = (unsigned)value;
        else if (!strcmp(argv[i], "--catch-up") && value <= 1) catch_up = (unsigned)value;
        else if (!strcmp(argv[i], "--unavailable-frame")) unavailable_frame = (unsigned)value;
        else return 2;
    }
    /* Like the renderer's shader cache, these names never move or mutate while
       leases/dictionary source tuples can refer to them. Dataset bytes are
       identical to the former per-observation snprintf workload. */
    unsigned max_frames = limit_frames ? limit_frames : seconds * (fps ? fps : 60) + 1;
    unsigned identity_count = 4096 + ((max_frames + 299) / 300) * 64;
    char (*names)[2][80] = calloc(identity_count, sizeof *names);
    if (!names) return 1;
    for (unsigned i = 0; i < identity_count; i++) {
        uint64_t variant = i < 4096 ? i :
            ((i - 4096) % 64) * 64 + (uint64_t)((i - 4096) / 64) * 4096;
        snprintf(names[i][0], sizeof names[i][0], "%016llx%016llx%016llx%016llx-vs",
            (unsigned long long)variant, (unsigned long long)(variant * 13),
            (unsigned long long)(variant * 31), (unsigned long long)(variant * 43));
        snprintf(names[i][1], sizeof names[i][1], "%016llx%016llx%016llx%016llx-fs",
            (unsigned long long)(variant * 7), (unsigned long long)(variant * 17),
            (unsigned long long)(variant * 37), (unsigned long long)(variant * 47));
    }
    fixture_mode = 1; vk_gpuprof_on = 1; vk_gpuprof_pipeline_on = 4; sample_budget = parent_budget;
    const char *raw = getenv("TOTK_VK_GPU_PROFILE_PIPELINES_RAW");
    pipeline_raw = raw && !strcmp(raw, "1");
    pipeline_launch_phase = 17; pipeline_random = 1;
    V.timestamp_bits = 48; V.props.limits.timestampPeriod = 1;
    V.props.limits.timestampComputeAndGraphics = 1;
    snprintf(V.props.deviceName, sizeof V.props.deviceName, "host synthetic fenced-lease fixture (no GPU)");
    game_reference_w = 1600; game_reference_h = 900;
    if (coupled_stall) fixture_coupled_stall_ms = writer_stall_ms;
    else { output_sink.stall_ms = writer_stall_ms; output_sink.stall_after = 65536; }
    background_start();
    struct timespec wall_start; clock_gettime(CLOCK_MONOTONIC, &wall_start);
    double logical = 0, paced = 0, producer_wait_ms = 0, producer_rebase_seconds = 0, producer_max_late_ms = 0;
    uint64_t producer_late_frames = 0, producer_late_slots = 0;
    unsigned frames = 0;
    uint32_t population = VK_GPUPROF_PIPE_ORDINALS;
    vk_gpuprof_slot *pending_gpu[8] = {0};
    uint64_t attempted = 0;
    while (limit_frames ? frames < limit_frames : logical < seconds - 1e-8) {
        uint64_t number = ++frames;
        frame = number; /* production drop diagnostics use the recording frame */
        unsigned logical_fps = fps ? fps : 20 + ((unsigned)logical / 20 % 5) * 10;
        if (pace) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = fixture_elapsed_ms(wall_start, now) / 1000;
            if (elapsed > paced + 1.0 / logical_fps) producer_late_frames++;
            double shift = fixture_deadline_rebase(elapsed, paced, 1.0 / logical_fps, catch_up);
            paced += shift; producer_rebase_seconds += shift;
        }
        uint32_t phase = (pipeline_launch_phase + (uint32_t)(number - 1) * 4) & (population - 1);
        for (unsigned part = 0; part < parts; part++) {
            /* Device submits span its ~20 ms recording interval, not an
               instantaneous 18-lease burst followed by an idle frame. */
            if (pace && spread_percent) {
                double deadline = paced + part * spread_percent / (100.0 * parts * logical_fps);
                fixture_wait(wall_start, deadline);
                struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                double late_ms = fixture_elapsed_ms(wall_start, now) - deadline * 1000;
                if (late_ms > 1000.0 / (parts * logical_fps)) producer_late_slots++;
                if (late_ms > producer_max_late_ms) producer_max_late_ms = late_ms;
            }
            unsigned gpu_at = gpu_held ? attempted % gpu_held : 0;
            if (gpu_held && attempted >= gpu_held) background_enqueue(&pending_gpu[gpu_at], 0);
            uint64_t sequence = ++recording_sequence;
            vk_gpuprof_slot *slot = NULL;
            if (nonblocking) {
                (void)background_acquire(&slot); /* exact production failure path */
            } else {
                pthread_mutex_lock(&background_mu);
                struct timespec wait_start, wait_end; clock_gettime(CLOCK_MONOTONIC, &wait_start);
                while (!background_nfree) pthread_cond_wait(&background_cv, &background_mu);
                clock_gettime(CLOCK_MONOTONIC, &wait_end);
                producer_wait_ms += fixture_elapsed_ms(wait_start, wait_end);
                slot = background_free[--background_nfree];
                pthread_mutex_unlock(&background_mu);
            }
            attempted++;
            if (!slot) { if (gpu_held) pending_gpu[gpu_at] = NULL; continue; }
            int parent_selected = !sample_budget || part < sample_budget;
            slot->n = 1; slot->nsegment = 1; slot->nt = parent_selected; slot->ns = slot->dropped = 0;
            slot->npipeline = slot->pipeline_timed = slot->pipeline_unselected = 0;
            slot->discard = 0; slot->sealed = part + 1 == parts ? number : 0; slot->sequence = sequence;
            slot->lease = slot->sequence; slot->has_pre = 0;
            slot->domain = (vk_gpuprof_time_domain){1, 1, 0, 48, 1};
            uint64_t tick = number * UINT64_C(20000000) + part * UINT64_C(10000000) / parts;
            unsigned span_first = !sample_budget || !part;
            unsigned span_last = !sample_budget || part + 1 == parts;
            slot->span_writes = span_first + span_last;
            slot->span_ticks[0] = (vk_gpuprof_timestamp){tick, span_first};
            slot->span_ticks[1] = (vk_gpuprof_timestamp){tick + UINT64_C(10000000) / parts, span_last};
            slot->ticks[0] = (vk_gpuprof_timestamp){tick + 100, 1};
            uint64_t pass_ns = UINT64_C(10000000) / parts / 2;
            if (pass_ns > 1000000) pass_ns = 1000000;
            slot->ticks[1] = (vk_gpuprof_timestamp){tick + 100 + pass_ns, 1};
            slot->rec[0] = (gp_record){.frame = number, .instance = part + 1,
                .draws = 4096 / parts, .selected = parent_selected,
                .timestamp_q1 = parent_selected, .timestamp_count = parent_selected * 2,
                .key = {.w = 1600, .h = 900, .samples = 1, .shader = part + 1}};
            gp_segment *s = &slot->segment[0];
            *s = (gp_segment){.frame = number, .first = 0, .last = 1,
                .pipeline_population = population, .pipeline_phase = phase,
                .first_valid = sample_budget && !part, .last_valid = sample_budget && part + 1 == parts,
                .timestamp_count = sample_budget ? (!part) + (part + 1 == parts) : 0};
            unsigned optional = 0, injected_drop = 0;
            unsigned begin = part * 4096 / parts, end = (part + 1) * 4096 / parts;
            for (unsigned ordinal = begin; ordinal < end; ordinal++) {
                unsigned j = ordinal - begin;
                int selected = vk_gpuprof_pipe_selected(ordinal, phase, 4, population);
                s->pipeline_observed++; s->pipeline_out_of_range += ordinal >= population;
                s->pipeline_selected += selected;
                if (loss && part && number % 107 == 0 && !selected && !injected_drop) {
                    s->pipeline_dropped++; injected_drop = 1; continue;
                }
                if (!selected && optional == 1024) { s->pipeline_unretained++; continue; }
                if (!selected) optional++;
                gp_pipeline *p = &slot->pipeline[slot->npipeline++];
                unsigned key = (ordinal + (unsigned)((number - 1) / 300 % 8) * 256 +
                    (unsigned)(number / 75 % 2) * 128) & 4095;
                /* Recurrent pool plus fresh variants exercises bounded lifetime
                   eviction, not just an immortal fixed 4096-entry dictionary. */
                unsigned identity = key % 64 ? key : 4096 + (unsigned)((number - 1) / 300) * 64 + key / 64;
                *p = (gp_pipeline){.frame = number, .key = {.pipeline = key + 1}, .ordinal = ordinal,
                    .population = population, .selected = selected, .draws = 1 + key % 7,
                    .primitives = 2 + key % 11, .unknown_draws = key % 19 == 0};
                p->key.shader[0] = names[identity][0]; p->key.shader[1] = names[identity][1];
                if (selected) {
                    p->query1 = slot->pipeline_timed++ * 2 + 1; p->timestamps = 2;
                    uint64_t duration = 10000 + key * 13;
                    slot->pipeline_ticks[p->query1 - 1] = (vk_gpuprof_timestamp){tick + j * 100, 1};
                    slot->pipeline_ticks[p->query1] = (vk_gpuprof_timestamp){tick + j * 100 + duration, 1};
                    if ((loss && number % 97 == 0) || number == unavailable_frame)
                        slot->pipeline_ticks[p->query1].available = 0;
                    if (loss && number % 101 == 0) slot->pipeline_ticks[p->query1].ticks = tick + j * 100 - 1;
                    if (loss && number % 103 == 0) {
                        p->timestamps = 1;
                        slot->pipeline_ticks[p->query1].available = 0; /* endpoint was never activated */
                    }
                    s->pipeline_timestamps += p->timestamps;
                }
            }
            if (loss && part && number == 109) atomic_fetch_add(&background_dropped_slots, 1);
            /* Synthetic GPU completion bypasses recording and vkQueueSubmit,
               not production retirement/readback. Account exactly the query
               instances supplied above before publishing the immutable lease. */
            atomic_fetch_add_explicit(&leases_acquired, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&leases_submitted, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&queries_submitted, slot_queries(slot), memory_order_relaxed);
            if (gpu_held) pending_gpu[gpu_at] = slot;
            else background_enqueue(&slot, 0);
        }
        if (nonblocking) {
            pthread_mutex_lock(&background_mu);
            if (background_close_tail - background_close_head == GP_CLOSURES)
                atomic_fetch_add(&background_dropped_closures, 1);
            else background_closures[background_close_tail++ % GP_CLOSURES] =
                (gp_closure){number, recording_sequence};
            pthread_cond_signal(&background_cv);
            pthread_mutex_unlock(&background_mu);
        }
        population = vk_gpuprof_pipe_population(population, 4096);
        logical += 1.0 / logical_fps;
        paced += 1.0 / logical_fps;
        if (pace) fixture_wait(wall_start, paced);
    }
    if (gpu_held) {
        unsigned count = attempted < gpu_held ? (unsigned)attempted : gpu_held;
        unsigned first = attempted < gpu_held ? 0 : attempted % gpu_held;
        for (unsigned i = 0; i < count; i++) background_enqueue(&pending_gpu[(first + i) % gpu_held], 0);
    }
    struct timespec close_start, close_end; clock_gettime(CLOCK_MONOTONIC, &close_start);
    vk_gpuprof_shutdown(); vk_gpuprof_shutdown();
    clock_gettime(CLOCK_MONOTONIC, &close_end);
    uint64_t windows = (frames + 299) / 300;
    printf("gpuprof-compact-fixture frames=%u logical_seconds=%.6f fps=%u pace=%u loss=%u raw=%d "
        "keys=4096 retained_optional_per_lease=1024 windows=%llu serialized_bytes=%llu bytes_per_window=%.3f "
        "worker_thread_cpu_ms=%.6f thread_cpu_ms_per_window=%.6f thread_cpu_peak_window_ms=%.6f "
        "backlog_peak=%llu aggregation_memory_bytes=%zu lease_memory_bytes=%zu parent_aggregation_memory_bound_bytes=%zu "
        "aggregation_allocations=0 output_write_calls=%llu close_flush_cpu_ms=%.6f close_wall_ms=%.6f "
        "producer_wait_ms=%.6f wall_ms=%.6f dropped_slots=%llu output_failures=%llu worker_scale=%u "
        "worker_scale_clock=thread_cpu "
        "queued_slots=%llu processed_slots=%llu parts=%u gpu_held=%u nonblocking=%u writer_stall_ms=%u "
        "coupled_stall=%u output_sink_peak_bytes=%llu output_sink_overflows=%llu fixture_identity_memory_bytes=%zu "
        "catch_up=%u producer_late_frames=%llu producer_late_slots=%llu producer_max_late_ms=%.6f "
        "producer_rebase_seconds=%.6f\n",
        frames, logical, fps, pace, loss, pipeline_raw, (unsigned long long)windows,
        (unsigned long long)output_serialized_bytes, output_serialized_bytes / (double)windows,
        worker_cpu_total_ms, worker_cpu_total_ms / windows, worker_window_cpu_peak_ms,
        (unsigned long long)background_backlog_peak, 2 * sizeof compact + sizeof compact_report_diagnostics,
        background_lease_bytes(),
        sizeof groups + sizeof group_occupied + GP_GROUPS * sizeof(gp_group),
        (unsigned long long)output_write_calls, compact_close_cpu_ms, fixture_elapsed_ms(close_start, close_end), producer_wait_ms,
        fixture_elapsed_ms(wall_start, close_end), (unsigned long long)atomic_load(&background_dropped_slots),
        (unsigned long long)atomic_load(&output_failures), fixture_worker_scale,
        (unsigned long long)atomic_load(&background_queued_slots),
        (unsigned long long)atomic_load(&background_processed_slots), parts, gpu_held, nonblocking,
        writer_stall_ms, coupled_stall, (unsigned long long)output_sink.peak,
        (unsigned long long)output_sink.overflows, identity_count * sizeof *names, catch_up,
        (unsigned long long)producer_late_frames, (unsigned long long)producer_late_slots,
        producer_max_late_ms, producer_rebase_seconds);
    free(names);
    return atomic_load(&output_failures) != 0 ||
        (nonblocking && !coupled_stall && !loss && atomic_load(&background_dropped_slots) != 0);
}

/* CPU-only recording microbenchmark. The production hooks and lease handoffs
   run unchanged; the timestamp sink counts calls but excludes Vulkan/driver
   command encoding. No worker, GPU, formatting, or fixture string generation
   is charged to the recording measurements. */
static uint64_t fixture_query_writes;
static void VKAPI_PTR fixture_write_timestamp(VkCommandBuffer cb, VkPipelineStageFlags2 stage,
        VkQueryPool pool, uint32_t query) {
    (void)cb; (void)stage; (void)pool; (void)query;
    fixture_query_writes++;
}
int vk_gpuprof_test_record_run(void) {
    const unsigned frames = 12000, groups_per_frame = 4096, draws_per_frame = 8192;
    vk_gpuprof_slot *slot = calloc(1, sizeof *slot);
    if (!slot) return 1;
    slot->pipeline = calloc(GP_RECORDS, sizeof *slot->pipeline);
    if (!slot->pipeline) { free(slot); return 1; }
    vk_shader shaders[2] = {0}; vk_shader *bound[6] = {&shaders[0], &shaders[1]};
    memset(shaders[0].key, 'a', 67); memset(shaders[1].key, 'b', 67);
    shaders[0].key[67] = shaders[1].key[67] = 0;
    vkCmdWriteTimestamp2 = fixture_write_timestamp;
    unsetenv("TOTK_VK_GPU_PROFILE"); vk_gpuprof_on = 1;
    vk_gpuprof_pipeline_on = 4; pipeline_population = 4096;
    VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)1;
    double hook_ms = 0, lease_ms = 0;
    for (unsigned f = 0; f < frames; f++) {
        frame = f + 1; pipeline_ordinal = 0; pipeline_phase = (17 + f * 4) & 4095;
        for (unsigned part = 0; part < 2; part++) {
            double start = thread_cpu_ms();
            background_free[background_nfree++] = slot;
            vk_gpuprof_slot *owner = NULL; vk_gpuprof_slot_begin(&owner, cb);
            lease_ms += thread_cpu_ms() - start;
            slot->n = 1; open_record = &slot->rec[0]; open_record->key.compute = 0;
            start = thread_cpu_ms();
            for (unsigned j = 0; j < groups_per_frame / 2; j++) {
                VkPipeline handle = (VkPipeline)(uintptr_t)(part * 2048 + j + 1);
                vk_gpuprof_pipeline_begin(cb, handle, bound);
                vk_gpuprof_pipeline_count(4, 3, 1, 0);
                vk_gpuprof_pipeline_begin(cb, handle, bound); /* unchanged-bind fast path */
                vk_gpuprof_pipeline_count(4, 3, 1, 0);
            }
            vk_gpuprof_pipeline_break(cb);
            hook_ms += thread_cpu_ms() - start;
            start = thread_cpu_ms();
            vk_gpuprof_slot_seal(slot); vk_gpuprof_slot_complete(&owner);
            slot = background_queue[background_head++ % GP_BACKLOG];
            lease_ms += thread_cpu_ms() - start;
        }
    }
    printf("gpuprof-record-fixture frames=%u groups_per_frame=%u draws_per_frame=%u "
        "pipeline_hook_cpu_ms_per_frame=%.9f lease_cpu_ms_per_frame=%.9f "
        "query_writes_per_frame=%.3f pipeline_query_writes_per_frame=8 driver_encoding_included=0\n",
        frames, groups_per_frame, draws_per_frame, hook_ms / frames, lease_ms / frames,
        fixture_query_writes / (double)frames);
    free(slot->pipeline); free(slot);
    return fixture_query_writes != (uint64_t)frames * 10;
}

int vk_gpuprof_test_sink_run(void) {
    gp_json escaped = {0}, overflow = {0};
    json_string(&escaped, "plain\"\\\n\001\xc3\xa9");
    int ok = !escaped.failed && !strcmp(escaped.data, "\"plain\\\"\\\\\\u000a\\u0001\xc3\xa9\"");
    static char oversized[8192];
    memset(oversized, 'a', sizeof oversized - 1);
    json_string(&overflow, oversized);
    ok &= overflow.failed;
    static char payload[GP_SINK_BYTES];
    for (size_t i = 0; i < sizeof payload; i++) payload[i] = (char)(i * 17u);
    output_file = tmpfile();
    if (!output_file) return 1;
    output_sink.paused = 1;
    gp_sink_start(output_file);
    ok &= output_write_locked(payload, sizeof payload);
    uint64_t before = monotonic_ns();
    int rejected = !output_write_locked("x", 1);
    double reject_ms = (monotonic_ns() - before) / 1e6;
    ok &= rejected && output_sink.overflows == 1 && atomic_load(&output_failures) == 1;
    pthread_mutex_lock(&output_sink.mu);
    output_sink.paused = 0; pthread_cond_broadcast(&output_sink.cv);
    pthread_mutex_unlock(&output_sink.mu);
    gp_sink_flush(1);
    ok &= output_write_locked(payload, sizeof payload - 100);
    gp_sink_flush(1);
    ok &= output_write_locked(payload, 200); /* ring wraps in the middle of this write */
    gp_sink_stop();
    rewind(output_file);
    char buffer[4096]; size_t offset = 0, n;
    while ((n = fread(buffer, 1, sizeof buffer, output_file))) {
        for (size_t i = 0; i < n; i++, offset++) {
            size_t p = offset < GP_SINK_BYTES ? offset :
                offset < 2 * GP_SINK_BYTES - 100 ? offset - GP_SINK_BYTES :
                offset - (2 * GP_SINK_BYTES - 100);
            ok &= buffer[i] == payload[p];
        }
    }
    ok &= offset == 2 * GP_SINK_BYTES + 100;
    fclose(output_file); output_file = NULL;
    printf("gpuprof-sink-contract failures=%d capacity_bytes=%u peak_bytes=%llu overflows=%llu "
        "output_failures=%llu rejected_write_wall_ms=%.6f bytes_checked=%zu\n", !ok,
        GP_SINK_BYTES, (unsigned long long)output_sink.peak, (unsigned long long)output_sink.overflows,
        (unsigned long long)atomic_load(&output_failures), reject_ms, offset);
    return !ok;
}
