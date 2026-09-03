/**
 * @file    bsp_camera.c
 * @brief   摄像头 BSP 实现（SC2336 + esp_video V4L2 + USERPTR 流任务）
 *
 * 初始化：BSP bsp_camera_start()（复用主 I2C 总线 SCCB 400kHz，无 RST/PWDN）
 * 取流：  open("/dev/video0") → S_FMT(RGB565) → REQBUFS(USERPTR, 4×PSRAM)
 *         → QBUF → STREAMON → 流任务 DQBUF/回调/QBUF 循环
 * 对齐官方 esp_brookesia_phone 的 app_video.c / Camera.cpp 组合。
 *
 * @date    2026-09-03
 * @version 1.0.0
 */

#include "bsp_camera.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/* 官方 BSP */
#include "bsp/esp-bsp.h"

#define TAG "bsp_camera"

/* 缓冲对齐：P4 L2 cache line=128B（CONFIG_CACHE_L2_CACHE_LINE_128B），
 * esp_video 要求 USERPTR 地址按 cache line 对齐，运行时查询兜底 */
static uint32_t s_buf_align = 128;

/* 事件位 */
#define EVT_STOP_REQ   BIT0
#define EVT_STOP_DONE  BIT1

typedef struct {
    int fd;                                            /* V4L2 设备描述符 */
    uint8_t *bufs[BSP_CAMERA_BUF_COUNT];               /* PSRAM USERPTR 缓冲 */
    size_t buf_size;                                   /* 单帧字节数 */
    uint32_t width, height;                            /* 帧尺寸 */
    int mem_mode;                                      /* V4L2_MEMORY_USERPTR */
    bsp_camera_frame_cb_t cb;                          /* 用户帧回调 */
    TaskHandle_t task_handle;
    EventGroupHandle_t events;
    uint32_t frame_count;
    bool inited;                                       /* init 完成（设备打开） */
    bool streaming;                                    /* 流任务运行中 */
} bsp_camera_state_t;

static bsp_camera_state_t s_cam = {
    .fd = -1,
};

static esp_err_t stream_start(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t stream_stop(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type) != 0) {
        ESP_LOGE(TAG, "STREAMOFF failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void stream_task(void *arg)
{
    struct v4l2_buffer buf;

    while (1) {
        /* 出队一帧 */
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = s_cam.mem_mode;
        if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "DQBUF failed");
            break;
        }

        s_cam.frame_count++;
        uint8_t *frame = s_cam.bufs[buf.index];

        /* 用户回调（流任务上下文，勿阻塞） */
        if (s_cam.cb) {
            s_cam.cb(frame, buf.index, s_cam.width, s_cam.height, s_cam.buf_size);
        } else if ((s_cam.frame_count % 30) == 0) {
            ESP_LOGI(TAG, "帧计数: %lu (%lux%lu RGB565)",
                     (unsigned long)s_cam.frame_count,
                     (unsigned long)s_cam.width, (unsigned long)s_cam.height);
        }

        /* 重新入队 */
        buf.m.userptr = (unsigned long)frame;
        buf.length = s_cam.buf_size;
        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF failed");
            break;
        }

        /* 停止请求检查 */
        if (xEventGroupGetBits(s_cam.events) & EVT_STOP_REQ) {
            break;
        }
    }

    stream_stop();
    xEventGroupSetBits(s_cam.events, EVT_STOP_DONE);
    s_cam.streaming = false;
    vTaskDelete(NULL);
}

esp_err_t bsp_camera_init(void)
{
    esp_err_t err;

    if (s_cam.inited) {
        return ESP_OK;
    }

    /* 1. esp_video_init（复用 BSP 主 I2C 作 SCCB） */
    err = bsp_camera_start(NULL);
    ESP_RETURN_ON_ERROR(err, TAG, "bsp_camera_start (esp_video_init) failed");

    /* 2. 打开 V4L2 设备 */
    s_cam.fd = open(BSP_CAMERA_DEV_PATH, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_cam.fd >= 0, ESP_FAIL, TAG, "open %s failed", BSP_CAMERA_DEV_PATH);

    /* 3. 获取默认格式（Kconfig 默认 RAW8 1280x720@30 → 设备默认输出） */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "G_FMT failed");
        goto err;
    }
    s_cam.width = fmt.fmt.pix.width;
    s_cam.height = fmt.fmt.pix.height;
    ESP_LOGI(TAG, "设备默认格式: %lux%lu fmt=0x%08lx",
             (unsigned long)s_cam.width, (unsigned long)s_cam.height,
             (unsigned long)fmt.fmt.pix.pixelformat);

    /* 4. 切换像素格式为 RGB565（ISP 管线完成 RAW→RGB 转换） */
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(s_cam.fd, VIDIOC_S_FMT, &fmt) != 0) {
            ESP_LOGE(TAG, "S_FMT(RGB565) failed");
            goto err;
        }
    }
    /* 帧大小：G_FMT/S_FMT 的 sizeimage 可能不回填，回退按 RGB565 手算（官方同款） */
    s_cam.buf_size = fmt.fmt.pix.sizeimage
                     ? fmt.fmt.pix.sizeimage
                     : (size_t)s_cam.width * s_cam.height * 2;
    ESP_LOGI(TAG, "输出格式: RGB565 %lux%lu, 帧大小 %u 字节",
             (unsigned long)s_cam.width, (unsigned long)s_cam.height, (unsigned)s_cam.buf_size);

    s_cam.inited = true;
    ESP_LOGI(TAG, "摄像头初始化成功 (SC2336 MIPI-CSI)");
    return ESP_OK;

err:
    close(s_cam.fd);
    s_cam.fd = -1;
    return ESP_FAIL;
}

esp_err_t bsp_camera_start_stream(bsp_camera_frame_cb_t cb)
{
    ESP_RETURN_ON_FALSE(s_cam.inited, ESP_ERR_INVALID_STATE, TAG, "camera not init");
    if (s_cam.streaming) {
        return ESP_OK;
    }
    s_cam.cb = cb;

    /* 1. 请求缓冲（USERPTR 模式） */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BSP_CAMERA_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;
    s_cam.mem_mode = req.memory;
    if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "REQBUFS failed");
        return ESP_FAIL;
    }

    /* 2. QUERYBUF 探明驱动要求的缓冲长度（可能与手算值有对齐差异） */
    struct v4l2_buffer buf0;
    memset(&buf0, 0, sizeof(buf0));
    buf0.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf0.memory = req.memory;
    buf0.index = 0;
    if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf0) != 0) {
        ESP_LOGE(TAG, "QUERYBUF 0 failed");
        return ESP_FAIL;
    }
    s_cam.buf_size = buf0.length;
    ESP_LOGI(TAG, "驱动要求缓冲长度 %u 字节 (sizeimage=%u)",
             (unsigned)s_cam.buf_size, (unsigned)buf0.length);

    /* 3. 分配 PSRAM USERPTR 帧缓冲（按 cache line 对齐） */
    size_t align = 0;
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align) == ESP_OK && align > 0) {
        s_buf_align = (uint32_t)align;
    }
    ESP_LOGI(TAG, "USERPTR 缓冲对齐 %u 字节", (unsigned)s_buf_align);
    for (int i = 0; i < BSP_CAMERA_BUF_COUNT; i++) {
        if (!s_cam.bufs[i]) {
            s_cam.bufs[i] = heap_caps_aligned_alloc(s_buf_align, s_cam.buf_size,
                                                    MALLOC_CAP_SPIRAM);
            ESP_RETURN_ON_FALSE(s_cam.bufs[i], ESP_ERR_NO_MEM, TAG, "alloc buf %d failed", i);
        }
    }

    /* 4. 逐个 QUERYBUF + 绑定用户指针 + 入队（保留 QUERYBUF 返回的 length） */
    for (int i = 0; i < BSP_CAMERA_BUF_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = req.memory;
        buf.index = i;
        if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QUERYBUF %d failed", i);
            return ESP_FAIL;
        }
        buf.m.userptr = (unsigned long)s_cam.bufs[i];
        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF %d failed (errno=%d, length=%u, userptr=%p)",
                     i, errno, (unsigned)buf.length, s_cam.bufs[i]);
            return ESP_FAIL;
        }
    }

    /* 3. 事件组 + 流任务 */
    if (!s_cam.events) {
        s_cam.events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_cam.events, ESP_ERR_NO_MEM, TAG, "create events failed");
    }
    xEventGroupClearBits(s_cam.events, EVT_STOP_REQ | EVT_STOP_DONE);

    if (xTaskCreatePinnedToCore(stream_task, "cam_stream", BSP_CAMERA_TASK_STACK,
                                NULL, BSP_CAMERA_TASK_PRIORITY, &s_cam.task_handle,
                                BSP_CAMERA_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create stream task failed");
        return ESP_FAIL;
    }

    /* 4. STREAMON（任务内先跑起来再开流会导致首帧前 DQBUF 阻塞，属正常等待） */
    esp_err_t err = stream_start();
    ESP_RETURN_ON_ERROR(err, TAG, "stream start failed");

    s_cam.streaming = true;
    ESP_LOGI(TAG, "取流启动 (%d×PSRAM USERPTR 缓冲, 每帧 %u 字节)",
             BSP_CAMERA_BUF_COUNT, (unsigned)s_cam.buf_size);
    return ESP_OK;
}

esp_err_t bsp_camera_stop_stream(void)
{
    if (!s_cam.streaming) {
        return ESP_OK;
    }
    xEventGroupSetBits(s_cam.events, EVT_STOP_REQ);
    EventBits_t bits = xEventGroupWaitBits(s_cam.events, EVT_STOP_DONE,
                                           pdTRUE, pdTRUE, pdMS_TO_TICKS(2000));
    if (!(bits & EVT_STOP_DONE)) {
        ESP_LOGW(TAG, "流任务停止超时");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "取流已停止 (总帧数 %lu)", (unsigned long)s_cam.frame_count);
    return ESP_OK;
}

uint32_t bsp_camera_get_frame_count(void)
{
    return s_cam.frame_count;
}

bool bsp_camera_is_ready(void)
{
    return s_cam.inited;
}
