// test_acam_record_playback.cpp - .acam recorder/playback smoke (ACAM_MOCK=1).
#include <ainstec_cam/ainstec_cam.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            std::printf("PASS: %s\n", #cond);                             \
            std::fflush(stdout);                                          \
        } else {                                                          \
            std::printf("FAIL: %s (%s:%d) err=%s\n", #cond, __FILE__,     \
                        __LINE__, ac_last_error());                       \
            std::fflush(stdout);                                          \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
#ifdef _WIN32
    _putenv("ACAM_MOCK=1");
#else
    setenv("ACAM_MOCK", "1", 1);
#endif

    ac_context_t* ctx = ac_context_create();
    CHECK(ctx != nullptr);
    ac_device_list_t* list = nullptr;
    CHECK(ac_context_query_devices(ctx, 1000, &list) == AC_OK);
    ac_device_t* dev = ac_device_list_get(list, 0);
    CHECK(dev != nullptr);
    CHECK(ac_device_open(dev) == AC_OK);
    CHECK(ac_device_set_value_int(dev, "Exposure", 25) == AC_OK);

    ac_sensor_t* s3d = ac_device_get_sensor(dev, 0);
    ac_sensor_t* srgb = ac_device_get_sensor(dev, 1);
    CHECK(s3d != nullptr && srgb != nullptr);
    // IR-only keeps the recording small.
    CHECK(ac_sensor_set_outputs(s3d, AC_OUTPUT_IR) == AC_OK);
    CHECK(ac_sensor_set_outputs(srgb, 0) == AC_OK);

    const char* path = "acam_record_playback_test.acam";
    CHECK(ac_device_record_to_file(dev, path) == AC_OK);

    ac_pipeline_t* pipe = nullptr;
    CHECK(ac_pipeline_create(ctx, &pipe) == AC_OK);
    CHECK(ac_pipeline_open(pipe, dev) == AC_OK);
    CHECK(ac_pipeline_start(pipe) == AC_OK);

    uint8_t samples[2] = {0, 0};
    for (int n = 0; n < 2; ++n) {
        CHECK(ac_pipeline_trigger(pipe) == AC_OK);
        ac_frameset_t* set = nullptr;
        CHECK(ac_pipeline_wait_for_frames(pipe, 5000, &set) == AC_OK);
        ac_frame_t* fir = ac_frameset_find(set, AC_FRAME_IR);
        CHECK(fir != nullptr);
        const void* data = nullptr;
        size_t nbytes = 0;
        CHECK(ac_frame_get_data(fir, &data, &nbytes) == AC_OK);
        CHECK(nbytes == (size_t)640 * 512);
        samples[n] = static_cast<const uint8_t*>(data)[100];
        ac_frameset_release(set);
    }
    CHECK(ac_device_stop_record(dev) == AC_OK);
    CHECK(ac_device_stop_record(dev) == AC_OK); // idempotent
    CHECK(ac_pipeline_stop(pipe) == AC_OK);
    ac_pipeline_destroy(pipe);
    CHECK(ac_device_close(dev) == AC_OK);
    ac_device_release(dev);

    ac_device_t* play = nullptr;
    CHECK(ac_context_load_device(ctx, path, &play) == AC_OK);
    CHECK(play != nullptr);
    CHECK(ac_device_open(play) == AC_OK);
    CHECK(ac_device_set_value_int(play, "Exposure", 1) != AC_OK);
    int32_t pev = 0;
    CHECK(ac_device_get_value_int(play, "Exposure", &pev) == AC_OK);
    CHECK(pev == 25);
    char* kjson = nullptr;
    CHECK(ac_device_get_intrinsics_json(play, &kjson) == AC_OK);
    CHECK(kjson != nullptr && std::strstr(kjson, "\"fx\":500") != nullptr);
    ac_string_free(kjson);
    char* cjson = nullptr;
    CHECK(ac_device_get_calibration_json(play, &cjson) == AC_OK);
    CHECK(cjson != nullptr && std::strstr(cjson, "\"fx\":520.0") != nullptr); // recorded RGB intrinsics
    CHECK(cjson != nullptr && std::strstr(cjson, "\"points_to_rgb\":[1,0,0,25") != nullptr);
    ac_string_free(cjson);

    ac_sensor_t* ps3d = ac_device_get_sensor(play, 0);
    ac_sensor_t* psrgb = ac_device_get_sensor(play, 1);
    CHECK(ac_sensor_set_outputs(ps3d, AC_OUTPUT_IR) == AC_OK);
    CHECK(ac_sensor_set_outputs(psrgb, 0) == AC_OK);

    ac_pipeline_t* ppipe = nullptr;
    CHECK(ac_pipeline_create(ctx, &ppipe) == AC_OK);
    CHECK(ac_pipeline_open(ppipe, play) == AC_OK);
    CHECK(ac_pipeline_start(ppipe) == AC_OK);
    for (int n = 0; n < 2; ++n) {
        CHECK(ac_pipeline_trigger(ppipe) == AC_OK);
        ac_frameset_t* set = nullptr;
        CHECK(ac_pipeline_wait_for_frames(ppipe, 5000, &set) == AC_OK);
        ac_frame_t* fir = ac_frameset_find(set, AC_FRAME_IR);
        CHECK(fir != nullptr);
        const void* data = nullptr;
        size_t nbytes = 0;
        CHECK(ac_frame_get_data(fir, &data, &nbytes) == AC_OK);
        CHECK(static_cast<const uint8_t*>(data)[100] == samples[n]);
        ac_frameset_release(set);
    }
    CHECK(ac_pipeline_trigger(ppipe) == AC_OK);
    ac_frameset_t* set = nullptr;
    CHECK(ac_pipeline_wait_for_frames(ppipe, 500, &set) != AC_OK);
    CHECK(set == nullptr);

    CHECK(ac_pipeline_stop(ppipe) == AC_OK);
    ac_pipeline_destroy(ppipe);
    CHECK(ac_device_close(play) == AC_OK);
    ac_device_release(play);
    ac_device_list_release(list);
    ac_context_destroy(ctx);
    std::remove(path);

    if (g_failures != 0) {
        std::printf("FAILED: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ALL CHECKS PASSED\n");
    return 0;
}
