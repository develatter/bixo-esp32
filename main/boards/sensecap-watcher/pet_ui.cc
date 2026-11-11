#include "pet_ui.h"

#include "application.h"
#include "display/display.h"
#include "lvgl_theme.h"

#include <esp_log.h>

#include <cmath>
#include <cstdlib>

#define TAG "pet_ui"

extern "C" const lv_font_t font_pixel_16;

namespace {

constexpr int kRingWidth = 10;
constexpr int kRingHideMs = 1500;
constexpr int kMenuCloseMs = 6000;
constexpr int kAdjustStep = 10;
constexpr int kAdjustMin = 10;    // a black screen looks off
constexpr int kRowGap = 92;       // px between the picker rows
constexpr int kTapCentre = 60;    // adjusting: taps closer than this to the middle are done
constexpr int kTapSlop = 12;      // px around a pill that still count as tapping it
constexpr int kPixel = 4;         // the menu's "pixel": pill padding, line spacing

// Bixo's palette (the same as his sprites).
constexpr uint32_t kBackground = 0x080C1E;
constexpr uint32_t kBody = 0x4080FA;
constexpr uint32_t kDim = 0x6E7FAF;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kTrack = 0x1A2244;

const lv_font_t* TextFont(Display* display) {
    return static_cast<LvglTheme*>(display->GetTheme())->text_font()->font();
}

// A capsule filled with bands one menu pixel tall, so its round ends are stair-stepped like
// pixel art instead of smoothed. Consecutive rows with the same inset share a band.
void FillPixelCapsule(lv_obj_t* parent, int width, int height, uint32_t color) {
    int rows = height / kPixel;
    float radius = rows / 2.0f;
    auto inset_of = [&](int row) {
        float dy = std::fabs(row + 0.5f - radius);
        return (int)std::lround(radius - std::sqrt(radius * radius - dy * dy));
    };
    int start = 0;
    for (int row = 1; row <= rows; row++) {
        if (row < rows && inset_of(row) == inset_of(start)) {
            continue;
        }
        int inset = inset_of(start) * kPixel;
        lv_obj_t* band = lv_obj_create(parent);
        lv_obj_remove_style_all(band);
        lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(band, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        lv_obj_set_pos(band, inset, start * kPixel);
        lv_obj_set_size(band, width - 2 * inset, (row - start) * kPixel);
        start = row;
    }
}

}  // namespace

const lv_font_t* PixelFont(Display* display) {
    static lv_font_t font = font_pixel_16;
    font.fallback = TextFont(display);
    return &font;
}

// --- LevelRing ------------------------------------------------------------------------------

LevelRing::LevelRing(Display* display) : display_(display) {
    esp_timer_create_args_t args = {
        .callback = [](void* arg) {
            auto self = static_cast<LevelRing*>(arg);
            DisplayLockGuard lock(self->display_);
            if (self->arc_ != nullptr) {
                lv_obj_add_flag(self->arc_, LV_OBJ_FLAG_HIDDEN);
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "level_ring",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &hide_timer_));
}

LevelRing::~LevelRing() {
    esp_timer_stop(hide_timer_);
    esp_timer_delete(hide_timer_);
}

void LevelRing::Show(int value, uint32_t color) {
    {
        DisplayLockGuard lock(display_);
        if (arc_ == nullptr) {
            int size = LV_MIN(LV_HOR_RES, LV_VER_RES) - 4;
            arc_ = lv_arc_create(lv_layer_top());
            lv_obj_set_size(arc_, size, size);
            lv_obj_center(arc_);
            lv_arc_set_rotation(arc_, 270);  // 0 at the top, filling clockwise
            lv_arc_set_bg_angles(arc_, 0, 360);
            lv_arc_set_range(arc_, 0, 100);
            lv_obj_remove_style(arc_, nullptr, LV_PART_KNOB);
            lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_arc_width(arc_, kRingWidth, LV_PART_MAIN);
            lv_obj_set_style_arc_color(arc_, lv_color_hex(kTrack), LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc_, kRingWidth, LV_PART_INDICATOR);
            lv_obj_set_style_arc_rounded(arc_, true, LV_PART_INDICATOR);
        }
        lv_obj_set_style_arc_color(arc_, lv_color_hex(color), LV_PART_INDICATOR);
        // A rounded cap still draws a dot at 0: hide the indicator instead.
        lv_obj_set_style_arc_opa(arc_, value > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_arc_set_value(arc_, value);
        lv_obj_remove_flag(arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(arc_);
    }
    esp_timer_stop(hide_timer_);
    esp_timer_start_once(hide_timer_, kRingHideMs * 1000);
}

// --- PetMenu --------------------------------------------------------------------------------

PetMenu::PetMenu(Display* display, LevelRing* ring, std::vector<PetMenuItem> items)
    : display_(display), ring_(ring), items_(std::move(items)) {
    esp_timer_create_args_t args = {
        .callback = [](void* arg) {
            auto self = static_cast<PetMenu*>(arg);
            Application::GetInstance().Schedule([self]() { self->Close(); });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pet_menu",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &close_timer_));
}

PetMenu::~PetMenu() {
    esp_timer_stop(close_timer_);
    esp_timer_delete(close_timer_);
}

void PetMenu::Build() {
    auto font = PixelFont(display_);
    overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(overlay_);
    lv_obj_set_style_radius(overlay_, 0, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    lv_obj_set_style_bg_color(overlay_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_90, 0);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);

    auto make_label = [&](lv_obj_t* parent, uint32_t color) {
        lv_obj_t* label = lv_label_create(parent);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(label, kPixel * 2, 0);
        return label;
    };

    // Three pixel-art pills of the same size: the selected item and its dimmed neighbours. The
    // label spans the pill so the text is centred whatever its length.
    int pill_w = LV_HOR_RES * 0.7 / kPixel * kPixel;
    int pill_h = font->line_height + 7 * kPixel;
    pill_w_ = pill_w;
    pill_h_ = pill_h;
    auto make_pill = [&](uint32_t fill, uint32_t text, int y, lv_obj_t** label) {
        lv_obj_t* pill = lv_obj_create(overlay_);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, pill_w, pill_h);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        FillPixelCapsule(pill, pill_w, pill_h, fill);
        lv_obj_align(pill, LV_ALIGN_CENTER, 0, y);
        *label = make_label(pill, text);
        lv_obj_set_width(*label, pill_w - 4 * kPixel);
        lv_label_set_long_mode(*label, LV_LABEL_LONG_CLIP);
        lv_obj_center(*label);
        return pill;
    };
    prev_pill_ = make_pill(kTrack, kDim, -kRowGap, &prev_label_);
    pill_ = make_pill(kBody, kWhite, 0, &current_label_);
    next_pill_ = make_pill(kTrack, kDim, kRowGap, &next_label_);

    hint_label_ = make_label(overlay_, kDim);
    lv_obj_set_width(hint_label_, LV_HOR_RES * 0.8);
    lv_obj_align(hint_label_, LV_ALIGN_CENTER, 0, kRowGap / 2 + 14);

    info_label_ = make_label(overlay_, kWhite);
    lv_obj_set_width(info_label_, LV_HOR_RES * 0.7);
    lv_label_set_long_mode(info_label_, LV_LABEL_LONG_WRAP);
    lv_obj_center(info_label_);
}

void PetMenu::Render() {
    DisplayLockGuard lock(display_);
    if (overlay_ == nullptr) {
        Build();
    }
    if (!open_) {
        lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    // Choosing leaves the screen clear behind the pill, to see each option as it applies.
    lv_obj_set_style_bg_opa(overlay_, mode_ == Mode::kChoose ? LV_OPA_TRANSP : LV_OPA_90, 0);

    bool info = mode_ == Mode::kInfo;
    bool list = mode_ == Mode::kList;
    auto show = [](lv_obj_t* obj, bool visible) {
        visible ? lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    };
    show(info_label_, info);
    show(pill_, !info);
    show(prev_pill_, list && selected_ > 0);
    show(next_pill_, list && selected_ + 1 < (int)items_.size());
    show(hint_label_, mode_ == Mode::kConfirm || mode_ == Mode::kAdjust);

    if (info) {
        lv_label_set_text(info_label_, Current().info().c_str());
        return;
    }
    if (selected_ > 0) {
        lv_label_set_text(prev_label_, items_[selected_ - 1].label.c_str());
    }
    if (selected_ + 1 < (int)items_.size()) {
        lv_label_set_text(next_label_, items_[selected_ + 1].label.c_str());
    }
    switch (mode_) {
        case Mode::kConfirm:
            lv_label_set_text(current_label_, "¿Seguro?");
            if (Current().warning.empty()) {
                lv_label_set_text_fmt(hint_label_, "Pulsa otra vez: %s", Current().label.c_str());
            } else {
                lv_label_set_text_fmt(hint_label_, "%s\nPulsa otra vez", Current().warning.c_str());
            }
            break;
        case Mode::kAdjust:
            lv_label_set_text_fmt(current_label_, "%s %d %%", Current().label.c_str(), Current().get());
            lv_label_set_text(hint_label_, "Gira para cambiar");
            break;
        case Mode::kChoose: {
            auto options = Current().options();
            int option = Current().get();
            const char* name = option >= 0 && option < (int)options.size() ? options[option].c_str() : "";
            lv_label_set_text_fmt(current_label_, "< %s >", name);
            break;
        }
        default:
            lv_label_set_text(current_label_, Current().label.c_str());
            break;
    }
}

void PetMenu::KeepOpen() {
    esp_timer_stop(close_timer_);
    esp_timer_start_once(close_timer_, kMenuCloseMs * 1000);
}

void PetMenu::Open() {
    ESP_LOGI(TAG, "Menu opened");
    open_ = true;
    mode_ = Mode::kList;
    selected_ = 0;
    Render();
    KeepOpen();
}

void PetMenu::Close() {
    if (!open_) {
        return;
    }
    ESP_LOGI(TAG, "Menu closed");
    open_ = false;
    LeaveChoosing();
    mode_ = Mode::kList;
    esp_timer_stop(close_timer_);
    Render();
}

void PetMenu::Move(int steps) {
    if (!open_) {
        return;
    }
    KeepOpen();
    switch (mode_) {
        case Mode::kAdjust: {
            int value = LV_CLAMP(kAdjustMin, Current().get() + steps * kAdjustStep, 100);
            Current().set(value);
            ring_->Show(value, Current().color);
            break;
        }
        case Mode::kChoose: {
            int count = Current().options().size();
            if (count > 0) {
                Current().set(((Current().get() + steps) % count + count) % count);
            }
            break;
        }
        case Mode::kList:
            selected_ = LV_CLAMP(0, selected_ - steps, (int)items_.size() - 1);
            break;
        default:  // moving away cancels a confirmation or closes an info page
            mode_ = Mode::kList;
            break;
    }
    Render();
}

void PetMenu::Select() {
    if (!open_) {
        return;
    }
    KeepOpen();
    const PetMenuItem& item = Current();
    switch (mode_) {
        case Mode::kList:
            switch (item.kind) {
                case PetMenuItem::kAction:
                    Close();
                    item.action();
                    return;
                case PetMenuItem::kConfirm:
                    mode_ = Mode::kConfirm;
                    break;
                case PetMenuItem::kAdjust:
                    mode_ = Mode::kAdjust;
                    ring_->Show(item.get(), item.color);
                    break;
                case PetMenuItem::kInfo:
                    mode_ = Mode::kInfo;
                    break;
                case PetMenuItem::kChoose:
                    mode_ = Mode::kChoose;
                    if (item.choosing) {
                        item.choosing(true);
                    }
                    break;
            }
            break;
        case Mode::kConfirm:
            Close();
            item.action();
            return;
        default:  // done adjusting or choosing, or leaving an info page
            LeaveChoosing();
            mode_ = Mode::kList;
            break;
    }
    Render();
}

void PetMenu::LeaveChoosing() {
    if (mode_ == Mode::kChoose && Current().choosing) {
        Current().choosing(false);
    }
}

std::optional<int> PetMenu::PillAt(int x, int y) const {
    int dx = std::abs(x - LV_HOR_RES / 2);
    if (dx > pill_w_ / 2 + kTapSlop) {
        return std::nullopt;
    }
    for (int row = -1; row <= 1; row++) {
        bool shown = row == 0 || (mode_ == Mode::kList && (row < 0 ? selected_ > 0
                                                                  : selected_ + 1 < (int)items_.size()));
        int dy = std::abs(y - (LV_VER_RES / 2 + row * kRowGap));
        if (shown && dy <= pill_h_ / 2 + kTapSlop) {
            return row;
        }
    }
    return std::nullopt;
}

void PetMenu::Tap(int x, int y) {
    switch (mode_) {
        case Mode::kInfo:
            Select();
            return;
        case Mode::kList:
        case Mode::kConfirm: {
            auto pill = PillAt(x, y);
            if (!pill) {
                Close();
            } else if (*pill == 0) {
                Select();
            } else {
                Move(-*pill);
            }
            return;
        }
        default: {  // adjusting or choosing: above the middle is up, below is down
            int from_centre = y - LV_VER_RES / 2;
            if (from_centre > -kTapCentre && from_centre < kTapCentre) {
                Select();
            } else {
                Move(from_centre < 0 ? 1 : -1);
            }
        }
    }
}
