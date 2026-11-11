#include "sscma_camera.h"
#include "board.h"
#include "config.h"
#include "lvgl_display.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cstring>
#include "application.h"
#include "sscma_client_commands.h"

#define TAG "SscmaCamera"

#define IMG_JPEG_BUF_SIZE 48 * 1024

static bool __himax_keepalive_check(sscma_client_handle_t client) {
    esp_err_t ret = ESP_OK;
    sscma_client_reply_t reply = {0};
    int retry = 3;
    while (retry--) {
        ret = sscma_client_request(client, CMD_PREFIX CMD_AT_ID CMD_QUERY CMD_SUFFIX, &reply, true,
                                   pdMS_TO_TICKS(2000));
        if (reply.payload != NULL) {
            sscma_client_reply_clear(&reply);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Himax keepalive check failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            return true;
        }
    }
    return false;
}

SscmaCamera::SscmaCamera(esp_io_expander_handle_t io_exp_handle) : io_exp_handle_(io_exp_handle) {
    sscma_client_io_spi_config_t spi_io_config = {0};
    spi_io_config.sync_gpio_num = BSP_SSCMA_CLIENT_SPI_SYNC;
    spi_io_config.cs_gpio_num = BSP_SSCMA_CLIENT_SPI_CS;
    spi_io_config.pclk_hz = BSP_SSCMA_CLIENT_SPI_CLK;
    spi_io_config.spi_mode = 0;
    spi_io_config.wait_delay =
        10;  // At least 4ms is needed between two transfers, but FREERTOS_HZ=100 currently limits delay precision to 10ms,
    spi_io_config.user_ctx = NULL;
    spi_io_config.io_expander = io_exp_handle;
    spi_io_config.flags.sync_use_expander = BSP_SSCMA_CLIENT_RST_USE_EXPANDER;

    sscma_client_new_io_spi_bus((sscma_client_spi_bus_handle_t)BSP_SSCMA_CLIENT_SPI_NUM,
                                &spi_io_config, &sscma_client_io_handle_);

    sscma_client_config_t sscma_client_config = SSCMA_CLIENT_CONFIG_DEFAULT();
    sscma_client_config.event_queue_size = CONFIG_SSCMA_EVENT_QUEUE_SIZE;
    sscma_client_config.tx_buffer_size = CONFIG_SSCMA_TX_BUFFER_SIZE;
    sscma_client_config.rx_buffer_size = CONFIG_SSCMA_RX_BUFFER_SIZE;
    sscma_client_config.process_task_stack = CONFIG_SSCMA_PROCESS_TASK_STACK_SIZE;
    sscma_client_config.process_task_affinity = CONFIG_SSCMA_PROCESS_TASK_AFFINITY;
    sscma_client_config.process_task_priority = CONFIG_SSCMA_PROCESS_TASK_PRIORITY;
    sscma_client_config.monitor_task_stack = CONFIG_SSCMA_MONITOR_TASK_STACK_SIZE;
    sscma_client_config.monitor_task_affinity = CONFIG_SSCMA_MONITOR_TASK_AFFINITY;
    sscma_client_config.monitor_task_priority = CONFIG_SSCMA_MONITOR_TASK_PRIORITY;
    sscma_client_config.reset_gpio_num = BSP_SSCMA_CLIENT_RST;
    sscma_client_config.io_expander = io_exp_handle;
    sscma_client_config.flags.reset_use_expander = BSP_SSCMA_CLIENT_RST_USE_EXPANDER;

    sscma_client_new(sscma_client_io_handle_, &sscma_client_config, &sscma_client_handle_);

    sscma_data_queue_ = xQueueCreate(1, sizeof(SscmaData));

    sscma_client_callback_t callback = {0};

    detection_state = SscmaCamera::IDLE;
    state_start_time = 0;
    need_start_cooldown = false;
    callback.on_event = [](sscma_client_handle_t client, const sscma_client_reply_t* reply,
                           void* user_ctx) {
        SscmaCamera* self = static_cast<SscmaCamera*>(user_ctx);
        if (!self)
            return;

        char* img = NULL;
        int img_size = 0;
        int box_count = 0;
        sscma_client_box_t* boxes = NULL;
        int class_count = 0;
        sscma_client_class_t* classes = NULL;
        int point_count = 0;
        sscma_client_point_t* points = NULL;
        int model_type = 0;
        int obj_cnt = 0;

        int width = 0, height = 0;
        cJSON* data = cJSON_GetObjectItem(reply->payload, "data");
        if (data != NULL && cJSON_IsObject(data)) {
            cJSON* resolution = cJSON_GetObjectItem(data, "resolution");
            if (data != NULL && cJSON_IsArray(resolution) && cJSON_GetArraySize(resolution) == 2) {
                width = cJSON_GetArrayItem(resolution, 0)->valueint;
                height = cJSON_GetArrayItem(resolution, 1)->valueint;
            }
        }

        switch ((width + height)) {
            case (416 + 416): {
                bool is_object_detected = false;
                bool is_need_wake = false;

                // Update detection config periodically to avoid frequent NVS access
                int64_t cur_tm = esp_timer_get_time();

                // Try to get bounding-box data (object detection model)
                if (sscma_utils_fetch_boxes_from_reply(reply, &boxes, &box_count) == ESP_OK &&
                    box_count > 0) {
                    for (int i = 0; i < box_count; i++) {
                        ESP_LOGI(TAG, "[box %d]: x=%d, y=%d, w=%d, h=%d, score=%d, target=%d", i,
                                 boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h, boxes[i].score,
                                 boxes[i].target);
                        if (boxes[i].target == self->detect_target &&
                            boxes[i].score > self->detect_threshold) {
                            is_object_detected = true;
                            model_type = 0;
                            obj_cnt++;
                            break;
                        }
                    }
                    free(boxes);
                } else if (sscma_utils_fetch_classes_from_reply(reply, &classes, &class_count) ==
                               ESP_OK &&
                           class_count > 0) {
                    // Try to get classification data (classification model)
                    for (int i = 0; i < class_count; i++) {
                        ESP_LOGI(TAG, "[class %d]: target=%d, score=%d", i, classes[i].target,
                                 classes[i].score);
                        if (classes[i].target == self->detect_target &&
                            classes[i].score > self->detect_threshold) {
                            is_object_detected = true;
                            model_type = 1;
                            obj_cnt++;
                        }
                    }
                    free(classes);
                } else if (sscma_utils_fetch_points_from_reply(reply, &points, &point_count) ==
                               ESP_OK &&
                           point_count > 0) {
                    // Try to get keypoint data (pose estimation model)
                    for (int i = 0; i < point_count; i++) {
                        ESP_LOGI(TAG, "[point %d]: x=%d, y=%d, z=%d, score=%d, target=%d", i,
                                 points[i].x, points[i].y, points[i].z, points[i].score,
                                 points[i].target);
                        if (points[i].target == self->detect_target &&
                            points[i].score > self->detect_threshold) {
                            is_object_detected = true;
                            model_type = 2;
                            obj_cnt++;
                        }
                    }
                    free(points);
                }

                // If a cooldown period needs to start, start timing now
                if (self->need_start_cooldown) {  // callback paused; keep the flag and start timing once the callback resumes
                    self->state_start_time = cur_tm;
                    self->need_start_cooldown = false;
                    ESP_LOGI(TAG, "Starting cooldown timer");
                }

                // State-machine-driven detection logic - triggers only when a person appears
                switch (self->detection_state) {
                    case SscmaCamera::IDLE:
                        if (is_object_detected) {
                            // Person appeared, start validating (this is the none-to-present transition)
                            self->detection_state = SscmaCamera::VALIDATING;
                            self->state_start_time = cur_tm;    // record the time the object appeared
                            self->last_detected_time = cur_tm;  // initialize the last detection time
                            ESP_LOGI(TAG, "object appeared, starting validation");
                        }
                        break;

                    case SscmaCamera::VALIDATING:
                        if (is_object_detected) {
                            // Update the last detection time
                            self->last_detected_time = cur_tm;
                            // Check whether validation has lasted long enough
                            if ((cur_tm - self->state_start_time) >=
                                (self->detect_duration_sec * 1000000)) {
                                is_need_wake = true;
                            }
                        } else {
                            // Person left during validation, check the debounce time
                            if (self->last_detected_time > 0 &&
                                (cur_tm - self->last_detected_time) >=
                                    self->detect_debounce_sec * 1000000LL) {
                                // Debounce time elapsed, person confirmed gone, return to idle
                                self->detection_state = SscmaCamera::IDLE;
                                self->last_detected_time = 0;
                                ESP_LOGI(TAG,
                                         "object left during validation (debounced), back to idle");
                            }
                        }
                        break;

                    case SscmaCamera::COOLDOWN:
                        // Cooldown: two conditions must be met: 1) the object has left 2) detect_invoke_interval_sec has passed
                        if (!is_object_detected &&
                            (cur_tm - self->state_start_time) >=
                                (self->detect_invoke_interval_sec * 1000000LL)) {
                            // Object left and cooldown elapsed, return to idle
                            self->detection_state = SscmaCamera::IDLE;
                            ESP_LOGI(TAG,
                                     "Cooldown complete and object left, back to idle - ready for "
                                     "next appearance");
                        }
                        // Otherwise stay in cooldown
                        break;
                }

                if (is_need_wake) {
                    ESP_LOGI(TAG,
                             "Validation complete, triggering conversation (type=%d, res=%dx%d)",
                             self->detect_target, width, height);

                    // Trigger a conversation
                    std::string wake_word;
                    if (model_type == 0) {
                        std::string cached_target_name = "object";
                        if (self->model != NULL &&
                            self->model->classes[self->detect_target] != NULL) {
                            cached_target_name = self->model->classes[self->detect_target];
                        }
                        wake_word = "<detect>" + std::to_string(obj_cnt) + " " +
                                    cached_target_name + " detected </detect>";
                    } else if (model_type == 1) {
                        std::string cached_target_name = "object";
                        if (self->model != NULL &&
                            self->model->classes[self->detect_target] != NULL) {
                            cached_target_name = self->model->classes[self->detect_target];
                        }
                        wake_word = "<detect>" + std::to_string(obj_cnt) + " " +
                                    cached_target_name + " detected </detect>";
                    } else if (model_type == 2) {
                        std::string cached_target_name = "object";
                        if (self->model != NULL &&
                            self->model->classes[self->detect_target] != NULL) {
                            cached_target_name = self->model->classes[self->detect_target];
                        }
                        wake_word = "<detect>" + std::to_string(obj_cnt) + " " +
                                    cached_target_name + " detected </detect>";
                    }
                    printf("wake_word:%s\n", wake_word.c_str());
                    Application::GetInstance().WakeWordInvoke(wake_word);

                    // Enter cooldown and mark that the cooldown period needs to start; the variables below are used after the session ends, timing starts once the callback resumes
                    self->detection_state = SscmaCamera::COOLDOWN;
                    self->need_start_cooldown = true;
                }
            } break;
            case (640 + 480):

                if (sscma_utils_fetch_image_from_reply(reply, &img, &img_size) == ESP_OK) {
                    ESP_LOGI(TAG, "image_size: %d\n", img_size);
                    // Send the data out through the queue
                    SscmaData data;
                    data.img = (uint8_t*)img;
                    data.len = img_size;

                    // Clear the queue so only the latest data is kept
                    SscmaData dummy;
                    while (xQueueReceive(self->sscma_data_queue_, &dummy, 0) == pdPASS) {
                        if (dummy.img) {
                            heap_caps_free(dummy.img);
                        }
                    }
                    xQueueSend(self->sscma_data_queue_, &data, 0);
                    // Note: the receiver is responsible for freeing img
                }
                break;
            default:
                ESP_LOGI(TAG, "unknown resolution");
                break;
        }
    };
    callback.on_connect = [](sscma_client_handle_t client, const sscma_client_reply_t* reply,
                             void* user_ctx) {
        ESP_LOGI(TAG, "SSCMA client connected");
        SscmaCamera* self = static_cast<SscmaCamera*>(user_ctx);
        if (self) {
            self->sscma_restarted_ = true;
            self->connected_ = true;
        }
    };

    callback.on_log = [](sscma_client_handle_t client, const sscma_client_reply_t* reply,
                         void* user_ctx) { ESP_LOGI(TAG, "log: %s\n", reply->data); };

    sscma_client_register_callback(sscma_client_handle_, &callback, this);

    sscma_client_init(sscma_client_handle_);

    ESP_LOGI(TAG, "SSCMA client initialized");
    // Set the resolution
    // 3 = 640x480
    if (sscma_client_set_sensor(sscma_client_handle_, 1, 3, true)) {
        ESP_LOGE(TAG, "Failed to set sensor");
        sscma_client_del(sscma_client_handle_);
        sscma_client_handle_ = NULL;
        return;
    }

    // Get device info
    sscma_client_info_t* info;
    if (sscma_client_get_info(sscma_client_handle_, &info, true) == ESP_OK) {
        ESP_LOGI(TAG, "Device Info - ID: %s, Name: %s", info->id ? info->id : "NULL",
                 info->name ? info->name : "NULL");
    }
    // Initialize memory for the JPEG data
    jpeg_data_.len = 0;
    jpeg_data_.buf = (uint8_t*)heap_caps_malloc(IMG_JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    ;
    if (jpeg_data_.buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG buffer");
        return;
    }

    sscma_client_set_model(sscma_client_handle_, 4);
    model_class_cnt = 0;
    if (sscma_client_get_model(sscma_client_handle_, &model, true) == ESP_OK) {
        printf("ID: %d\n", model->id ? model->id : -1);
        printf("UUID: %s\n", model->uuid ? model->uuid : "N/A");
        printf("Name: %s\n", model->name ? model->name : "N/A");
        printf("Version: %s\n", model->ver ? model->ver : "N/A");
        printf("URL: %s\n", model->url ? model->url : "N/A");
        printf("Checksum: %s\n", model->checksum ? model->checksum : "N/A");
        printf("Classes:\n");
        if (model->classes[0] != NULL) {
            for (int i = 0; model->classes[i] != NULL; i++) {
                printf("  - %s\n", model->classes[i]);
                model_class_cnt++;
            }
        } else {
            printf("  N/A\n");
        }
    } else {
        printf("get model failed\n");
    }

    ESP_LOGI(TAG, "initialize mcp tools");
    InitializeMcpTools();

    last_use_us_ = esp_timer_get_time();  // powered for start-up, off after the idle time

    xTaskCreate(
        [](void* arg) {
            auto this_ = (SscmaCamera*)arg;
            bool is_inference = false;
            int64_t last_keepalive_time = esp_timer_get_time();
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(200));
                std::lock_guard<std::mutex> lock(this_->chip_mutex_);
                if (this_->powered_ && !this_->inference_en &&
                    esp_timer_get_time() - this_->last_use_us_ > kIdlePowerOffUs) {
                    this_->PowerDown();
                }
                if (!this_->powered_ && !this_->inference_en) {
                    continue;
                }
                if (this_->sscma_restarted_) {
                    ESP_LOGI(TAG, "SSCMA restarted detected");
                    this_->sscma_restarted_ = false;
                    is_inference = false;
                }

                if (esp_timer_get_time() - last_keepalive_time > 10 * 1000000) {
                    last_keepalive_time = esp_timer_get_time();
                    if (!__himax_keepalive_check(this_->sscma_client_handle_)) {
                        ESP_LOGE(TAG, "restart himax");
                        sscma_client_reset(this_->sscma_client_handle_);
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                }

                if (this_->inference_en &&
                    Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    if (!is_inference) {
                        ESP_LOGI(TAG, "Start inference (enable=1)");
                        if (!this_->powered_) {
                            if (!this_->PowerUp()) {
                                continue;
                            }
                            sscma_client_get_model(this_->sscma_client_handle_, &this_->model, true);
                        }
                        sscma_client_break(this_->sscma_client_handle_);
                        sscma_client_set_model(this_->sscma_client_handle_, 4);
                        sscma_client_set_sensor(this_->sscma_client_handle_, 1, 1,
                                                true);  // Set resolution to 416X416
                        sscma_client_invoke(this_->sscma_client_handle_, -1, false, true);
                        is_inference = true;
                    }
                } else if (is_inference &&
                           (!this_->inference_en ||
                            Application::GetInstance().GetDeviceState() != kDeviceStateIdle)) {
                    ESP_LOGI(TAG, "Stop inference (enable=%d state=%d)", this_->inference_en,
                             Application::GetInstance().GetDeviceState());
                    is_inference = false;
                    sscma_client_break(this_->sscma_client_handle_);
                    this_->last_use_us_ = esp_timer_get_time();
                }
            }
        },
        "sscma_camera", 4096, this, 1, nullptr);
}

bool SscmaCamera::PowerUp() {
    if (powered_) {
        return true;
    }
    ESP_LOGI(TAG, "Powering the AI chip up");
    connected_ = false;
    esp_io_expander_set_level(io_exp_handle_, BSP_PWR_AI_CHIP, 1);
    sscma_client_init(sscma_client_handle_);  // resets the chip and resumes polling it
    powered_ = true;
    last_use_us_ = esp_timer_get_time();
    for (int i = 0; i < 40 && !connected_; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!connected_) {
        ESP_LOGE(TAG, "The AI chip did not come up");
        PowerDown();
        return false;
    }
    return true;
}

void SscmaCamera::PowerDown() {
    if (!powered_) {
        return;
    }
    ESP_LOGI(TAG, "Powering the AI chip down");
    sscma_client_handle_->inited = false;  // its process task stops polling the chip
    vTaskDelay(pdMS_TO_TICKS(30));         // let a poll in flight finish
    esp_io_expander_set_level(io_exp_handle_, BSP_PWR_AI_CHIP, 0);
    powered_ = false;
}

SscmaCamera::~SscmaCamera() {
    if (sscma_client_handle_) {
        sscma_client_del(sscma_client_handle_);
    }
    if (sscma_data_queue_) {
        vQueueDelete(sscma_data_queue_);
    }
    if (jpeg_data_.buf) {
        heap_caps_free(jpeg_data_.buf);
        jpeg_data_.buf = nullptr;
    }
}

void SscmaCamera::InitializeMcpTools() {
    Settings settings("model", false);
    detect_threshold = settings.GetInt("threshold", 75);
    detect_invoke_interval_sec = settings.GetInt("interval", 8);
    detect_duration_sec = settings.GetInt("duration", 2);
    detect_target = settings.GetInt("target", 0);
    inference_en = settings.GetInt("enable", 0);

    auto& mcp_server = McpServer::GetInstance();
    // Get model parameter config
    mcp_server.AddTool("self.model.param_get",
                       "Get the current detection parameter config of the vision model.\n"
                       "The result contains:\n"
                       "  `threshold`: Detection confidence threshold (0-100); detections below it are ignored;\n"
                       "  `interval`: Cooldown (seconds) after a conversation is triggered, to prevent frequent interruptions;\n"
                       "  `duration`: Time (seconds) a detection must persist to be confirmed;\n"
                       "  `target`: Index of the detection target currently watched.",
                       PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                           Settings settings("model", false);
                           int threshold = settings.GetInt("threshold", 75);
                           int interval = settings.GetInt("interval", 8);
                           int duration = settings.GetInt("duration", 2);
                           int target_type = settings.GetInt("target", 0);

                           std::string result = "{\"threshold\":" + std::to_string(threshold) +
                                                ",\"interval\":" + std::to_string(interval) +
                                                ",\"duration\":" + std::to_string(duration) +
                                                ",\"target_type\":" + std::to_string(target_type) +
                                                "}";
                           return result;
                       });

    // Set model parameter config
    mcp_server.AddTool(
        "self.model.param_set",
        "Configure the vision model detection parameters. Use when the user wants to adjust detection sensitivity, frequency or a specific target.\n"
        "Parameters (all optional; omitted ones keep their current setting):\n"
        "  `threshold`: Confidence threshold (0-100). Raising it reduces false positives but may miss detections;\n"
        "  `interval`: Cooldown (seconds). How long after a conversation ends detection will not trigger again;\n"
        "  `duration`: Time (seconds) a detection must persist.\n"
        "  `target`: Index ID of the detection target to set.",
        PropertyList({Property("threshold", kPropertyTypeInteger, -1, -1, 100),
                      Property("interval", kPropertyTypeInteger, -1, -1, 60),
                      Property("duration", kPropertyTypeInteger, -1, -1, 60),
                      Property("target", kPropertyTypeInteger, -1, -1,
                               this->model_class_cnt > 0 ? this->model_class_cnt - 1 : 255)}),
        [this](const PropertyList& properties) -> ReturnValue {
            Settings settings("model", true);
            int threshold = properties["threshold"].value<int>();
            if (threshold != -1) {
                settings.SetInt("threshold", threshold);
                this->detect_threshold = threshold;
                ESP_LOGI(TAG, "Set detection threshold to %d", threshold);
            }

            int interval = properties["interval"].value<int>();
            if (interval != -1) {
                settings.SetInt("interval", interval);
                this->detect_invoke_interval_sec = interval;
                ESP_LOGI(TAG, "Set detection interval to %d", interval);
            }

            int duration = properties["duration"].value<int>();
            if (duration != -1) {
                settings.SetInt("duration", duration);
                this->detect_duration_sec = duration;
            }

            int target = properties["target"].value<int>();
            if (target != -1) {
                settings.SetInt("target", target);
                this->detect_target = target;
                ESP_LOGI(TAG, "Set detection target to %d", target);
            }

            return "{\"status\": \"success\", \"message\": \"Detection configuration updated\"}";
        });

    // Get the inference switch
    mcp_server.AddTool(
        "self.model.enable",
        "Turn visual inference (camera detection) on or off, or query the current state.\n"
        "Use when the user's request involves 'enable/disable inference' or 'start/stop detection'.\n"
        "Parameters:\n"
        "  `enable`: (optional) Integer. 1=enable inference, 0=disable inference. If omitted, returns the current switch state.",
        PropertyList({Property("enable", kPropertyTypeInteger, inference_en, 0, 1)}),
        [this](const PropertyList& properties) -> ReturnValue {
            Settings settings("model", true);
            int en = properties["enable"].value<int>();
            settings.SetInt("enable", en);
            this->inference_en = en;
            ESP_LOGI(TAG, "Set inference enable to %d", en);
            // Return the current config
            int cur_en = settings.GetInt("enable", this->inference_en);
            return std::string("{\"enable\":") + std::to_string(cur_en) + "}";
        });
}

void SscmaCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool SscmaCamera::Capture() {
    SscmaData data;
    int ret = 0;

    if (sscma_client_handle_ == nullptr) {
        ESP_LOGE(TAG, "SSCMA client handle is not initialized");
        return false;
    }

    std::lock_guard<std::mutex> lock(chip_mutex_);
    if (!PowerUp()) {
        return false;
    }
    last_use_us_ = esp_timer_get_time();
    if (sscma_client_set_sensor(sscma_client_handle_, 1, 3, true)) {
        ESP_LOGE(TAG, "Failed to set sensor");
        return false;
    }
    ESP_LOGI(TAG, "Capturing image...");
    // himax may have buffered data; only the latest photo is needed.
    if (sscma_client_sample(sscma_client_handle_, 1)) {
        ESP_LOGE(TAG, "Failed to capture image from SSCMA client");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));  // wait for the SSCMA client to process the data
    if (xQueueReceive(sscma_data_queue_, &data, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to receive JPEG data from SSCMA client");
        return false;
    }

    if (jpeg_data_.buf == nullptr) {
        heap_caps_free(data.img);
        return false;
    }

    ret = mbedtls_base64_decode(jpeg_data_.buf, IMG_JPEG_BUF_SIZE, &jpeg_data_.len, data.img,
                                data.len);
    if (ret != 0 || jpeg_data_.len == 0) {
        ESP_LOGE(TAG, "Failed to decode base64 image data, ret: %d, output_len: %zu", ret,
                 jpeg_data_.len);
        heap_caps_free(data.img);
        return false;
    }
    heap_caps_free(data.img);

    return true;
}
bool SscmaCamera::SetHMirror(bool enabled) { return false; }

bool SscmaCamera::SetVFlip(bool enabled) { return false; }

/**
 * @brief Send the image captured by the camera to a remote server for AI analysis and explanation
 *
 * This function encodes the image in the current camera buffer as JPEG and sends it via an HTTP POST request
 * as multipart/form-data to the specified explain server. The server analyzes the image
 * with AI based on the provided question and returns the result.
 *
 * @param question The question to ask the AI about the image, sent as a form field
 * @return On success, the server's JSON response; on failure, a readable error message
 *
 * @note SetExplainUrl() must be called first to set the server URL
 * @note The function waits for the previous encoding thread to finish before starting new processing
 * @warning If the camera buffer is empty or the network connection fails, an error message is returned
 */
std::expected<std::string, std::string> SscmaCamera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        return std::unexpected("Image explain URL or token is not set");
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    // Build the multipart/form-data request body
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    // Build the question field
    std::string question_field;
    question_field += "--" + boundary + "\r\n";
    question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
    question_field += "\r\n";
    question_field += question + "\r\n";

    // Build the file field header
    std::string file_header;
    file_header += "--" + boundary + "\r\n";
    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
    file_header += "Content-Type: image/jpeg\r\n";
    file_header += "\r\n";

    // Build the footer
    std::string multipart_footer;
    multipart_footer += "\r\n--" + boundary + "--\r\n";

    // Configure the HTTP client to use chunked transfer encoding
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (auto opened = http->Open("POST", explain_url_); !opened) {
        ESP_LOGE(TAG, "Failed to connect to explain URL: %s", opened.error().ToString().c_str());
        return std::unexpected("Failed to connect to explain URL");
    }

    auto write_or_fail = [&http](const char* data, size_t size) -> bool {
        if (auto written = http->Write(data, size); !written) {
            ESP_LOGE(TAG, "Failed to upload photo: %s", written.error().ToString().c_str());
            return true;
        }
        return false;
    };

    // Chunk 1: question field
    if (write_or_fail(question_field.c_str(), question_field.size())) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    // Chunk 2: file field header
    if (write_or_fail(file_header.c_str(), file_header.size())) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    // Chunk 3: JPEG data
    if (write_or_fail((const char*)jpeg_data_.buf, jpeg_data_.len)) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    // Chunk 4: multipart footer
    if (write_or_fail(multipart_footer.c_str(), multipart_footer.size())) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    // End chunk
    if (write_or_fail("", 0)) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        http->Close();
        return std::unexpected("Failed to upload photo");
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", *status_code);
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "Explain image size=%d, question=%s\n%s", jpeg_data_.len, question.c_str(),
             result.c_str());
    return result;
}
