#include "pet_background.h"

#include "assets.h"
#include "lvgl_theme.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <cstring>

#define TAG "pet_background"

namespace {

// bg_<name>.bin: "BXBG", version, cell size, origin (int8, px of cell 0), cols, rows, a pad
// byte, frames and ms per frame (uint16 LE), the shadow's row, first and end column and RGB,
// then the cells. Made by sprites/backgrounds.py.
constexpr size_t kHeaderSize = 20;
constexpr uint8_t kVersion = 2;

uint16_t ReadU16(const uint8_t* p) { return p[0] | (p[1] << 8); }

}  // namespace

PetBackground::~PetBackground() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
    }
    if (image_ != nullptr) {
        lv_obj_delete(image_);
    }
    heap_caps_free(pixels_);
}

void PetBackground::Load(lv_obj_t* parent) {
    list_.clear();
    void* ptr = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData("index.json", ptr, size)) {
        return;
    }
    cJSON* root = cJSON_ParseWithLength(static_cast<const char*>(ptr), size);
    cJSON* backgrounds = cJSON_GetObjectItem(root, "backgrounds");
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, backgrounds) {
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* label = cJSON_GetObjectItem(item, "label");
        cJSON* file = cJSON_GetObjectItem(item, "file");
        cJSON* text = cJSON_GetObjectItem(item, "text");
        cJSON* panel = cJSON_GetObjectItem(item, "panel");
        if (cJSON_IsString(name) && cJSON_IsString(label) && cJSON_IsString(file) &&
            cJSON_IsString(text) && cJSON_IsString(panel)) {
            list_.push_back({name->valuestring, label->valuestring, file->valuestring,
                             LvglTheme::ParseColor(text->valuestring),
                             LvglTheme::ParseColor(panel->valuestring)});
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%u backgrounds", (unsigned)list_.size());

    if (image_ == nullptr) {
        image_ = lv_image_create(parent);
        lv_obj_set_pos(image_, 0, 0);
        lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    }
}

int PetBackground::Find(const std::string& name) const {
    for (size_t i = 0; i < list_.size(); i++) {
        if (list_[i].name == name) {
            return i;
        }
    }
    return -1;
}

bool PetBackground::Show(int index) {
    if (index < 0 || index >= (int)list_.size() || image_ == nullptr) {
        return false;
    }
    if (index == current_) {
        return true;
    }
    const Info& info = list_[index];
    void* ptr = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(info.file, ptr, size) || size < kHeaderSize) {
        ESP_LOGE(TAG, "%s is missing", info.file.c_str());
        return false;
    }
    auto data = static_cast<const uint8_t*>(ptr);
    Header header = {
        .cell = data[5],
        .origin = (int8_t)data[6],
        .cols = data[7],
        .rows = data[8],
        .frames = ReadU16(data + 10),
        .ms = ReadU16(data + 12),
        .shadow_row = data[14],
        .shadow_first = data[15],
        .shadow_end = data[16],
        .shadow_rgb = {data[17], data[18], data[19]},
    };
    if (memcmp(data, "BXBG", 4) != 0 || data[4] != kVersion || header.frames == 0 ||
        size != kHeaderSize + (size_t)header.frames * header.rows * header.cols * 3) {
        ESP_LOGE(TAG, "%s is not a background", info.file.c_str());
        return false;
    }

    if (pixels_ == nullptr) {
        int width = lv_display_get_horizontal_resolution(nullptr);
        int height = lv_display_get_vertical_resolution(nullptr);
        size_t bytes = width * height * sizeof(uint16_t);
        pixels_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (pixels_ == nullptr) {
            ESP_LOGE(TAG, "No memory for the background");
            return false;
        }
        dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc_.header.w = width;
        dsc_.header.h = height;
        dsc_.header.stride = width * sizeof(uint16_t);
        dsc_.data = reinterpret_cast<const uint8_t*>(pixels_);
        dsc_.data_size = bytes;
        lv_image_set_src(image_, &dsc_);
    }

    header_ = header;
    cells_ = data + kHeaderSize;
    current_ = index;
    frame_ = 0;
    Paint(0, -1);
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(image_, 0);

    if (header_.frames > 1) {
        if (timer_ == nullptr) {
            timer_ = lv_timer_create(
                [](lv_timer_t* timer) {
                    static_cast<PetBackground*>(lv_timer_get_user_data(timer))->NextFrame();
                },
                header_.ms, this);
        }
        lv_timer_set_period(timer_, header_.ms);
        paused_ ? lv_timer_pause(timer_) : lv_timer_resume(timer_);
    } else if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
    ESP_LOGI(TAG, "Background %s, %d frames", info.name.c_str(), header_.frames);
    return true;
}

void PetBackground::SetPaused(bool paused) {
    paused_ = paused;
    if (timer_ != nullptr && header_.frames > 1) {
        paused ? lv_timer_pause(timer_) : lv_timer_resume(timer_);
    }
}

void PetBackground::SetShadow(bool shown) {
    if (shown == shadow_) {
        return;
    }
    shadow_ = shown;
    if (current_ >= 0) {
        Paint(frame_, -1);
    }
}

const uint8_t* PetBackground::Cell(int frame, int row, int col) const {
    if (shadow_ && row == header_.shadow_row && col >= header_.shadow_first && col < header_.shadow_end) {
        return header_.shadow_rgb;
    }
    return cells_ + (((size_t)frame * header_.rows + row) * header_.cols + col) * 3;
}

void PetBackground::NextFrame() {
    int from = frame_;
    frame_ = (frame_ + 1) % header_.frames;
    Paint(frame_, from);
}

void PetBackground::Paint(int frame, int from) {
    const int width = dsc_.header.w;
    const int height = dsc_.header.h;
    lv_area_t dirty = {width, height, -1, -1};

    for (int row = 0; row < header_.rows; row++) {
        int y0 = std::max(0, header_.origin + row * header_.cell);
        int y1 = std::min(height, header_.origin + (row + 1) * header_.cell);
        for (int col = 0; col < header_.cols; col++) {
            int x0 = std::max(0, header_.origin + col * header_.cell);
            int x1 = std::min(width, header_.origin + (col + 1) * header_.cell);
            if (y0 >= y1 || x0 >= x1) {
                continue;
            }
            const uint8_t* rgb = Cell(frame, row, col);
            if (from >= 0 && memcmp(rgb, Cell(from, row, col), 3) == 0) {
                continue;
            }
            uint16_t color = lv_color_to_u16(lv_color_make(rgb[0], rgb[1], rgb[2]));
            for (int y = y0; y < y1; y++) {
                std::fill(pixels_ + y * width + x0, pixels_ + y * width + x1, color);
            }
            dirty.x1 = std::min<int32_t>(dirty.x1, x0);
            dirty.y1 = std::min<int32_t>(dirty.y1, y0);
            dirty.x2 = std::max<int32_t>(dirty.x2, x1 - 1);
            dirty.y2 = std::max<int32_t>(dirty.y2, y1 - 1);
        }
    }
    if (dirty.x2 >= dirty.x1) {
        lv_obj_invalidate_area(image_, &dirty);
    }
}
