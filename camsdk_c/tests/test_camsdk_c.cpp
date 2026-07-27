// test_camsdk_c.cpp - framework-free smoke test for the camsdk_c C ABI
// (context / device / sensor / frame / frameset / filter / pipeline).
// Runs entirely on the mock backend (ACAM_MOCK=1); no camera hardware needed.
#include <ainstec_cam/ainstec_cam.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            std::printf("PASS: %s\n", #cond);                             \
        } else {                                                          \
            std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
    _putenv("ACAM_MOCK=1"); // must happen before ac_context_create()

    CHECK(ac_last_error() != nullptr);

    // 1. Context, discovery, device info and open.
    ac_context_t* ctx = ac_context_create();
    CHECK(ctx != nullptr);
    ac_device_list_t* list = nullptr;
    CHECK(ac_context_query_devices(ctx, 1000, &list) == AC_OK);
    CHECK(list != nullptr);
    CHECK(ac_device_list_count(list) == 1);
    ac_device_t* dev = ac_device_list_get(list, 0);
    CHECK(dev != nullptr);
    char* info = nullptr;
    CHECK(ac_device_get_info_json(dev, &info) == AC_OK);
    CHECK(info != nullptr && std::strstr(info, "10.0.0.1") != nullptr);
    ac_string_free(info);
    CHECK(ac_device_is_open(dev) == 0);
    // Intrinsics require an open device.
    char* kjson = nullptr;
    CHECK(ac_device_get_intrinsics_json(dev, &kjson) != AC_OK);
    CHECK(kjson == nullptr);
    // The pipeline requires an already-open device.
    ac_pipeline_t* pipe0 = nullptr;
    CHECK(ac_pipeline_create(ctx, &pipe0) == AC_OK);
    CHECK(ac_pipeline_open(pipe0, dev) != AC_OK); // device not open yet
    ac_pipeline_destroy(pipe0);
    CHECK(ac_device_open(dev) == AC_OK);
    CHECK(ac_device_is_open(dev) == 1);
    // Mock intrinsics match its synthetic back-projection.
    CHECK(ac_device_get_intrinsics_json(dev, &kjson) == AC_OK);
    CHECK(kjson != nullptr && std::strstr(kjson, "\"width\":1280") != nullptr);
    CHECK(kjson != nullptr && std::strstr(kjson, "\"fx\":1000.0") != nullptr);
    CHECK(kjson != nullptr && std::strstr(kjson, "\"cx\":640.0") != nullptr);
    ac_string_free(kjson);

    // 2. Sensors: names, defaults, output mask roundtrip.
    CHECK(ac_device_get_sensor_count(dev) == 2);
    ac_sensor_t* s3d = ac_device_get_sensor(dev, 0);
    ac_sensor_t* srgb = ac_device_get_sensor(dev, 1);
    CHECK(s3d != nullptr && srgb != nullptr);
    CHECK(ac_sensor_get_name(s3d) != nullptr && std::strcmp(ac_sensor_get_name(s3d), "3D Module") == 0);
    CHECK(ac_sensor_get_name(srgb) != nullptr && std::strcmp(ac_sensor_get_name(srgb), "RGB Camera") == 0);
    uint32_t mask = 0;
    CHECK(ac_sensor_get_outputs(s3d, &mask) == AC_OK);
    CHECK(mask == (AC_OUTPUT_POINT3D | AC_OUTPUT_DEPTH | AC_OUTPUT_IR)); // mock default
    CHECK(ac_sensor_set_outputs(s3d, AC_OUTPUT_POINT3D | AC_OUTPUT_DEPTH | AC_OUTPUT_IR) == AC_OK);
    CHECK(ac_sensor_set_outputs(srgb, AC_OUTPUT_RGB) == AC_OK);
    mask = 0;
    CHECK(ac_sensor_get_outputs(srgb, &mask) == AC_OK && mask == AC_OUTPUT_RGB);

    // 3. Attribute tree and parameters.
    char* tree = nullptr;
    CHECK(ac_device_get_attribute_tree_json(dev, &tree) == AC_OK);
    CHECK(tree != nullptr && std::strstr(tree, "Exposure") != nullptr);
    ac_string_free(tree);
    CHECK(ac_device_set_value_int(dev, "Exposure", 25) == AC_OK);
    int32_t iv = 0;
    CHECK(ac_device_get_value_int(dev, "Exposure", &iv) == AC_OK);
    CHECK(iv == 25);
    iv = 0;
    CHECK(ac_device_get_value_int(dev, "Gain", &iv) == AC_OK);
    CHECK(iv == 3); // default, never set

    // 4. Pipeline: manual trigger -> one frameset with all four typed frames.
    ac_pipeline_t* pipe = nullptr;
    CHECK(ac_pipeline_create(ctx, &pipe) == AC_OK);
    CHECK(pipe != nullptr);
    CHECK(ac_pipeline_open(pipe, dev) == AC_OK);
    CHECK(ac_pipeline_start(pipe) == AC_OK);
    CHECK(ac_pipeline_trigger(pipe) == AC_OK);
    ac_frameset_t* set = nullptr;
    CHECK(ac_pipeline_wait_for_frames(pipe, 5000, &set) == AC_OK);
    CHECK(set != nullptr);
    CHECK(ac_frameset_count(set) == 4);
    ac_frame_t* fpts = ac_frameset_find(set, AC_FRAME_POINTS);
    ac_frame_t* fdep = ac_frameset_find(set, AC_FRAME_DEPTH);
    ac_frame_t* fir  = ac_frameset_find(set, AC_FRAME_IR);
    ac_frame_t* frgb = ac_frameset_find(set, AC_FRAME_RGB);
    CHECK(fpts != nullptr && fdep != nullptr && fir != nullptr && frgb != nullptr);
    CHECK(ac_frame_get_type(fpts) == AC_FRAME_POINTS);
    CHECK(ac_frame_get_type(frgb) == AC_FRAME_RGB);
    uint32_t w = 0, h = 0;
    CHECK(ac_frame_get_size(fdep, &w, &h) == AC_OK && w == 1280 && h == 1024);
    CHECK(ac_frame_get_size(frgb, &w, &h) == AC_OK && w == 640 && h == 480);
    const void* pdata = nullptr;
    size_t pbytes = 0;
    CHECK(ac_frame_get_data(fpts, &pdata, &pbytes) == AC_OK);
    CHECK(pbytes == (size_t)1280 * 1024 * 12);
    CHECK(pdata != nullptr);
    uint64_t index = 0;
    double ts = 0.0;
    CHECK(ac_frame_get_info(fdep, &index, &ts) == AC_OK);
    ac_frameset_release(set);

    // 5. A frame survives the frameset release when add_ref()'d.
    CHECK(ac_pipeline_trigger(pipe) == AC_OK);
    set = nullptr;
    CHECK(ac_pipeline_wait_for_frames(pipe, 5000, &set) == AC_OK);
    ac_frame_t* dep = ac_frameset_find(set, AC_FRAME_DEPTH);
    CHECK(dep != nullptr);
    ac_frame_add_ref(dep);
    ac_frameset_release(set);
    const void* ddata = nullptr;
    size_t dbytes = 0;
    CHECK(ac_frame_get_data(dep, &ddata, &dbytes) == AC_OK);
    CHECK(ddata != nullptr && dbytes == (size_t)1280 * 1024 * 4);
    ac_frame_release(dep);

    // 6. Filter: decimation by 8, unknown filter, passthrough contract.
    ac_filter_t* filt = nullptr;
    CHECK(ac_filter_create("decimation", &filt) == AC_OK);
    CHECK(filt != nullptr);
    ac_filter_t* bad = nullptr;
    CHECK(ac_filter_create("no_such_filter", &bad) != AC_OK);
    CHECK(bad == nullptr);
    CHECK(ac_filter_set_config(filt, "factor", "8") == AC_OK);
    CHECK(ac_pipeline_trigger(pipe) == AC_OK);
    set = nullptr;
    CHECK(ac_pipeline_wait_for_frames(pipe, 5000, &set) == AC_OK);
    ac_frameset_t* dset = nullptr;
    CHECK(ac_filter_process(filt, set, &dset) == AC_OK);
    CHECK(dset != nullptr);
    ac_frame_t* dpts = ac_frameset_find(dset, AC_FRAME_POINTS);
    CHECK(dpts != nullptr);
    const void* dpd = nullptr;
    size_t dpb = 0;
    CHECK(ac_frame_get_data(dpts, &dpd, &dpb) == AC_OK);
    CHECK(dpb == (size_t)(1280 * 1024 / 8) * 12);
    ac_frame_t* ddep = ac_frameset_find(dset, AC_FRAME_DEPTH);
    CHECK(ddep != nullptr && ac_frame_get_size(ddep, &w, &h) == AC_OK && w == 160 && h == 128);
    CHECK(ac_frameset_find(dset, AC_FRAME_RGB) != nullptr); // still present
    ac_frameset_release(set);
    ac_frameset_release(dset);
    ac_filter_destroy(filt);

    // 6b. DEPTH-only negative: no points frame in the set.
    CHECK(ac_sensor_set_outputs(s3d, AC_OUTPUT_DEPTH) == AC_OK);
    CHECK(ac_sensor_set_outputs(srgb, 0) == AC_OK);
    CHECK(ac_pipeline_trigger(pipe) == AC_OK);
    set = nullptr;
    CHECK(ac_pipeline_wait_for_frames(pipe, 5000, &set) == AC_OK);
    CHECK(ac_frameset_find(set, AC_FRAME_DEPTH) != nullptr);
    CHECK(ac_frameset_find(set, AC_FRAME_POINTS) == nullptr);
    CHECK(ac_frameset_count(set) == 1);
    ac_frameset_release(set);
    // Restore full masks for the auto-trigger phase.
    CHECK(ac_sensor_set_outputs(s3d, AC_OUTPUT_POINT3D | AC_OUTPUT_DEPTH | AC_OUTPUT_IR) == AC_OK);
    CHECK(ac_sensor_set_outputs(srgb, AC_OUTPUT_RGB) == AC_OK);

    // 7. Auto trigger at 5 Hz: two consecutive sets with increasing indices.
    CHECK(ac_pipeline_set_auto_trigger(pipe, 5.0) == AC_OK);
    ac_frameset_t *s1 = nullptr, *s2 = nullptr;
    CHECK(ac_pipeline_wait_for_frames(pipe, 5000, &s1) == AC_OK);
    CHECK(ac_pipeline_wait_for_frames(pipe, 5000, &s2) == AC_OK);
    uint64_t i1 = 0, i2 = 0;
    double t1 = 0.0, t2 = 0.0;
    CHECK(ac_frame_get_info(ac_frameset_get(s1, 0), &i1, &t1) == AC_OK);
    CHECK(ac_frame_get_info(ac_frameset_get(s2, 0), &i2, &t2) == AC_OK);
    CHECK(i2 > i1);
    ac_frameset_release(s1);
    ac_frameset_release(s2);
    CHECK(ac_pipeline_set_auto_trigger(pipe, 0.0) == AC_OK);

    // 8. Negative: no pending trigger -> timeout. Drain leftovers from the
    // auto-trigger phase first; a full 1s quiet wait proves the queue empty
    // and the worker idle.
    {
        ac_frameset_t* s = nullptr;
        while (ac_pipeline_wait_for_frames(pipe, 1000, &s) == AC_OK) {
            ac_frameset_release(s);
            s = nullptr;
        }
        CHECK(ac_pipeline_wait_for_frames(pipe, 200, &s) != AC_OK);
        CHECK(s == nullptr);
        CHECK(ac_last_error() != nullptr && std::strstr(ac_last_error(), "timeout") != nullptr);
    }

    // 9. Teardown + NULL-safety spot checks.
    CHECK(ac_pipeline_stop(pipe) == AC_OK);
    CHECK(ac_pipeline_stop(pipe) == AC_OK); // idempotent
    ac_pipeline_destroy(pipe);
    ac_pipeline_destroy(nullptr);
    CHECK(ac_device_close(dev) == AC_OK);
    CHECK(ac_device_is_open(dev) == 0);
    ac_device_release(dev);
    ac_device_release(nullptr);
    ac_device_list_release(list);
    ac_device_list_release(nullptr);
    ac_context_destroy(ctx);
    ac_context_destroy(nullptr);
    ac_frame_release(nullptr);
    ac_frameset_release(nullptr);
    ac_filter_destroy(nullptr);
    ac_string_free(nullptr);
    CHECK(ac_last_error() != nullptr);

    if (g_failures != 0) {
        std::printf("FAILED: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ALL CHECKS PASSED\n");
    return 0;
}
