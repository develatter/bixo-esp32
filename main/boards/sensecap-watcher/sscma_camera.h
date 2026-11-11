#ifndef SSCMA_CAMERA_H
#define SSCMA_CAMERA_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <esp_io_expander_tca95xx_16bit.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mbedtls/base64.h>

#include "camera.h"
#include "sscma_client.h"

struct SscmaData {
    uint8_t* img;
    size_t len;
};
struct JpegData {
    uint8_t* buf;
    size_t len;
};

class SscmaCamera : public Camera {
private:
    std::string explain_url_;
    std::string explain_token_;
    sscma_client_io_handle_t sscma_client_io_handle_;
    sscma_client_handle_t sscma_client_handle_;
    QueueHandle_t sscma_data_queue_;
    JpegData jpeg_data_;
    // Detection state machine
    enum DetectionState {
        IDLE,        // idle
        VALIDATING,  // validating (continuous detection, detect_duration_sec)
        COOLDOWN     // cooldown (waiting to detect again)
    };

    DetectionState detection_state = IDLE;
    int64_t state_start_time = 0;
    bool need_start_cooldown = false;  // whether the cooldown period needs to start
    int64_t last_detected_time = 0;    // time of the last object detection during validation

    int detect_target = 0;
    int detect_threshold = 75;
    int detect_duration_sec = 2;         // a person must stay this long to count
    int detect_invoke_interval_sec = 8;  // cooldown so sessions do not start too often
    int detect_debounce_sec = 1;         // 1-second debounce time for a person leaving during validation
    int inference_en = 0;                // inference enable switch (0: off, 1: on)
    bool sscma_restarted_ = false;

    sscma_client_model_t* model;
    int model_class_cnt = 0;

    // The AI chip draws power all the time, so it is switched off when unused: a photo powers
    // it up (about a second) and the camera task switches it off after kIdlePowerOffUs.
    static constexpr int64_t kIdlePowerOffUs = 30 * 1000 * 1000;
    esp_io_expander_handle_t io_exp_handle_;
    std::mutex chip_mutex_;  // held for every request to the chip and to switch it on or off
    bool powered_ = true;
    int64_t last_use_us_ = 0;
    std::atomic<bool> connected_{false};  // set when the chip reports it has booted
    bool PowerUp();    // with chip_mutex_ held
    void PowerDown();  // with chip_mutex_ held

public:
    SscmaCamera(esp_io_expander_handle_t io_exp_handle);
    ~SscmaCamera();
    void InitializeMcpTools();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    // Flip control functions
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::expected<std::string, std::string> Explain(const std::string& question) override;
};

#endif  // ESP32_CAMERA_H
