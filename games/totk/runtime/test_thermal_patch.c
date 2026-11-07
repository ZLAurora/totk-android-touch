/* Reuse the dataless patch fixture and its runtime stand-ins. */
#define TOTK_THERMAL_PATCH_TEST 1
#define main fps60_fixture_main
#include "test_v143_fps60_patch_table.c"
#undef main
int main(void) {
    image = calloc(1, IMAGE_SIZE);
    if (!image) return 2;
    module = (Module){.base = image, .text_end = TEXT_END, .file_size = FILE_SIZE, .size = IMAGE_SIZE};
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++)
        put_word(image, fps60[i].off, fps60[i].replacement);
    pristine = malloc(IMAGE_SIZE);
    if (!pristine) return 2;
    memcpy(pristine, image, IMAGE_SIZE);
    const char *off[] = {NULL, "0", "", "invalid", "01"};
    for (unsigned i = 0; i < sizeof off / sizeof *off; i++) {
        set_env("TOTK_THERMAL_STEP", off[i]);
        CHECK(run(patch_thermal_step), "disabled thermal gate refused");
        CHECK(!memcmp(image, pristine, IMAGE_SIZE),
              "off/unset thermal gate changed controller, veneer or data bytes");
    }
    set_env("TOTK_THERMAL_STEP", "1");
    const uint64_t thermal_stub = TEXT_END + 916;
    /* Refuse mismatches and occupied code/data BEFORE writing any part. */
    const uint64_t bad[] = {fps60[1].off, fps60[6].off, fps60[4].off,
        thermal_stub, thermal_stub + 107, DATA + 144};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        reset_image(); image[bad[i]] ^= 0x40;
        CHECK(!run(patch_thermal_step), "accepted mismatch at %#llx", (unsigned long long)bad[i]);
        image[bad[i]] ^= 0x40;
        CHECK(!memcmp(image, pristine, IMAGE_SIZE), "refusal changed image");
    }
    reset_image();
    CHECK(run(patch_thermal_step), "thermal install failed");
    CHECK(word_at(DATA + 144) == 1, "default floor must be one");
    const unsigned indices[] = {1, 6, 4};
    printf("{\"stub\":%u,\"floor\":%u,\"entries\":[", (unsigned)thermal_stub, DATA + 144);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(branch_target(fps60[indices[i]].off, word_at(fps60[indices[i]].off)) == thermal_stub + 36 * i,
            "select branches to wrong veneer");
        printf("%s[%u,%u]", i ? "," : "", (unsigned)fps60[indices[i]].off, fps60[indices[i]].replacement);
        put_word(image, fps60[indices[i]].off, fps60[indices[i]].replacement);
    }
    printf("],\"code\":\"");
    for (unsigned i = 0; i < 108; i++) printf("%02x", image[thermal_stub + i]);
    puts("\"}");
    memset(image + thermal_stub, 0, 108); put_word(image, DATA + 144, 0);
    CHECK(!memcmp(image, pristine, IMAGE_SIZE), "bytes outside owned regions changed");
    free(pristine); free(image);
    return failures ? 1 : 0;
}
