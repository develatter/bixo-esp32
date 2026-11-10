#include "display/lv_display.h"
#include "misc/lv_event.h"
#include "wifi_board.h"
#include "sensecap_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "knob.h"
#include "config.h"
#include "led/single_led.h"
#include "power_save_timer.h"
#include "sscma_camera.h"
#include "lvgl_theme.h"
#include "wifi_manager.h"
#include "pet_ui.h"
#include "pet_background.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_check.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_spd2010.h>
#include <esp_lcd_touch_spd2010.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/spi_master.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <iot_button.h>
#include <iot_knob.h>
#include <esp_io_expander_tca95xx_16bit.h>
#include <esp_sleep.h>
#include <esp_console.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "assets/lang_config.h"

#define TAG "sensecap_watcher"

// Bixo's screen: only the pet, centred. No clock or status icons: the body colour fades to
// grey below half battery (the <emotion>_g1.._g3 sprites), an almost flat battery shows the
// "exhausted" face and a lost connection the "no_wifi" one. The state shows on his face too;
// only notifications and subtitles are text.
class CustomLcdDisplay : public SpiLcdDisplay {
    public:
        CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                        esp_lcd_panel_handle_t panel_handle,
                        int width,
                        int height,
                        int offset_x,
                        int offset_y,
                        bool mirror_x,
                        bool mirror_y,
                        bool swap_xy)
            : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
            // Note: UI customization should be done in SetupUI(), not in constructor
            // to ensure lvgl objects are created before accessing them
        }

        virtual void SetupUI() override {
            // Call parent SetupUI() first to create all lvgl objects
            SpiLcdDisplay::SetupUI();

            DisplayLockGuard lock(this);
            auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
            auto text_font = lvgl_theme->text_font()->font();

            lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_size(status_bar_, LV_HOR_RES, text_font->line_height);
            lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
            lv_obj_set_style_pad_top(status_bar_, 10, 0);
            lv_obj_set_style_pad_bottom(status_bar_, 1, 0);
            lv_obj_set_y(status_bar_, text_font->line_height);
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_IGNORE_LAYOUT);

            lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_flex_grow(status_label_, 0);
            lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
            lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

            lv_obj_align(notification_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
            lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

            // The sprites draw the body below the middle of the canvas (it sits on the ground).
            lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -kBodyOffset);

            // Move the bottom dialog for the round screen so the rounded corners don't clip it
            lv_obj_set_style_pad_bottom(bottom_bar_, 30, 0);
            lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.75); // limit the width so text doesn't touch the edge
            lv_obj_set_style_text_line_space(chat_message_label_, 6, 0);
            UsePixelFont();
            LoadBackground();
        }

        // The assets bring their own theme font: put the pixel font back on top of it, and the
        // background and its text colours over the theme's.
        virtual void SetTheme(Theme* theme) override {
            SpiLcdDisplay::SetTheme(theme);
            DisplayLockGuard lock(this);
            UsePixelFont();
            LoadBackground();
        }

        virtual void SetPowerSaveMode(bool on) override {
            SpiLcdDisplay::SetPowerSaveMode(on);
            DisplayLockGuard lock(this);
            background_.SetPaused(on);
        }

        // Hides Bixo and his subtitles, to see a background on its own.
        void SetPetHidden(bool hidden) {
            DisplayLockGuard lock(this);
            pet_hidden_ = hidden;
            UpdateShadow();
            bool subtitle = lv_label_get_text(chat_message_label_)[0] != '\0';
            for (lv_obj_t* obj : {emoji_box_, bottom_bar_, status_bar_}) {
                bool show = !hidden && (obj != bottom_bar_ || subtitle);  // an empty bar stays hidden
                show ? lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            }
        }

        std::vector<std::string> BackgroundLabels() {
            DisplayLockGuard lock(this);
            std::vector<std::string> labels;
            for (const auto& info : background_.list()) {
                labels.push_back(info.label);
            }
            return labels;
        }

        int BackgroundIndex() {
            DisplayLockGuard lock(this);
            return background_.current();
        }

        // Shows a background and remembers it.
        bool SetBackground(int index) {
            DisplayLockGuard lock(this);
            if (!ShowBackground(index)) {
                return false;
            }
            Settings settings("pet", true);
            settings.SetString("background", background_.list()[index].name);
            return true;
        }

        bool SetBackground(const std::string& name) {
            DisplayLockGuard lock(this);
            return SetBackground(background_.Find(name));
        }

        // Idle, connecting, listening, speaking and errors show no status text: Bixo's face says it.
        virtual void SetStatus(const char* status) override {
            bool error = strcmp(status, Lang::Strings::ERROR) == 0;
            bool silent = error || strcmp(status, Lang::Strings::STANDBY) == 0 ||
                          strcmp(status, Lang::Strings::CONNECTING) == 0 ||
                          strcmp(status, Lang::Strings::LISTENING) == 0 ||
                          strcmp(status, Lang::Strings::SPEAKING) == 0;
            connecting_ = strcmp(status, Lang::Strings::CONNECTING) == 0;
            listening_ = strcmp(status, Lang::Strings::LISTENING) == 0;
            error_ = error;
            if (error) {
                StartErrorTimer();
            }
            SpiLcdDisplay::SetStatus(silent ? "" : status);
        }

        bool ShowingError() const { return error_; }

        // Clears an error face and its message, also outside idle (where the app keeps them).
        void DismissError() {
            if (!error_) {
                return;
            }
            Application::GetInstance().DismissAlert();
            if (error_) {
                SetStatus(Lang::Strings::STANDBY);
                SetChatMessage("system", "");
                SetEmotion("neutral");
            }
        }

        virtual bool SetEmotion(const char* emotion) override {
            {
                std::lock_guard<std::mutex> lock(emotion_mutex_);
                requested_emotion_ = emotion;
            }
            return ShowEmotion();
        }

        // Called every second: battery and Wi-Fi show on Bixo himself, not as icons.
        virtual void UpdateStatusBar(bool update_all = false) override {
            auto& board = Board::GetInstance();
            int level;
            bool charging, discharging;
            int tier = battery_tier_;
            bool exhausted = exhausted_;
            esp_pm_lock_acquire(pm_lock_);
            bool have_level = board.GetBatteryLevel(level, charging, discharging);
            esp_pm_lock_release(pm_lock_);
            if (have_level) {
                tier = BatteryTier(level);
                if (tier < battery_tier_ && BatteryTier(level - kHysteresis) != tier) {
                    tier = battery_tier_;  // not clearly above the boundary yet
                }
                exhausted = discharging && level <= (exhausted_ ? kExhaustedLevel + kHysteresis : kExhaustedLevel);
            }
            auto& wifi = WifiManager::GetInstance();
            bool offline = !wifi.IsConnected() || wifi.IsConfigMode();

            if (update_all || tier != battery_tier_ || exhausted != exhausted_ || offline != offline_) {
                if (tier != battery_tier_ || exhausted != exhausted_ || offline != offline_) {
                    ESP_LOGI(TAG, "Look: battery tier %d, exhausted %d, offline %d", tier, exhausted, offline);
                }
                battery_tier_ = tier;
                exhausted_ = exhausted;
                offline_ = offline;
                ShowEmotion();
            }
        }

    private:
        // Every text on screen inherits it from the screen object.
        void UsePixelFont() {
            lv_obj_set_style_text_font(lv_screen_active(), PixelFont(this), 0);
        }

        PetBackground background_;

        // The saved background, or the first one.
        void LoadBackground() {
            if (container_ == nullptr) {
                return;
            }
            background_.Load(container_);
            Settings settings("pet");
            int index = background_.Find(settings.GetString("background"));
            ShowBackground(index >= 0 ? index : 0);
            UpdateShadow();
        }

        bool ShowBackground(int index) {
            if (!background_.Show(index)) {
                return false;
            }
            const auto& info = background_.list()[index];
            lv_obj_set_style_bg_color(bottom_bar_, info.panel, 0);
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_60, 0);
            lv_obj_set_style_text_color(chat_message_label_, info.text, 0);
            lv_obj_set_style_text_color(status_label_, info.text, 0);
            lv_obj_set_style_text_color(notification_label_, info.text, 0);
            return true;
        }

        static constexpr int kBodyOffset = 39;      // px from the canvas centre to the body's
        static constexpr int kExhaustedLevel = 10;  // % (discharging) for the exhausted face
        static constexpr int kHysteresis = 3;       // % to climb back past a boundary
        static constexpr const char* kTierSuffix[] = {"", "_g1", "_g2", "_g3"};

        std::mutex emotion_mutex_;
        std::string requested_emotion_ = "neutral";
        std::string shown_emotion_;  // only for the log
        bool sprite_shown_ = false;
        bool pet_hidden_ = false;
        int battery_tier_ = 0;
        bool exhausted_ = false;
        bool offline_ = false;

        static constexpr int64_t kErrorShownUs = 8 * 1000 * 1000;
        std::atomic<bool> connecting_{false};
        std::atomic<bool> listening_{false};
        std::atomic<bool> error_{false};
        esp_timer_handle_t error_timer_ = nullptr;

        void StartErrorTimer() {
            if (error_timer_ == nullptr) {
                esp_timer_create_args_t args = {
                    .callback = [](void* arg) {
                        auto self = static_cast<CustomLcdDisplay*>(arg);
                        Application::GetInstance().Schedule([self]() { self->DismissError(); });
                    },
                    .arg = this,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "error_face",
                    .skip_unhandled_events = true,
                };
                ESP_ERROR_CHECK(esp_timer_create(&args, &error_timer_));
            }
            esp_timer_stop(error_timer_);
            esp_timer_start_once(error_timer_, kErrorShownUs);
        }

        static int BatteryTier(int level) {
            return level >= 50 ? 0 : level >= 35 ? 1 : level >= 20 ? 2 : 3;
        }

        bool ShowEmotion() {
            std::string emotion;
            {
                std::lock_guard<std::mutex> lock(emotion_mutex_);
                emotion = requested_emotion_;
            }
            if (offline_ && Application::GetInstance().GetDeviceState() != kDeviceStateStarting) {
                emotion = "no_wifi";
            } else if (error_) {
                emotion = "error";
            } else if (connecting_ && emotion == "neutral") {
                emotion = "connecting";
            } else if (listening_ && emotion == "neutral") {
                emotion = "listening";
            } else if (exhausted_ && (emotion == "neutral" || emotion == "sleepy")) {
                // Only the resting faces: a tap or a reply still shows, just very grey.
                emotion = "exhausted";
            }
            if (emotion != shown_emotion_) {
                ESP_LOGI(TAG, "Face: %s", emotion.c_str());
                shown_emotion_ = emotion;
            }
            // System icons (link, download...) have no grey sprites: they show as they are.
            std::string tinted = emotion + kTierSuffix[battery_tier_];
            bool shown = (battery_tier_ > 0 && SpiLcdDisplay::SetEmotion(tinted.c_str())) ||
                         SpiLcdDisplay::SetEmotion(emotion.c_str());
            DisplayLockGuard lock(this);
            sprite_shown_ = shown && !lv_obj_has_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            UpdateShadow();
            return shown;
        }

        // The shadow belongs to Bixo's sprite: not under a system icon (while starting up, say),
        // nor while he is hidden. Call with the display locked.
        void UpdateShadow() {
            background_.SetShadow(sprite_shown_ && !pet_hidden_);
        }
};

class SensecapWatcher : public WifiBoard {
private:
    static constexpr uint32_t kVolumeColor = 0x3DDC84;
    static constexpr int kVolumeStep = 5;
    static constexpr int64_t kKnobReverseUs = 120 * 1000;
    static constexpr int64_t kLongPressUs = 700 * 1000;
    // Sent as if it were a wake word; the server starts a new conversation with Hermes.
    static constexpr const char* kNewChatCommand = "<nueva charla>";

    i2c_master_bus_handle_t i2c_bus_;
    CustomLcdDisplay* display_;
    std::unique_ptr<Knob> knob_;
    int knob_direction_ = 0;
    bool knob_reverse_pending_ = false;
    int64_t knob_last_us_ = 0;
    std::atomic<int> knob_steps_{0};
    std::atomic<bool> knob_scheduled_{false};
    std::atomic<bool> wifi_reconnecting_{false};
    esp_io_expander_handle_t io_exp_handle;
    button_handle_t btns;
    PowerSaveTimer* power_save_timer_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    uint32_t long_press_cnt_;
    button_driver_t* btn_driver_ = nullptr;
    static SensecapWatcher* instance_;
    SscmaCamera* camera_ = nullptr;
    i2c_master_bus_handle_t touch_i2c_bus_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    esp_timer_handle_t touch_reaction_timer_ = nullptr;
    int64_t last_tap_us_ = 0;
    int tap_streak_ = 0;
    std::unique_ptr<LevelRing> ring_;
    std::unique_ptr<PetMenu> menu_;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            bool is_charging = (IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0);
            if (is_charging) {
                ESP_LOGI(TAG, "charging");
                // Serialize with battery polling on the main task. USB may have
                // disconnected by the time the request is handled.
                Application::GetInstance().Schedule([this]() {
                    power_save_timer_->SetEnabled(false);
                    if (IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0) {
                        GetBacklight()->SetBrightness(0);
                    } else {
                        power_save_timer_->SetEnabled(true);
                    }
                });
            } else {
                IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);
            }
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = BSP_GENERAL_I2C_SDA,
            .scl_io_num = BSP_GENERAL_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // pulldown for lcd i2c
        const gpio_config_t io_config = {
            .pin_bit_mask = (1ULL << BSP_TOUCH_I2C_SDA) | (1ULL << BSP_TOUCH_I2C_SCL) | (1ULL << BSP_SPI3_HOST_PCLK) | (1ULL << BSP_SPI3_HOST_DATA0) | (1ULL << BSP_SPI3_HOST_DATA1)
                            | (1ULL << BSP_SPI3_HOST_DATA2) | (1ULL << BSP_SPI3_HOST_DATA3) | (1ULL << BSP_LCD_SPI_CS) | (1UL << DISPLAY_BACKLIGHT_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_config);

        gpio_set_level(BSP_TOUCH_I2C_SDA, 0);
        gpio_set_level(BSP_TOUCH_I2C_SCL, 0);
    
        gpio_set_level(BSP_LCD_SPI_CS, 0);
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
        gpio_set_level(BSP_SPI3_HOST_PCLK, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA0, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA1, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA2, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA3, 0);

    }

    esp_err_t IoExpanderSetLevel(uint16_t pin_mask, uint8_t level) {
        return esp_io_expander_set_level(io_exp_handle, pin_mask, level);
    }

    uint8_t IoExpanderGetLevel(uint16_t pin_mask) {
        uint32_t pin_val = 0;
        esp_io_expander_get_level(io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
        pin_mask &= DRV_IO_EXP_INPUT_MASK;
        return (uint8_t)((pin_val & pin_mask) ? 1 : 0);
    }

    void InitializeExpander() {
        esp_err_t ret = ESP_OK;
        esp_io_expander_new_i2c_tca95xx_16bit(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_001, &io_exp_handle);

        ret |= esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);
        ret |= esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, IO_EXPANDER_OUTPUT);
        ret |= esp_io_expander_set_level(io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, 0);
        ret |= esp_io_expander_set_level(io_exp_handle, BSP_PWR_SYSTEM, 1);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        ret |= esp_io_expander_set_level(io_exp_handle, BSP_PWR_START_UP, 1);
        vTaskDelay(50 / portTICK_PERIOD_MS);
    
        uint32_t pin_val = 0;
        ret |= esp_io_expander_get_level(io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
        ESP_LOGI(TAG, "IO expander initialized: %x", DRV_IO_EXP_OUTPUT_MASK | (uint16_t)pin_val);
    
        assert(ret == ESP_OK);
    }

    // Runs in the knob's polling timer, which decodes the encoder in software: keep it short
    // or steps get lost and read backwards. Steps are added up and applied in the main task.
    void OnKnobRotate(bool clockwise) {
        int step = clockwise ? -1 : 1;  // +1 turns the volume up and moves up the menu
        int64_t now = esp_timer_get_time();
        // A lone step against the way the wheel is turning is a decoding glitch; two in a row
        // are the wheel really turning back.
        bool reversing = step != knob_direction_ && now - knob_last_us_ < kKnobReverseUs;
        knob_last_us_ = now;
        if (reversing && !knob_reverse_pending_) {
            knob_reverse_pending_ = true;
            return;
        }
        knob_reverse_pending_ = false;
        knob_direction_ = step;
        knob_steps_ += step;
        if (!knob_scheduled_.exchange(true)) {
            Application::GetInstance().Schedule([this]() { ApplyKnobSteps(); });
        }
    }

    void ApplyKnobSteps() {
        knob_scheduled_ = false;
        int steps = knob_steps_.exchange(0);
        if (steps == 0) {
            return;
        }
        power_save_timer_->WakeUp();
        if (MenuOpen()) {
            menu_->Move(steps);
            return;
        }
        auto codec = GetAudioCodec();
        int volume = std::clamp(codec->output_volume() + steps * kVolumeStep, 0, 100);
        codec->SetOutputVolume(volume);
        if (ring_) {
            ring_->Show(codec->output_volume(), kVolumeColor);
        }
    }

    void InitializeKnob() {
        knob_ = std::make_unique<Knob>(BSP_KNOB_A_PIN, BSP_KNOB_B_PIN);
        knob_->OnRotate([this](bool clockwise) {
            ESP_LOGD(TAG, "Knob rotation detected. Clockwise:%s", clockwise ? "true" : "false");
            OnKnobRotate(clockwise);
        });
        ESP_LOGI(TAG, "Knob initialized with pins A:%d B:%d", BSP_KNOB_A_PIN, BSP_KNOB_B_PIN);
    }

    void InitializeButton() {
        // Set the static instance pointer
        instance_ = this;
        
        // The watcher powers on by long-pressing the wheel; wait for the wheel to be released, otherwise letting go after power-on may be misread as a click
        ESP_LOGI(TAG, "waiting for knob button release");
        while(IoExpanderGetLevel(BSP_KNOB_BTN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        button_config_t btn_config = {
            .long_press_time = 2000,
            .short_press_time = 0
        };
        btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        btn_driver_->enable_power_save = false;
        btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            return !instance_->IoExpanderGetLevel(BSP_KNOB_BTN);
        };
        
        ESP_ERROR_CHECK(iot_button_create(&btn_config, btn_driver_, &btns));
        
        iot_button_register_cb(btns, BUTTON_SINGLE_CLICK, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<SensecapWatcher*>(usr_data);
            self->power_save_timer_->WakeUp();
            if (self->MenuOpen()) {
                Application::GetInstance().Schedule([self]() { self->menu_->Select(); });
                return;
            }

            if (self->display_->ShowingError()) {
                Application::GetInstance().Schedule([self]() { self->display_->DismissError(); });
                return;
            }

            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateStarting) {
                self->EnterWifiConfigMode();
                return;
            }
            if (self->Offline()) {
                self->ReconnectWifi();
                return;
            }
            // Push to talk: one click starts listening (interrupting a reply), the next one sends.
            if (state == kDeviceStateListening) {
                app.StopListening();
            } else if (state == kDeviceStateIdle || state == kDeviceStateSpeaking) {
                app.StartListening();
            } else {
                app.ToggleChatState();
            }
        }, this);

        // Double click opens a conversation (listening) or hangs up the current one.
        iot_button_register_cb(btns, BUTTON_DOUBLE_CLICK, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<SensecapWatcher*>(usr_data);
            self->power_save_timer_->WakeUp();
            if (self->MenuOpen()) {
                Application::GetInstance().Schedule([self]() { self->menu_->Close(); });
                return;
            }
            if (self->display_->ShowingError()) {
                Application::GetInstance().Schedule([self]() { self->display_->DismissError(); });
                return;
            }
            if (self->Offline() && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                self->ReconnectWifi();
                return;
            }
            Application::GetInstance().ToggleConversation();
        }, this);
        
        iot_button_register_cb(btns, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<SensecapWatcher*>(usr_data);
            bool is_charging = (self->IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0);
            self->long_press_cnt_ = 0;
            if (is_charging) {
                ESP_LOGI(TAG, "charging");
            } else {
                self->IoExpanderSetLevel(BSP_PWR_LCD, 0);
                self->IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);
            }
        }, this);

        iot_button_register_cb(btns, BUTTON_LONG_PRESS_HOLD, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<SensecapWatcher*>(usr_data);
            self->long_press_cnt_++; // incremented every 20ms
            // Long press for 10s to factory reset: 2+0.02*400 = 10
            if (self->long_press_cnt_ > 400) {
                ESP_LOGI(TAG, "Factory reset");
                nvs_flash_erase();
                esp_restart();
            }
        }, this);
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SSCMA SPI bus");
        spi_bus_config_t spi_cfg = {0};

        spi_cfg.mosi_io_num = BSP_SPI2_HOST_MOSI;
        spi_cfg.miso_io_num = BSP_SPI2_HOST_MISO;
        spi_cfg.sclk_io_num = BSP_SPI2_HOST_SCLK;
        spi_cfg.quadwp_io_num = -1;
        spi_cfg.quadhd_io_num = -1;
        spi_cfg.isr_cpu_id = ESP_INTR_CPU_AFFINITY_1;
        spi_cfg.max_transfer_sz = 4095;
   
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &spi_cfg, SPI_DMA_CH_AUTO));

        ESP_LOGI(TAG, "Initialize QSPI bus");

        spi_bus_config_t qspi_cfg = {0};
        qspi_cfg.sclk_io_num = BSP_SPI3_HOST_PCLK;
        qspi_cfg.data0_io_num = BSP_SPI3_HOST_DATA0;
        qspi_cfg.data1_io_num = BSP_SPI3_HOST_DATA1;
        qspi_cfg.data2_io_num = BSP_SPI3_HOST_DATA2;
        qspi_cfg.data3_io_num = BSP_SPI3_HOST_DATA3;
        qspi_cfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * DRV_LCD_BITS_PER_PIXEL / 8 / CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV;
    
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &qspi_cfg, SPI_DMA_CH_AUTO));
    }

    void Initializespd2010Display() {
        ESP_LOGI(TAG, "Install panel IO");
        const esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = BSP_LCD_SPI_CS,
            .dc_gpio_num = GPIO_NUM_NC,
            .spi_mode = 3,
            .pclk_hz = DRV_LCD_PIXEL_CLK_HZ,
            .trans_queue_depth = 2,
            .lcd_cmd_bits = DRV_LCD_CMD_BITS,
            .lcd_param_bits = DRV_LCD_PARAM_BITS,
            .flags = {
                .quad_mode = true,
            },
        };
        spd2010_vendor_config_t vendor_config = {
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &panel_io_);
    
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = DRV_LCD_RGB_ELEMENT_ORDER;
        panel_config.bits_per_pixel = DRV_LCD_BITS_PER_PIXEL;
        panel_config.reset_gpio_num = BSP_LCD_GPIO_RST; // Shared with Touch reset
        panel_config.vendor_config = &vendor_config;
        esp_lcd_new_panel_spd2010(panel_io_, &panel_config, &panel_);

        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel_, true);

        display_ = new CustomLcdDisplay(panel_io_, panel_,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        
        // Make the start column index of each refresh a multiple of 4, and the total column count a multiple of 4, as the SPD2010 requires
        lv_display_add_event_cb(lv_display_get_default(), [](lv_event_t *e) {
            lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
            uint16_t x1 = area->x1;
            uint16_t x2 = area->x2;
            // round the start of area down to the nearest 4N number
            area->x1 = (x1 >> 2) << 2;
            // round the end of area up to the nearest 4M+3 number
            area->x2 = ((x2 >> 2) << 2) + 3;
        }, LV_EVENT_INVALIDATE_AREA, NULL);
        
    }

    uint16_t BatterygetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);
    
            adc_oneshot_chan_cfg_t ch_config = {
                .atten = BSP_BAT_ADC_ATTEN,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            adc_oneshot_config_channel(adc_handle, BSP_BAT_ADC_CHAN, &ch_config);
    
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .chan = BSP_BAT_ADC_CHAN,
                .atten = BSP_BAT_ADC_ATTEN,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }
        if (initialized) {
            int raw_value = 0;
            int voltage = 0; // mV
            adc_oneshot_read(adc_handle, BSP_BAT_ADC_CHAN, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &voltage);
            voltage = voltage * 82 / 20;
            // ESP_LOGI(TAG, "voltage: %dmV", voltage);
            return (uint16_t)voltage;
        }
        return 0;
    }

    uint8_t BatterygetPercent(bool print = false) {
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatterygetVoltage();
        }
        voltage /= 10;
        int percent = (-1 * voltage * voltage + 9016 * voltage - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        if (print) {
            printf("voltage: %dmV, percentage: %d%%\r\n", voltage, percent);
        }
        return (uint8_t)percent;
    }

    void InitializeCmd() {
        esp_console_repl_t *repl = NULL;
        esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
        repl_config.max_cmdline_length = 1024;
        repl_config.prompt = "SenseCAP>";
        
        const esp_console_cmd_t cmd1 = {
            .command = "reboot",
            .help = "reboot the device",
            .hint = nullptr,
            .func = [](int argc, char** argv) -> int {
                esp_restart();
                return 0;
            },
            .argtable = nullptr
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd1));

        const esp_console_cmd_t cmd2 = {
            .command = "shutdown",
            .help = "shutdown the device",
            .hint = nullptr,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                auto self = static_cast<SensecapWatcher*>(context);
                self->GetBacklight()->SetBrightness(0);
                self->IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd2));

        const esp_console_cmd_t cmd3 = {
            .command = "battery",
            .help = "get battery percent",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                auto self = static_cast<SensecapWatcher*>(context);
                self->BatterygetPercent(true);
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd3));

        const esp_console_cmd_t cmd4 = {
            .command = "factory_reset",
            .help = "factory reset and reboot the device",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                nvs_flash_erase();
                esp_restart();
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd4));

        const esp_console_cmd_t cmd5 = {
            .command = "read_mac",
            .help = "Read mac address",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                uint8_t mac[6];
                esp_read_mac(mac, ESP_MAC_WIFI_STA);
                printf("wifi_sta_mac: " MACSTR "\n", MAC2STR(mac));
                esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
                printf("wifi_softap_mac: " MACSTR "\n", MAC2STR(mac));
                esp_read_mac(mac, ESP_MAC_BT);
                printf("bt_mac: " MACSTR "\n", MAC2STR(mac));
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd5));

        const esp_console_cmd_t cmd6 = {
            .command = "version",
            .help = "Read version info",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                auto self = static_cast<SensecapWatcher*>(context);
                auto app_desc = esp_app_get_description();
                const char* region = "UNKNOWN";
                #if defined(CONFIG_LANGUAGE_ZH_CN)
                    region = "CN";
                #elif defined(CONFIG_LANGUAGE_EN_US)
                    region = "US";
                #elif defined(CONFIG_LANGUAGE_JA_JP)
                    region = "JP";
                #elif defined(CONFIG_LANGUAGE_ES_ES)
                    region = "ES";
                #elif defined(CONFIG_LANGUAGE_DE_DE)
                    region = "DE";
                #elif defined(CONFIG_LANGUAGE_FR_FR)
                    region = "FR";
                #elif defined(CONFIG_LANGUAGE_IT_IT)
                    region = "IT";
                #elif defined(CONFIG_LANGUAGE_PT_PT)
                    region = "PT";
                #elif defined(CONFIG_LANGUAGE_RU_RU)
                    region = "RU";
                #elif defined(CONFIG_LANGUAGE_KO_KR)
                    region = "KR";
                #endif
                printf("{\"type\":0,\"name\":\"VER?\",\"code\":0,\"data\":{\"software\":\"%s\",\"hardware\":\"watcher xiaozhi agent\",\"camera\":%d,\"region\":\"%s\"}}\n",
                       app_desc->version,
                       self->GetCamera() == nullptr ? 0 : 1,
                       region);
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd6));

        esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
        ESP_ERROR_CHECK(esp_console_start_repl(repl));
    }

    void InitializeCamera() {

        ESP_LOGI(TAG, "Initialize Camera");

        // !!!NOTE: SD Card use same SPI bus as sscma client, so we need to disable SD card CS pin first
        const gpio_config_t io_config = {
            .pin_bit_mask = (1ULL << BSP_SD_SPI_CS),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t ret = gpio_config(&io_config);
        if (ret != ESP_OK)
            return;

        gpio_set_level(BSP_SD_SPI_CS, 1);

        camera_ = new SscmaCamera(io_exp_handle);
    }


    // The SPD2010 panel has its own touch controller on a second I2C bus. It is polled
    // (its interrupt line sits behind the IO expander) and only drives Bixo's reactions.
    void InitializeTouch() {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = (i2c_port_t)BSP_TOUCH_I2C_NUM,
            .sda_io_num = BSP_TOUCH_I2C_SDA,
            .scl_io_num = BSP_TOUCH_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        if (i2c_new_master_bus(&bus_cfg, &touch_i2c_bus_) != ESP_OK) {
            ESP_LOGE(TAG, "Touch I2C bus init failed");
            return;
        }
        esp_lcd_panel_io_handle_t io = nullptr;
        // Same values as ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG(), whose designators are out of
        // declaration order for C++.
        esp_lcd_panel_io_i2c_config_t io_cfg = {};
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_SPD2010_ADDRESS;
        io_cfg.control_phase_bytes = 1;
        io_cfg.flags.disable_control_phase = 1;
        io_cfg.scl_speed_hz = 100000;
        if (esp_lcd_new_panel_io_i2c(touch_i2c_bus_, &io_cfg, &io) != ESP_OK) {
            ESP_LOGE(TAG, "Touch panel IO init failed");
            return;
        }
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC,  // shared with the LCD reset
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
        };
        if (esp_lcd_touch_new_i2c_spd2010(io, &tp_cfg, &touch_) != ESP_OK) {
            ESP_LOGE(TAG, "Touch controller init failed");
            touch_ = nullptr;
            return;
        }

        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                Application::GetInstance().Schedule([]() {
                    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                        Board::GetInstance().GetDisplay()->SetEmotion("neutral");
                    }
                });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touch_reaction",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &touch_reaction_timer_));

        // A short touch is a tap (on release); holding it opens the menu.
        xTaskCreate([](void* arg) {
            auto self = static_cast<SensecapWatcher*>(arg);
            int64_t pressed_since = 0;  // 0 = not touching
            bool held = false;
            esp_lcd_touch_point_data_t point = {};
            while (true) {
                esp_lcd_touch_point_data_t now_point;
                uint8_t count = 0;
                if (esp_lcd_touch_read_data(self->touch_) == ESP_OK) {
                    esp_lcd_touch_get_data(self->touch_, &now_point, &count, 1);
                }
                int64_t now = esp_timer_get_time();
                if (count > 0) {
                    if (pressed_since == 0) {
                        pressed_since = now;
                        held = false;
                    }
                    point = now_point;
                    if (!held && now - pressed_since >= kLongPressUs) {
                        held = true;
                        self->OnLongPress();
                    }
                } else if (pressed_since != 0) {
                    if (!held) {
                        self->OnTap(point.x, point.y);
                    }
                    pressed_since = 0;
                }
                vTaskDelay(pdMS_TO_TICKS(40));
            }
        }, "touch", 3072, this, 2, nullptr);
        ESP_LOGI(TAG, "Touch initialized");
    }

    // A tap pets Bixo while it's idle: happy, then loving, then giggling if you keep at it.
    void OnTap(uint16_t x, uint16_t y) {
        if (MenuOpen()) {
            power_save_timer_->WakeUp();
            Application::GetInstance().Schedule([this, y]() { menu_->Tap(y); });
            return;
        }
        if (display_->ShowingError()) {
            power_save_timer_->WakeUp();
            Application::GetInstance().Schedule([this]() { display_->DismissError(); });
            return;
        }
        int64_t now = esp_timer_get_time();
        tap_streak_ = (now - last_tap_us_ < 1500 * 1000) ? tap_streak_ + 1 : 0;
        last_tap_us_ = now;
        ESP_LOGI(TAG, "Tap at %u,%u (streak %d)", x, y, tap_streak_);
        power_save_timer_->WakeUp();
        static const char* const kReactions[] = {"happy", "loving", "laughing"};
        const char* emotion = kReactions[tap_streak_ < 2 ? tap_streak_ : 2];
        Application::GetInstance().Schedule([this, emotion]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateIdle) {
                return;
            }
            GetDisplay()->SetEmotion(emotion);
            if (tap_streak_ == 0) {
                app.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            esp_timer_stop(touch_reaction_timer_);
            esp_timer_start_once(touch_reaction_timer_, 2500 * 1000);
        });
    }

    // Knob and button work before the menu exists (they are set up first).
    bool MenuOpen() const { return menu_ && menu_->IsOpen(); }

    void OnLongPress() {
        ESP_LOGI(TAG, "Long press");
        power_save_timer_->WakeUp();
        Application::GetInstance().Schedule([this]() {
            if (!menu_->IsOpen()) {
                // The timer that would end a tap reaction is stopped: end it now.
                esp_timer_stop(touch_reaction_timer_);
                if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    display_->SetEmotion("neutral");
                }
                menu_->Open();
            }
        });
    }

    bool Offline() const {
        auto& wifi = WifiManager::GetInstance();
        return !wifi.IsConnected() && !wifi.IsConfigMode();
    }

    // Starts a fresh scan now instead of waiting for the next retry.
    void ReconnectWifi() {
        if (WifiManager::GetInstance().IsConfigMode() || wifi_reconnecting_.exchange(true)) {
            return;
        }
        ESP_LOGI(TAG, "Reconnecting Wi-Fi");
        xTaskCreate([](void* arg) {
            auto self = static_cast<SensecapWatcher*>(arg);
            auto& wifi = WifiManager::GetInstance();
            wifi.StopStation();
            wifi.StartStation();
            self->wifi_reconnecting_ = false;
            vTaskDelete(nullptr);
        }, "wifi_reconnect", 4096, this, 2, nullptr);
    }

    void PowerOff() {
        ESP_LOGI(TAG, "Power off from the menu");
        IoExpanderSetLevel(BSP_PWR_LCD, 0);
        IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);  // with the charger plugged in it stays on
    }

    std::string NetworkInfo() {
        auto& wifi = WifiManager::GetInstance();
        if (!wifi.IsConnected()) {
            return "Sin conexión";
        }
        return "Red: " + wifi.GetSsid() + "\nIP: " + wifi.GetIpAddress() +
               "\nSeñal: " + std::to_string(wifi.GetRssi()) + " dBm";
    }

    std::string StatusInfo() {
        int level = 0;
        bool charging = false, discharging = false;
        GetBatteryLevel(level, charging, discharging);
        std::string text = "Batería: " + std::to_string(level) + " %";
        if (charging) {
            text += " (cargando)";
        }
        return text + "\nVersión: " + esp_app_get_description()->version;
    }

    void InitializeMenu() {
        ring_ = std::make_unique<LevelRing>(display_);
        auto backlight = GetBacklight();
        std::vector<PetMenuItem> items = {
            {.kind = PetMenuItem::kAdjust, .label = "Brillo",
             .get = [backlight]() { return (int)backlight->brightness(); },
             .set = [backlight](int value) { backlight->SetBrightness(value, true); }},
            {.kind = PetMenuItem::kChoose, .label = "Fondo",
             .get = [this]() { return display_->BackgroundIndex(); },
             .set = [this](int index) { display_->SetBackground(index); },
             .options = [this]() { return display_->BackgroundLabels(); },
             .choosing = [this](bool on) { display_->SetPetHidden(on); }},
            {.kind = PetMenuItem::kConfirm, .label = "Nueva charla",
             .action = []() { Application::GetInstance().WakeWordInvoke(kNewChatCommand); }},
            {.kind = PetMenuItem::kInfo, .label = "Wi-Fi", .info = [this]() { return NetworkInfo(); }},
            {.kind = PetMenuItem::kAction, .label = "Reconectar wifi", .action = [this]() { ReconnectWifi(); }},
            {.kind = PetMenuItem::kConfirm, .label = "Cambiar wifi", .action = [this]() { EnterWifiConfigMode(); }},
            {.kind = PetMenuItem::kInfo, .label = "Estado", .info = [this]() { return StatusInfo(); }},
            {.kind = PetMenuItem::kConfirm, .label = "Reiniciar",
             .action = []() { Application::GetInstance().Reboot(); }},
            {.kind = PetMenuItem::kConfirm, .label = "Apagar", .action = [this]() { PowerOff(); }},
            {.kind = PetMenuItem::kAction, .label = "Salir", .action = []() {}},
        };
        menu_ = std::make_unique<PetMenu>(display_, ring_.get(), std::move(items));
    }

    void InitializeTools() {
        McpServer::GetInstance().AddTool("self.screen.set_background",
            "Change the background behind Bixo on the screen, by name.",
            PropertyList({Property("name", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ToolResult {
                auto name = properties["name"].value<std::string>();
                if (!display_->SetBackground(name)) {
                    return std::unexpected("Unknown background: " + name);
                }
                return true;
            });
    }

public:
    SensecapWatcher() {
        ESP_LOGI(TAG, "Initialize Sensecap Watcher");
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeSpi();
        InitializeExpander();
        InitializeCmd();  //used for factory production testing
        InitializeButton();
        InitializeKnob();
        Initializespd2010Display();
        InitializeMenu();
        InitializeTools();
        InitializeTouch();
        GetBacklight()->RestoreBrightness();  // On versions without a camera, InitializeCamera takes 3s, so restore the backlight brightness first
        InitializeCamera();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static SensecapAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7243E_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    // Per https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher/blob/main/Hardware/SenseCAP_Watcher_v1.0_SCH.pdf
    // The RGB LED is a ws2813 mini on GPIO 40, powered at 3.3v, with the BIN backup signal line not connected
    // so it is directly compatible with the ws2812 driver used by SingleLED
    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = (IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0);
        discharging = !charging;
        level = (int)BatterygetPercent(false);

        // Always synchronize, including the first snapshot and a short USB
        // connection observed only by the shutdown callback. Re-enabling is a no-op.
        power_save_timer_->SetEnabled(discharging);
        if (level <= 1  &&  discharging) {
            ESP_LOGI(TAG, "Battery level is low, shutting down");
            IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);
        }
        return true;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(SensecapWatcher);

// Define the static member variable
SensecapWatcher* SensecapWatcher::instance_ = nullptr;
