// SPDX-License-Identifier: 0BSD
// Copyright (c) 2026 angellfear
/*
 * Permissive, and it has to stay that way. This file is built into
 * libchiron-ov13b10.so, which links Amlogic's proprietary libispaml and
 * libmediaAPI -- build-camera.sh asserts that it does. A GPL-2.0 object
 * linked against proprietary libraries cannot be redistributed by anyone
 * downstream, so relicensing this file GPL-2.0 to match drivers/ would hand
 * every redistributor of the image a violation they did not create.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "sensor_config.h"
#include "staticPipe.h"
#include "v4l2subdev.h"
#include "aml_isp_adapt.h"

int aml_isp_set_wb_attr(int pipe, isp_wb_attr_s *attr);
int aml_isp_querywbinfo(int pipe, aml_isp_wb_info_attr *info);

#define OV13B10_WIDTH 4208
#define OV13B10_HEIGHT 3120
#define OV13B10_TOTAL_WIDTH 1176
#define OV13B10_TOTAL_HEIGHT 3264
#define OV13B10_LINES_PER_SECOND 97680
#define OV13B10_AGAIN_LOG2_MAX 16179U
#define OV13B10_FPS_CONTROL 0x00981900U
#define OV13B10_SBGGR10_CODE 0x3007U
#define OV13B10_WB_RED 400U
#define OV13B10_WB_BLUE 438U
#define OV13B10_CALIB_DEFAULT "/usr/lib/chiron-camera/ov13b10-sdr.calib"

struct calibration_record {
    uint32_t index;
    uint16_t rows;
    uint16_t cols;
    uint16_t width;
    uint16_t reserved;
    uint32_t size;
};

struct ov13b10_runtime {
    pthread_mutex_t lock;
    struct media_entity *sensor;
    ALG_SENSOR_DEFAULT_S defaults;
    uint32_t gain;
    uint32_t exposure;
    uint32_t short_exposure;
    unsigned int gain_pending;
    unsigned int exposure_pending;
};

static struct ov13b10_runtime runtime = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static pthread_once_t calibration_once = PTHREAD_ONCE_INIT;
static pthread_once_t wb_probe_once = PTHREAD_ONCE_INIT;
static LookupTable calibration_tables[CALIBRATION_TOTAL_SIZE];
static uint8_t calibration_present[CALIBRATION_TOTAL_SIZE];
static uint8_t *calibration_blob;
static int calibration_status = -1;

static void *wb_probe_main(void *unused)
{
    isp_wb_attr_s attr = {0};
    aml_isp_wb_info_attr info = {0};
    const char *manual = getenv("CHIRON_OV13B10_MANUAL_WB");
    unsigned int red = OV13B10_WB_RED;
    unsigned int blue = OV13B10_WB_BLUE;
    char tail;

    (void)unused;
    if (manual && sscanf(manual, "%u:%u%c", &red, &blue, &tail) != 2) {
        fprintf(stderr, "chiron-camera: invalid manual WB override: %s\n",
                manual);
        return NULL;
    }
    sleep(3);
    attr.bByPass = mbp_true;
    attr.stManual.u32ManualRGain = red;
    attr.stManual.u32ManualBGain = blue;
    attr.stManual.u32ManualTemperature = 5000;
    aml_isp_set_wb_attr(0, &attr);
    sleep(1);
    aml_isp_querywbinfo(0, &info);
    fprintf(stderr,
            "chiron-camera: fixed WB r=%u b=%u applied=(%u,%u)\n",
            red, blue, info.u16Rgain, info.u16Bgain);
    return NULL;
}

static void start_wb_probe(void)
{
    pthread_t thread;
    int error = pthread_create(&thread, NULL, wb_probe_main, NULL);

    if (error) {
        fprintf(stderr, "chiron-camera: cannot start WB probe: %s\n",
                strerror(error));
        return;
    }
    pthread_detach(thread);
}

/*
 * The factory Chiron HAL uses the 2023 Amlogic calibration enum (148 core
 * slots) plus VERSION, AWB_REF_REMOVE_LUT and HIGHLIGHT_DETECT. Khadas 0.8
 * inserted AE_WEIGHT_T and three lens-shading slots in the core enum, then
 * added CSC and two AWB weight tables at the tail. Loading old slot numbers
 * verbatim is unsafe: old FPNR slot 130 becomes AWB_CT_BG_CURVE and causes
 * libispaml to emit malformed parameter blocks. Keep the IQ payload exact and
 * translate only its enum ABI.  Chiron's Android branch omits the historical
 * DYM_CUSTOM_SETTING slot, so its tail starts with AWB_PRESET at slot 147.
 */
static int remap_calibration_index(uint32_t old_index)
{
    if (old_index <= 13)
        return (int)old_index;
    if (old_index <= 55)
        return (int)old_index + 1;
    if (old_index == 56)
        return 58;
    if (old_index <= 101)
        return (int)old_index + 3;
    if (old_index <= 147)
        return (int)old_index + 4;
    if (old_index == 148)
        return CALIBRATION_VERSION;
    if (old_index == 149)
        return CALIBRATION_AWB_REF_REMOVE_LUT;
    if (old_index == 150)
        return CALIBRATION_HIGHLIGHT_DETECT;
    return -1;
}

static int read_complete(int fd, void *buffer, size_t size)
{
    uint8_t *cursor = buffer;

    while (size) {
        ssize_t got = read(fd, cursor, size);

        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (got == 0)
            return -1;
        cursor += got;
        size -= (size_t)got;
    }
    return 0;
}

static void load_calibration(void)
{
    static const uint8_t magic[8] = {'O', 'V', '1', '3', 'C', 'A', 'L', 1};
    static const uint8_t ov08_test_magic[8] =
        {'O', 'V', '0', '8', 'C', 'A', 'L', 1};
    const char *path = getenv("CHIRON_OV13B10_CALIB");
    struct stat st;
    uint32_t slots;
    int keep_2023_abi;
    size_t offset;
    int fd;

    if (!path || !*path)
        path = OV13B10_CALIB_DEFAULT;
    keep_2023_abi = getenv("CHIRON_OV13B10_KEEP_2023_ABI") != NULL;
    fprintf(stderr, "chiron-camera: calibration ABI %s\n",
            keep_2023_abi ? "2023" : "Khadas 0.8");
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 12 ||
        st.st_size > 4 * 1024 * 1024) {
        fprintf(stderr, "chiron-camera: cannot open calibration %s: %s\n",
                path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }

    calibration_blob = malloc((size_t)st.st_size);
    if (!calibration_blob || read_complete(fd, calibration_blob,
                                            (size_t)st.st_size) < 0) {
        fprintf(stderr, "chiron-camera: cannot read calibration %s\n", path);
        close(fd);
        return;
    }
    close(fd);

    if (memcmp(calibration_blob, magic, sizeof(magic)) != 0 &&
        memcmp(calibration_blob, ov08_test_magic,
               sizeof(ov08_test_magic)) != 0) {
        fprintf(stderr, "chiron-camera: invalid calibration magic\n");
        return;
    }
    memcpy(&slots, calibration_blob + 8, sizeof(slots));
    if (slots < CALIBRATION_TOTAL_SIZE) {
        fprintf(stderr, "chiron-camera: calibration has only %u slots\n", slots);
        return;
    }

    offset = 12;
    while (offset < (size_t)st.st_size) {
        struct calibration_record record;
        uint64_t expected;
        int index;

        if ((size_t)st.st_size - offset < sizeof(record)) {
            fprintf(stderr, "chiron-camera: truncated calibration record\n");
            return;
        }
        memcpy(&record, calibration_blob + offset, sizeof(record));
        offset += sizeof(record);
        expected = (uint64_t)record.rows * record.cols * record.width;
        index = keep_2023_abi ? (int)record.index :
                                remap_calibration_index(record.index);
        if (index < 0 || index >= CALIBRATION_TOTAL_SIZE || !record.rows ||
            !record.cols || !record.width || expected != record.size ||
            record.size > (size_t)st.st_size - offset ||
            calibration_present[index]) {
            fprintf(stderr, "chiron-camera: invalid calibration record %u\n",
                    record.index);
            return;
        }
        calibration_tables[index].ptr = calibration_blob + offset;
        calibration_tables[index].rows = record.rows;
        calibration_tables[index].cols = record.cols;
        calibration_tables[index].width = record.width;
        calibration_present[index] = 1;
        offset += record.size;
    }
    calibration_status = 0;
}

static void reset_defaults(ALG_SENSOR_DEFAULT_S *defaults)
{
    memset(defaults, 0, sizeof(*defaults));
    defaults->fps = 30U * 256U;
    defaults->pixels_per_line = OV13B10_TOTAL_WIDTH;
    defaults->again_log2_max = OV13B10_AGAIN_LOG2_MAX;
    defaults->again_high_log2_max = OV13B10_AGAIN_LOG2_MAX;
    defaults->again_accuracy_fmt = 1;
    defaults->again_accuracy = 204;
    defaults->again_high_accuracy_fmt = 1;
    defaults->again_high_accuracy = 204;
    defaults->dgain_accuracy = 1;
    defaults->dgain_high_accuracy = 1;
    defaults->expos_lines = 0x003d2000;
    defaults->expos_accuracy = 4096;
    defaults->sexpos_accuracy = 4096;
    defaults->vsexpos_accuracy = 4096;
    defaults->vvsexpos_accuracy = 4096;
    defaults->integration_time_min = 4U << 12;
    defaults->integration_time_max = (OV13B10_TOTAL_HEIGHT - 8U) << 12;
    defaults->integration_time_long_max = defaults->integration_time_max;
    defaults->integration_time_limit = defaults->integration_time_max;
    defaults->lines_per_second = OV13B10_LINES_PER_SECOND;
    defaults->sensor_exp_number = 1;
    defaults->sensor_gain_number = 0;
    defaults->bits = 10;
    defaults->total.width = OV13B10_TOTAL_WIDTH;
    defaults->total.height = OV13B10_TOTAL_HEIGHT;
    defaults->active.width = OV13B10_WIDTH;
    defaults->active.height = OV13B10_HEIGHT;
}

static int ov13b10_get_default(int pipe, ALG_SENSOR_DEFAULT_S *defaults)
{
    (void)pipe;
    if (!defaults)
        return -EINVAL;

    pthread_mutex_lock(&runtime.lock);
    reset_defaults(&runtime.defaults);
    memcpy(defaults, &runtime.defaults, sizeof(*defaults));
    pthread_mutex_unlock(&runtime.lock);
    return 0;
}

static void ov13b10_again_calc(int pipe, uint32_t *again_log2,
                               uint32_t *again_sensor)
{
    double sensor_gain;
    uint32_t requested;
    uint32_t value;
    static uint32_t last_requested = UINT32_MAX;
    static uint32_t last_value = UINT32_MAX;

    (void)pipe;
    (void)again_sensor;
    if (!again_log2)
        return;

    /*
     * Amlogic's sensor plug-ins use 0x80 as unity gain.  The AE core may
     * briefly request a value above the advertised sensor limit while it is
     * converging, so clamp in log2 space before converting it for the V4L2
     * sensor driver.  Using 0x100 here doubled every requested gain and let
     * the initial out-of-range request reach the OV13B10 driver unchanged.
     */
    requested = *again_log2;
    if (*again_log2 > OV13B10_AGAIN_LOG2_MAX)
        *again_log2 = OV13B10_AGAIN_LOG2_MAX;
    sensor_gain = 128.0 * exp2((double)(*again_log2) / 4096.0);
    value = (uint32_t)llround(sensor_gain);

    if (requested != last_requested || value != last_value) {
        fprintf(stderr,
                "chiron-camera: gain log2 requested=%u applied=%u sensor=%u\n",
                requested, *again_log2, value);
        last_requested = requested;
        last_value = value;
    }

    pthread_mutex_lock(&runtime.lock);
    runtime.gain = value;
    runtime.gain_pending = 1;
    pthread_mutex_unlock(&runtime.lock);
}

static void ov13b10_dgain_calc(int pipe, uint32_t *dgain_log2,
                               uint32_t *dgain_sensor)
{
    (void)pipe;
    (void)dgain_log2;
    (void)dgain_sensor;
}

static void ov13b10_inttime_calc(int pipe, uint32_t long_exp,
                                 uint32_t short_exp, uint32_t very_short_exp,
                                 uint32_t very_very_short_exp)
{
    uint32_t long_lines = long_exp >> 12;
    uint32_t short_lines = short_exp >> 12;

    (void)pipe;
    (void)very_short_exp;
    (void)very_very_short_exp;
    if (long_lines < 4)
        long_lines = 4;
    if (long_lines > OV13B10_TOTAL_HEIGHT)
        long_lines = OV13B10_TOTAL_HEIGHT;
    if (short_lines < 4)
        short_lines = 4;

    pthread_mutex_lock(&runtime.lock);
    runtime.exposure = long_lines;
    runtime.short_exposure = short_lines;
    runtime.exposure_pending = 1;
    pthread_mutex_unlock(&runtime.lock);
}

static int set_control(struct media_entity *entity, uint32_t id, int32_t value)
{
    struct v4l2_ext_control control;

    if (!entity)
        return -ENODEV;
    memset(&control, 0, sizeof(control));
    control.id = id;
    control.value = value;
    return v4l2_subdev_set_ctrls(entity, &control, 1);
}

static void ov13b10_fps_set(int pipe, float requested,
                            ALG_SENSOR_DEFAULT_S *defaults)
{
    float fps = requested > 255.0f ? requested / 256.0f : requested;
    uint32_t vts;

    (void)pipe;
    if (fps < 1.0f)
        fps = 1.0f;
    if (fps > 30.0f)
        fps = 30.0f;
    vts = (uint32_t)(97920.0f / fps);

    pthread_mutex_lock(&runtime.lock);
    runtime.defaults.fps = (uint32_t)lrintf(fps * 256.0f);
    runtime.defaults.total.height = vts;
    runtime.defaults.integration_time_max = (vts - 8U) << 12;
    runtime.defaults.integration_time_long_max = runtime.defaults.integration_time_max;
    runtime.defaults.integration_time_limit = runtime.defaults.integration_time_max;
    runtime.defaults.lines_per_second = (uint32_t)((vts - 8U) * fps);
    if (defaults)
        memcpy(defaults, &runtime.defaults, sizeof(*defaults));
    set_control(runtime.sensor, OV13B10_FPS_CONTROL, (int32_t)lrintf(fps));
    pthread_mutex_unlock(&runtime.lock);
}

static void ov13b10_alg_update(int pipe)
{
    struct media_entity *sensor;
    uint32_t gain = 0;
    uint32_t exposure = 0;
    unsigned int write_gain;
    unsigned int write_exposure;

    (void)pipe;
    pthread_mutex_lock(&runtime.lock);
    sensor = runtime.sensor;
    write_gain = runtime.gain_pending;
    write_exposure = runtime.exposure_pending;
    if (write_gain) {
        gain = runtime.gain;
        runtime.gain_pending = 0;
    }
    if (write_exposure) {
        exposure = runtime.exposure;
        runtime.exposure_pending = 0;
    }
    pthread_mutex_unlock(&runtime.lock);

    if (write_gain && set_control(sensor, V4L2_CID_GAIN, (int32_t)gain) < 0)
        fprintf(stderr, "chiron-camera: failed to set gain %u\n", gain);
    if (write_exposure &&
        set_control(sensor, V4L2_CID_EXPOSURE, (int32_t)exposure) < 0)
        fprintf(stderr, "chiron-camera: failed to set exposure %u\n", exposure);
}

static void ov13b10_set_entity(struct media_entity *sensor, int wdr)
{
    (void)wdr;
    pthread_mutex_lock(&runtime.lock);
    runtime.sensor = sensor;
    reset_defaults(&runtime.defaults);
    pthread_mutex_unlock(&runtime.lock);
    pthread_once(&wb_probe_once, start_wb_probe);
}

static void ov13b10_get_calibration(struct media_entity *sensor,
                                    aisp_calib_info_t *calibration)
{
    unsigned int index;

    (void)sensor;
    if (!calibration)
        return;
    pthread_once(&calibration_once, load_calibration);
    if (calibration_status != 0) {
        fprintf(stderr, "chiron-camera: factory OV13B10 calibration unavailable\n");
        _exit(78);
    }
    memset(calibration, 0, sizeof(*calibration));
    for (index = 0; index < CALIBRATION_TOTAL_SIZE; ++index) {
        if (calibration_present[index])
            calibration->calibrations[index] = &calibration_tables[index];
    }
}

static struct sensorConfig ov13b10_config = {
    .expFunc = {
        .pfn_cmos_fps_set = ov13b10_fps_set,
        .pfn_cmos_get_alg_default = ov13b10_get_default,
        .pfn_cmos_alg_update = ov13b10_alg_update,
        .pfn_cmos_again_calc_table = ov13b10_again_calc,
        .pfn_cmos_dgain_calc_table = ov13b10_dgain_calc,
        .pfn_cmos_inttime_calc_table = ov13b10_inttime_calc,
    },
    .cmos_set_sensor_entity = ov13b10_set_entity,
    .cmos_get_sensor_calibration = ov13b10_get_calibration,
    .sensorWidth = OV13B10_WIDTH,
    .sensorHeight = OV13B10_HEIGHT,
    .sensorName = "ov13b10",
    .wdrFormat = OV13B10_SBGGR10_CODE,
    .sdrFormat = OV13B10_SBGGR10_CODE,
    .type = sensor_raw,
};

struct sensorConfig *matchSensorConfigByStream(media_stream_t *stream)
{
    if (!stream)
        return NULL;
    if (stream->sensor_ent_name[0] &&
        !strstr(stream->sensor_ent_name, "ov13b10"))
        fprintf(stderr, "chiron-camera: forcing OV13B10 profile for %s\n",
                stream->sensor_ent_name);
    return &ov13b10_config;
}

struct sensorConfig *matchSensorConfigByName(const char *name)
{
    if (!name || !strstr(name, "ov13b10"))
        return NULL;
    return &ov13b10_config;
}

void cmos_sensor_control_cb(struct sensorConfig *config,
                            ALG_SENSOR_EXP_FUNC_S *functions)
{
    if (config && functions)
        memcpy(functions, &config->expFunc, sizeof(*functions));
}

void cmos_set_sensor_entity(struct sensorConfig *config,
                            struct media_entity *sensor, int wdr)
{
    if (config && config->cmos_set_sensor_entity)
        config->cmos_set_sensor_entity(sensor, wdr);
}

void cmos_get_sensor_calibration(struct sensorConfig *config,
                                 struct media_entity *sensor,
                                 aisp_calib_info_t *calibration)
{
    if (config && config->cmos_get_sensor_calibration)
        config->cmos_get_sensor_calibration(sensor, calibration);
}

int fetchPipeMaxResolution(media_stream_t *stream, uint32_t *width,
                           uint32_t *height)
{
    if (!stream || !width || !height)
        return -EINVAL;
    *width = OV13B10_WIDTH;
    *height = OV13B10_HEIGHT;
    return 0;
}
