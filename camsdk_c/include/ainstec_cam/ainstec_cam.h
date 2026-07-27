/* ainstec_cam.h - C ABI for Ainstec 3D cameras (layer 3 of libainstecmono).
 *
 * Object model in the style of librealsense/Orbbec:
 *   context      backend selection (ACAM_MOCK env) + device discovery
 *   device list  snapshot of discovered devices (reference-counted devices)
 *   device       open/close + string-keyed parameters
 *   sensor       capture unit of a device with its own output mask (borrowed)
 *   frame        one typed data plane (reference-counted)
 *   frameset     the typed frames produced by one capture
 *   filter       frameset -> frameset processing block, created by name
 *   pipeline     trigger + worker thread + bounded frameset queue
 *
 * Pure C99. Unless noted, functions return AC_OK (0) on success and a nonzero
 * error code on failure; after a failure ac_last_error() gives a thread-local
 * message. Out-strings are library-malloc'd and freed with ac_string_free().
 */
#ifndef AINSTEC_CAM_H
#define AINSTEC_CAM_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(ACAM_BUILDING_DLL)
#    define ACAM_API __declspec(dllexport)
#  else
#    define ACAM_API __declspec(dllimport)
#  endif
#else
#  define ACAM_API
#endif

/* AC_OK is also defined by the vendor SDK headers (GenericError.h); the
 * ifndef guard lets both coexist when the vendor header is included first. */
#ifndef AC_OK
#define AC_OK 0
#endif

/* Output plane selection bits for ac_sensor_set_outputs(). */
#define AC_OUTPUT_POINT3D (1u << 0) /* float xyz point cloud (3D sensor) */
#define AC_OUTPUT_DEPTH   (1u << 1) /* float depth map in mm (3D sensor) */
#define AC_OUTPUT_IR      (1u << 2) /* 8-bit mono IR image (3D sensor)   */
#define AC_OUTPUT_RGB     (1u << 3) /* packed 24-bit RGB (RGB sensor)    */
#define AC_OUTPUT_NORMALS (1u << 4) /* float normals (3D sensor)         */

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles. */
typedef struct ac_context ac_context_t;
typedef struct ac_device_list ac_device_list_t;
typedef struct ac_device ac_device_t;
typedef struct ac_sensor ac_sensor_t;
typedef struct ac_frame ac_frame_t;
typedef struct ac_frameset ac_frameset_t;
typedef struct ac_filter ac_filter_t;
typedef struct ac_pipeline ac_pipeline_t;

/* Typed frame kinds produced by a capture. */
typedef enum {
    AC_FRAME_DEPTH = 1, /* float32, millimeters, w*h      */
    AC_FRAME_IR,        /* uint8 mono, w*h                */
    AC_FRAME_RGB,       /* uint8 packed RGB, w*h*3        */
    AC_FRAME_POINTS,    /* float32 xyz interleaved, N*12  */
    AC_FRAME_NORMALS    /* float32 nxnynz interleaved     */
} ac_frame_type;

/* ---- context: backend selection + discovery ---- */

/* Create a context. The ACAM_MOCK environment variable is read ONCE here:
 * "1" selects the mock backend, anything else the real vendor SDK backend.
 * Returns NULL on failure (see ac_last_error()). */
ACAM_API ac_context_t* ac_context_create(void);
/* Destroy a context. NULL-safe. */
ACAM_API void ac_context_destroy(ac_context_t* ctx);
/* Discover devices via UDP broadcast, waiting up to timeout_ms. */
ACAM_API int ac_context_query_devices(ac_context_t* ctx, uint32_t timeout_ms, ac_device_list_t** out);

/* ---- device list / device ---- */

/* Number of devices in the list (0 for NULL). */
ACAM_API size_t ac_device_list_count(const ac_device_list_t* list);
/* Get a device by index; returns a NEW reference the caller must free with
 * ac_device_release(). NULL when index is out of range. */
ACAM_API ac_device_t* ac_device_list_get(const ac_device_list_t* list, size_t index);
/* Release the list; device references obtained from it stay valid. NULL-safe. */
ACAM_API void ac_device_list_release(ac_device_list_t* list);
/* Release one device reference. NULL-safe. */
ACAM_API void ac_device_release(ac_device_t* device);
/* Identity as a malloc'ed JSON object {"name","model","ip","serial","mac"};
 * works without opening the device. Free with ac_string_free(). */
ACAM_API int ac_device_get_info_json(const ac_device_t* device, char** out_json);
/* Open/close the camera. Both are idempotent. */
ACAM_API int ac_device_open(ac_device_t* device);
ACAM_API int ac_device_close(ac_device_t* device);
/* Returns 1 when open, 0 when closed or NULL. */
ACAM_API int ac_device_is_open(const ac_device_t* device);
/* String-keyed camera parameters; enumerate keys, types and ranges with
 * ac_device_get_attribute_tree_json() (*out_json freed via ac_string_free()). */
ACAM_API int ac_device_get_attribute_tree_json(ac_device_t* device, char** out_json);
ACAM_API int ac_device_set_value_int(ac_device_t* device, const char* key, int32_t value);
ACAM_API int ac_device_set_value_float(ac_device_t* device, const char* key, float value);
ACAM_API int ac_device_set_value_bool(ac_device_t* device, const char* key, int value);
ACAM_API int ac_device_get_value_int(ac_device_t* device, const char* key, int32_t* out);
ACAM_API int ac_device_get_value_float(ac_device_t* device, const char* key, float* out);
ACAM_API int ac_device_get_value_bool(ac_device_t* device, const char* key, int* out);
/* Pinhole intrinsics of the 3D camera as a malloc'ed JSON object
 * {"width":W,"height":H,"fx":..,"fy":..,"cx":..,"cy":..} (free with
 * ac_string_free()). W/H are the depth-map dimensions. Requires the device
 * to be OPEN (error otherwise). */
ACAM_API int ac_device_get_intrinsics_json(ac_device_t* device, char** out_json);

/* ---- sensors (borrowed from their device) ---- */

/* Number of sensors (0 for NULL). */
ACAM_API int ac_device_get_sensor_count(const ac_device_t* device);
/* Borrowed sensor handle: valid while the device handle it came from is
 * alive; must NOT be released. NULL when index is out of range. */
ACAM_API ac_sensor_t* ac_device_get_sensor(const ac_device_t* device, size_t index);
/* Static sensor name ("3D Module" or "RGB Camera"); NULL for NULL input. */
ACAM_API const char* ac_sensor_get_name(const ac_sensor_t* sensor);
/* Select the output planes this sensor contributes to a capture
 * (AC_OUTPUT_* bitmask). */
ACAM_API int ac_sensor_set_outputs(ac_sensor_t* sensor, uint32_t mask);
ACAM_API int ac_sensor_get_outputs(const ac_sensor_t* sensor, uint32_t* mask_out);

/* ---- frames (typed, reference-counted) ---- */

/* Frame kind; (ac_frame_type)0 for NULL. */
ACAM_API ac_frame_type ac_frame_get_type(const ac_frame_t* frame);
/* index: per-device capture counter; timestamp_s: seconds. */
ACAM_API int ac_frame_get_info(const ac_frame_t* frame, uint64_t* index, double* timestamp_s);
/* Dimensions: DEPTH/IR/RGB pixels; POINTS: w = point count, h = 1. */
ACAM_API int ac_frame_get_size(const ac_frame_t* frame, uint32_t* w, uint32_t* h);
/* Zero-copy payload, valid while the frame is alive. Sizes: DEPTH w*h*4,
 * IR w*h, RGB w*h*3, POINTS N*12, NORMALS w*h*12 bytes. */
ACAM_API int ac_frame_get_data(const ac_frame_t* frame, const void** data, size_t* byte_size);
/* Frames start with one reference. add_ref/release are thread-safe; the
 * payload stays valid until the last release. */
ACAM_API void ac_frame_add_ref(ac_frame_t* frame);
ACAM_API void ac_frame_release(ac_frame_t* frame); /* NULL-safe */

/* ---- framesets (all frames of one capture) ---- */

/* Number of frames in the set (0 for NULL). */
ACAM_API size_t ac_frameset_count(const ac_frameset_t* frameset);
/* Borrowed frame, valid until ac_frameset_release(); call ac_frame_add_ref()
 * to keep it longer. NULL when index is out of range. */
ACAM_API ac_frame_t* ac_frameset_get(const ac_frameset_t* frameset, size_t index);
/* Borrowed frame of the given type, or NULL when absent. */
ACAM_API ac_frame_t* ac_frameset_find(const ac_frameset_t* frameset, ac_frame_type type);
/* Release the set and its frame references. NULL-safe. Frames that were
 * add_ref()'d survive the set release. */
ACAM_API void ac_frameset_release(ac_frameset_t* frameset);

/* ---- processing framework: named filters, frameset -> frameset ---- */

/* Create a filter by name. v1 names: "decimation". Unknown name -> error. */
ACAM_API int ac_filter_create(const char* name, ac_filter_t** out);
/* Configure a filter with string key/value pairs. "decimation" accepts key
 * "factor" with an integer >= 1 (default 1 = passthrough). */
ACAM_API int ac_filter_set_config(ac_filter_t* filter, const char* key, const char* value);
/* Process a frameset into a new frameset. Frames the filter does not handle
 * pass through (add_ref'd) into the output set. "decimation": POINTS keeps
 * every factor-th point (count/factor); DEPTH/IR/RGB are stride-subsampled
 * to (w/factor, h/factor). */
ACAM_API int ac_filter_process(ac_filter_t* filter, const ac_frameset_t* in, ac_frameset_t** out);
/* Destroy a filter. NULL-safe. */
ACAM_API void ac_filter_destroy(ac_filter_t* filter);

/* ---- pipeline: trigger + worker thread + bounded queue ---- */

/* Create a pipeline bound to a context (context must outlive the pipeline). */
ACAM_API int ac_pipeline_create(ac_context_t* ctx, ac_pipeline_t** out);
/* Bind an already-open device (ac_device_open() must have succeeded, else
 * error). The device lifecycle stays with the caller: the pipeline never
 * closes it. */
ACAM_API int ac_pipeline_open(ac_pipeline_t* pipeline, ac_device_t* device);
/* Spawn the worker thread. All device captures happen on this single thread. */
ACAM_API int ac_pipeline_start(ac_pipeline_t* pipeline);
/* Software trigger: enqueue one capture signal; returns immediately. Each
 * signal produces exactly one capture on the worker. */
ACAM_API int ac_pipeline_trigger(ac_pipeline_t* pipeline);
/* hz > 0: the worker self-triggers at hz. hz == 0.0: manual triggering only
 * (default). */
ACAM_API int ac_pipeline_set_auto_trigger(ac_pipeline_t* pipeline, double hz);
/* Pop the next frameset from the bounded queue (capacity 2, drop-oldest).
 * On timeout returns a distinct error code and ac_last_error() is "timeout". */
ACAM_API int ac_pipeline_wait_for_frames(ac_pipeline_t* pipeline, uint32_t timeout_ms, ac_frameset_t** out);
/* Stop the worker thread and join it. Idempotent. Queued framesets stay
 * available until destroy. */
ACAM_API int ac_pipeline_stop(ac_pipeline_t* pipeline);
/* Destroy the pipeline (stops first if needed) and drop queued framesets.
 * NULL-safe. */
ACAM_API void ac_pipeline_destroy(ac_pipeline_t* pipeline);

/* ---- misc ---- */

/* Free strings returned by this library (same as free()). NULL-safe. */
ACAM_API void ac_string_free(char* s);
/* Thread-local message of the most recent failed call; never NULL. */
ACAM_API const char* ac_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AINSTEC_CAM_H */
