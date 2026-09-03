/**
 * @file    bsp_camera.h
 * @brief   摄像头 BSP 接口（SC2336 MIPI-CSI，esp_video V4L2 方案）
 *
 * 硬件事实（官方 BSP 冻结基线）：
 *   - 传感器 SC2336，MIPI-CSI 2-lane，SCCB 复用主 I2C1(GPIO7/8, 400kHz)
 *   - 传感器输出 RAW8 1280x720@30fps（Kconfig 默认格式），经 P4 ISP 转 RGB565
 *   - V4L2 设备 /dev/video0（ESP_VIDEO_MIPI_CSI_DEVICE_NAME）
 *   - 无独立 RST/PWDN 引脚（NC / -1）
 * V4L2 流程对齐官方 esp_brookesia_phone 的 app_video.c：
 *   open → S_FMT(RGB565) → REQBUFS(USERPTR) → QBUF → STREAMON → DQBUF 循环
 *
 * @date    2026-09-03
 * @version 1.0.0
 */

#ifndef BSP_CAMERA_H
#define BSP_CAMERA_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 摄像头默认参数（对齐官方 Camera App） */
#define BSP_CAMERA_DEV_PATH         "/dev/video0"
#define BSP_CAMERA_WIDTH            (1280)
#define BSP_CAMERA_HEIGHT           (720)
#define BSP_CAMERA_BUF_COUNT        (4)     /* USERPTR 帧缓冲数（2~6） */
#define BSP_CAMERA_TASK_STACK       (4 * 1024)
#define BSP_CAMERA_TASK_PRIORITY    (3)
#define BSP_CAMERA_TASK_CORE        (0)

/**
 * @brief 帧回调（在流任务上下文中调用，勿做耗时处理）
 *
 * @param frame 帧数据指针（RGB565，USERPTR 缓冲，回调返回后失效）
 * @param index 缓冲索引
 * @param width 宽（像素）
 * @param height 高（像素）
 * @param size  帧字节数（width*height*2）
 */
typedef void (*bsp_camera_frame_cb_t)(uint8_t *frame, uint32_t index,
                                      uint32_t width, uint32_t height, uint32_t size);

/**
 * @brief 初始化摄像头（esp_video_init + 打开 /dev/video0 + 配置 RGB565 格式）
 *
 * 幂等；重复调用直接返回 ESP_OK。无摄像头模组时返回错误但不影响系统其它部分。
 */
esp_err_t bsp_camera_init(void);

/**
 * @brief 启动取流（分配 PSRAM USERPTR 缓冲 + 创建流任务）
 * @param cb 帧回调；传 NULL 时内部每 30 帧打一条日志（验证用）
 */
esp_err_t bsp_camera_start_stream(bsp_camera_frame_cb_t cb);

/**
 * @brief 停止取流（阻塞至流任务退出）
 */
esp_err_t bsp_camera_stop_stream(void);

/** @brief 累计帧数（自 init 起） */
uint32_t bsp_camera_get_frame_count(void);

/** @brief 摄像头是否就绪 */
bool bsp_camera_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_CAMERA_H */
