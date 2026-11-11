#include "pet_ui.h"

#include "application.h"
#include "display/display.h"
#include "lvgl_theme.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cmath>
#include <cstdlib>

#define TAG "pet_ui"

extern "C" const lv_font_t font_pixel_16;
extern "C" const lv_font_t font_pixel_12;

namespace {

constexpr int kRingHideMs = 1500;
constexpr int kMenuCloseMs = 6000;
constexpr int kAdjustStep = 5;   // one ring segment per notch
constexpr int kAdjustMin = 10;    // a black screen looks off
constexpr int kTapCentre = 60;    // adjusting: taps closer than this to the middle are done
constexpr int kTapSlop = 12;      // px around a pill or an icon that still count as tapping it
constexpr int kBlock = 3;         // the menu's art pixel, as in the icons (pet_icons.c)

// The menu's layout on the 412 px screen: icon centres on a circle, the label pill just under
// Bixo's feet and the confirmation hint just over his head.
constexpr int kItemRadius = 170;
constexpr int kDiscRadius = 30;
constexpr int kPillH = 27;
constexpr int kPillPad = 21;
constexpr int kLabelY = 280;      // top of the label pill
constexpr int kHintCentreY = 108;
constexpr int kInfoPad = 15;
constexpr int kInfoLineSpace = 8;

// Bixo's palette (the same as his sprites).
constexpr uint32_t kPlate = 0x080C1E;
constexpr uint32_t kDisc = 0x263468;
constexpr uint32_t kDiscOn = 0x4080FA;
constexpr uint32_t kIcon = 0x96AADE;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kConfirm = 0xD63A46;
constexpr uint32_t kHintText = 0xFF969C;
constexpr uint32_t kRingOff = 0x282E48;
constexpr uint32_t kTrack = 0x1A2244;
constexpr uint32_t kShade = 0x30343C;  // over the screen when there is no memory for the still

const lv_font_t* TextFont(Display* display) {
    return static_cast<LvglTheme*>(display->GetTheme())->text_font()->font();
}

// Fills parent (width x height) with a rectangle of block-tall bands whose corners are
// stair-stepped circles of `radius` blocks; radius = rows / 2 makes a capsule. Consecutive rows
// with the same inset share a band.
void FillPixelRect(lv_obj_t* parent, int width, int height, float radius, uint32_t color) {
    int rows = height / kBlock;
    auto inset_of = [&](int row) {
        float centre = row + 0.5f;
        float dy = centre < radius ? radius - centre : centre > rows - radius ? centre - (rows - radius) : 0;
        return (int)std::lround(radius - std::sqrt(std::max(0.0f, radius * radius - dy * dy)));
    };
    int start = 0;
    for (int row = 1; row <= rows; row++) {
        if (row < rows && inset_of(row) == inset_of(start)) {
            continue;
        }
        int inset = inset_of(start) * kBlock;
        lv_obj_t* band = lv_obj_create(parent);
        lv_obj_remove_style_all(band);
        lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(band, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        lv_obj_set_pos(band, inset, start * kBlock);
        lv_obj_set_size(band, width - 2 * inset, (row - start) * kBlock);
        start = row;
    }
}

lv_obj_t* PlainBox(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

// An A8 image, painted in `color`.
lv_obj_t* ArtImage(lv_obj_t* parent, const lv_image_dsc_t* art, uint32_t color) {
    lv_obj_t* image = lv_image_create(parent);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(image, art);
    lv_obj_set_style_image_recolor(image, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    return image;
}

void SetArt(lv_obj_t* image, const lv_image_dsc_t* art, lv_point_t centre, uint32_t color) {
    if (lv_image_get_src(image) != art) {
        lv_image_set_src(image, art);
    }
    lv_obj_set_pos(image, centre.x - art->header.w / 2, centre.y - art->header.h / 2);
    lv_obj_set_style_image_recolor(image, lv_color_hex(color), 0);
}

void SetShown(lv_obj_t* obj, bool shown) {
    shown ? lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* PixelLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(label, kInfoLineSpace, 0);
    return label;
}

// A pixel capsule around one line of text, centred on (screen centre, centre_y). The bands are
// rebuilt to fit the text; the label is a sibling drawn over them.
void ShowPill(lv_obj_t* pill, lv_obj_t* label, const std::string& text, uint32_t fill, int centre_y) {
    lv_label_set_text(label, text.c_str());
    lv_obj_update_layout(label);
    int width = (lv_obj_get_width(label) + kPillPad) / kBlock * kBlock;
    lv_obj_clean(pill);
    lv_obj_set_size(pill, width, kPillH);
    FillPixelRect(pill, width, kPillH, kPillH / kBlock / 2.0f, fill);
    lv_obj_set_pos(pill, (LV_HOR_RES - width) / 2, centre_y - kPillH / 2);
    lv_obj_set_pos(label, (LV_HOR_RES - lv_obj_get_width(label)) / 2, centre_y - lv_obj_get_height(label) / 2);
    SetShown(pill, true);
    SetShown(label, true);
}

// Greys the screen still and darkens it a little: the same scene, clearly behind the menu.
// out = 0.8 * (0.2 * colour + 0.8 * luma), per channel.
void Desaturate(lv_draw_buf_t* buf) {
    for (uint32_t y = 0; y < buf->header.h; y++) {
        auto row = reinterpret_cast<uint16_t*>(buf->data + y * buf->header.stride);
        for (uint32_t x = 0; x < buf->header.w; x++) {
            uint16_t px = row[x];
            int r = (px >> 11) << 3, g = ((px >> 5) & 0x3F) << 2, b = (px & 0x1F) << 3;
            int luma = (r * 77 + g * 150 + b * 29) >> 8;
            r = (41 * r + 164 * luma) >> 8;
            g = (41 * g + 164 * luma) >> 8;
            b = (41 * b + 164 * luma) >> 8;
            row[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }
    }
}

}  // namespace

const lv_font_t* PixelFont(Display* display) {
    static lv_font_t font = font_pixel_16;
    font.fallback = TextFont(display);
    return &font;
}

const lv_font_t* SmallPixelFont(Display* display) {
    static lv_font_t font = font_pixel_12;
    font.fallback = TextFont(display);
    return &font;
}

// --- LevelRing ------------------------------------------------------------------------------

LevelRing::LevelRing(Display* display) : display_(display) {
    esp_timer_create_args_t args = {
        .callback = [](void* arg) {
            auto self = static_cast<LevelRing*>(arg);
            DisplayLockGuard lock(self->display_);
            if (self->root_ != nullptr) {
                lv_obj_add_flag(self->root_, LV_OBJ_FLAG_HIDDEN);
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

void LevelRing::Show(int value, uint32_t color, bool hold) {
    {
        DisplayLockGuard lock(display_);
        if (root_ == nullptr) {
            root_ = PlainBox(lv_layer_top());
            lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
            for (int i = 0; i < PET_RING_SEGMENTS; i++) {
                segments_[i] = ArtImage(root_, pet_ring_segments[i], kRingOff);
                lv_obj_set_pos(segments_[i], pet_ring_segment_pos[i].x, pet_ring_segment_pos[i].y);
            }
        }
        int lit = (value * PET_RING_SEGMENTS + 99) / 100;  // any level above 0 lights one
        for (int i = 0; i < PET_RING_SEGMENTS; i++) {
            lv_obj_set_style_image_recolor(segments_[i], lv_color_hex(i < lit ? color : kRingOff), 0);
        }
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
    }
    esp_timer_stop(hide_timer_);
    if (!hold) {
        esp_timer_start_once(hide_timer_, kRingHideMs * 1000);
    }
}

void LevelRing::Hide() {
    esp_timer_stop(hide_timer_);
    DisplayLockGuard lock(display_);
    if (root_ != nullptr) {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

// --- PetSubtitles ---------------------------------------------------------------------------

namespace {

// The band lines up with the backgrounds' 13 px grid (cells start at -2): a lighter row as a
// fade, then the panel down to the bottom of the screen.
constexpr int kFadeY = 284;
constexpr int kBandY = 297;
constexpr int kTextY = 303;
constexpr int kTextW = 256;       // 16 characters: wider lines would touch the round edge
constexpr int kLineSpace = 6;
constexpr int kLineMs = 1100;     // one line every so often while it speaks (about speech pace)
constexpr int kTrackMs = 1500;    // the position arc after the last scroll
constexpr int kSlideMs = 150;
constexpr int kArrowPixel = 2;
constexpr uint32_t kCeleste = 0x2BD5FF;
// The position arc on the right edge (degrees clockwise from 3 o'clock).
constexpr int kTrackFrom = 10;
constexpr int kTrackSpan = 52;
constexpr int kThumb = 12;

// A small pixel triangle (pointing up or down) built from bands, like the menu's capsules.
lv_obj_t* PixelArrow(lv_obj_t* parent, bool up) {
    constexpr int kRows = 3;
    lv_obj_t* arrow = PlainBox(parent);
    lv_obj_set_size(arrow, (2 * kRows - 1) * kArrowPixel, kRows * kArrowPixel);
    for (int row = 0; row < kRows; row++) {
        int width = (2 * (up ? row : kRows - 1 - row) + 1) * kArrowPixel;
        lv_obj_t* band = PlainBox(arrow);
        lv_obj_set_style_bg_color(band, lv_color_hex(kCeleste), 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        lv_obj_set_pos(band, (kRows - 1) * kArrowPixel - width / 2 + kArrowPixel / 2, row * kArrowPixel);
        lv_obj_set_size(band, width, kArrowPixel);
    }
    return arrow;
}

int LinePitch(lv_obj_t* label) {
    return lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)) + kLineSpace;
}

}  // namespace

PetSubtitles::PetSubtitles(Display* display, lv_obj_t* screen) : display_(display) {
    root_ = PlainBox(screen);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(root_, 0, 0);

    fade_ = PlainBox(root_);
    lv_obj_set_pos(fade_, 0, kFadeY);
    lv_obj_set_size(fade_, LV_HOR_RES, kBandY - kFadeY);
    lv_obj_set_style_bg_opa(fade_, LV_OPA_30, 0);

    band_ = PlainBox(root_);
    lv_obj_set_pos(band_, 0, kBandY);
    lv_obj_set_size(band_, LV_HOR_RES, LV_VER_RES - kBandY);
    lv_obj_set_style_bg_opa(band_, LV_OPA_70, 0);

    box_ = PlainBox(root_);  // clips the label to its three lines
    lv_obj_set_width(box_, kTextW);
    lv_obj_set_pos(box_, (LV_HOR_RES - kTextW) / 2, kTextY);

    label_ = lv_label_create(box_);
    lv_obj_set_width(label_, kTextW);
    lv_label_set_long_mode(label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(label_, kLineSpace, 0);
    lv_label_set_text(label_, "");

    up_ = PixelArrow(root_, true);
    down_ = PixelArrow(root_, false);

    int size = LV_MIN(LV_HOR_RES, LV_VER_RES) - 4;
    track_ = lv_arc_create(root_);
    lv_obj_set_size(track_, size, size);
    lv_obj_center(track_);
    lv_obj_remove_style(track_, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(track_, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_mode(track_, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(track_, kTrackFrom, kTrackFrom + kTrackSpan + kThumb);
    lv_obj_set_style_arc_width(track_, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(track_, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_arc_width(track_, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(track_, lv_color_hex(kCeleste), LV_PART_INDICATOR);
    lv_obj_add_flag(track_, LV_OBJ_FLAG_HIDDEN);

    timer_ = lv_timer_create([](lv_timer_t* timer) {
        static_cast<PetSubtitles*>(lv_timer_get_user_data(timer))->Tick();
    }, 100, this);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

PetSubtitles::~PetSubtitles() {
    lv_timer_delete(timer_);
    lv_obj_delete(root_);
}

void PetSubtitles::Set(const std::string& role, const std::string& text) {
    if (text.empty()) {
        Clear();
        return;
    }
    bool continues = role == "assistant" && role_ == "assistant" && !finished_;
    if (continues && dismissed_) {
        return;
    }
    text_ = continues ? text_ + " " + text : text;
    if (!continues) {
        top_ = 0;
        following_ = true;
        dismissed_ = false;
    }
    role_ = role;
    finished_ = false;
    int64_t now = esp_timer_get_time();
    last_input_us_ = now;
    if (!continues) {
        last_step_us_ = now;
    }
    lv_label_set_text(label_, text_.c_str());
    Layout();
    UpdateVisibility();
}

void PetSubtitles::Finish(int linger_ms) {
    if (!finished_) {
        finished_ = true;
        linger_ms_ = linger_ms;
        last_input_us_ = esp_timer_get_time();
    }
}

void PetSubtitles::Clear() {
    // Mid-reply, the sentences still to come stay hidden too.
    dismissed_ = role_ == "assistant" && !finished_ && !text_.empty();
    text_.clear();
    lines_ = 0;
    top_ = 0;
    lv_label_set_text(label_, "");
    UpdateVisibility();
}

bool PetSubtitles::Shown() const { return !text_.empty() && !hidden_; }

void PetSubtitles::Scroll(int lines) {
    following_ = false;
    last_input_us_ = esp_timer_get_time();
    ScrollTo(top_ + lines, true);
    ShowTrack();
}

bool PetSubtitles::InText(int x, int y) const {
    return Shown() && y >= kFadeY && std::abs(x - LV_HOR_RES / 2) <= kTextW / 2 + kTapSlop;
}

void PetSubtitles::DragStart() { drag_top_ = top_; }

void PetSubtitles::Drag(int dy) {
    following_ = false;
    last_input_us_ = esp_timer_get_time();
    // Dragging up shows the lines below, like scrolling a page.
    int pitch = LinePitch(label_);
    int lines = (dy >= 0 ? dy + pitch / 2 : dy - pitch / 2) / pitch;
    ScrollTo(drag_top_ - lines, false);
    ShowTrack();
}

void PetSubtitles::SetColors(lv_color_t panel, lv_color_t text) {
    lv_obj_set_style_bg_color(fade_, panel, 0);
    lv_obj_set_style_bg_color(band_, panel, 0);
    lv_obj_set_style_text_color(label_, text, 0);
}

void PetSubtitles::SetHidden(bool hidden) {
    hidden_ = hidden;
    UpdateVisibility();
}

void PetSubtitles::Tick() {
    if (text_.empty()) {
        return;
    }
    int64_t now = esp_timer_get_time();
    bool behind = following_ && top_ < MaxTop();
    if (behind && now - last_step_us_ >= kLineMs * 1000LL) {
        last_step_us_ = now;
        last_input_us_ = now;
        ScrollTo(top_ + 1, true);
        return;
    }
    if (!lv_obj_has_flag(track_, LV_OBJ_FLAG_HIDDEN) && now - last_input_us_ >= kTrackMs * 1000LL) {
        lv_obj_add_flag(track_, LV_OBJ_FLAG_HIDDEN);
    }
    if (finished_ && !behind && now - last_input_us_ >= linger_ms_ * 1000LL) {
        Clear();
    }
}

// After the text changes: count its lines and keep the view where it was.
void PetSubtitles::Layout() {
    // The font can change with the theme: size the box from it every time.
    int pitch = LinePitch(label_);
    lv_obj_set_height(box_, kLines * pitch - kLineSpace);
    lv_obj_update_layout(label_);
    lines_ = (lv_obj_get_height(label_) + kLineSpace) / pitch;
    ScrollTo(top_, false);
}

void PetSubtitles::ScrollTo(int top, bool animate) {
    top_ = LV_CLAMP(0, top, MaxTop());
    int pitch = LinePitch(label_);
    // Fewer lines than the box: centred in it.
    int y = lines_ < kLines ? (kLines - lines_) * pitch / 2 : -top_ * pitch;
    lv_anim_delete(label_, nullptr);
    if (animate && lv_obj_get_y(label_) != y) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, label_);
        lv_anim_set_values(&anim, lv_obj_get_y(label_), y);
        lv_anim_set_duration(&anim, kSlideMs);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&anim, [](void* obj, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(obj), v); });
        lv_anim_start(&anim);
    } else {
        lv_obj_set_y(label_, y);
    }
    lv_obj_align_to(up_, box_, LV_ALIGN_OUT_TOP_MID, 0, -1);
    lv_obj_align_to(down_, box_, LV_ALIGN_OUT_BOTTOM_MID, 0, 3);
    top_ > 0 ? lv_obj_remove_flag(up_, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(up_, LV_OBJ_FLAG_HIDDEN);
    top_ < MaxTop() ? lv_obj_remove_flag(down_, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(down_, LV_OBJ_FLAG_HIDDEN);
    int max_top = MaxTop();
    if (max_top > 0) {
        int from = kTrackFrom + top_ * kTrackSpan / max_top;
        lv_arc_set_angles(track_, from, from + kThumb);
    }
}

void PetSubtitles::ShowTrack() {
    if (MaxTop() > 0) {
        lv_obj_remove_flag(track_, LV_OBJ_FLAG_HIDDEN);
    }
}

void PetSubtitles::UpdateVisibility() {
    if (Shown()) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(track_, LV_OBJ_FLAG_HIDDEN);
    }
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
    heap_caps_free(backdrop_pixels_);
}

lv_point_t PetMenu::ItemCentre(int index) const {
    float angle = -M_PI / 2 + index * 2 * M_PI / items_.size();  // clockwise from the top
    return {(int32_t)std::lround(LV_HOR_RES / 2 + kItemRadius * std::cos(angle)),
            (int32_t)std::lround(LV_VER_RES / 2 + kItemRadius * std::sin(angle))};
}

void PetMenu::Build() {
    overlay_ = PlainBox(lv_layer_top());
    lv_obj_set_size(overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(overlay_, lv_color_hex(kShade), 0);

    backdrop_ = lv_image_create(overlay_);
    lv_obj_remove_flag(backdrop_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(backdrop_, 0, 0);

    for (const auto& item : items_) {
        discs_.push_back(ArtImage(overlay_, &pet_disc, kDisc));
        icons_.push_back(ArtImage(overlay_, item.icon, kIcon));
    }

    auto font = SmallPixelFont(display_);
    label_pill_ = PlainBox(overlay_);
    label_ = PixelLabel(overlay_, font, kWhite);
    hint_pill_ = PlainBox(overlay_);
    hint_ = PixelLabel(overlay_, font, kHintText);
    info_box_ = PlainBox(overlay_);
    info_label_ = PixelLabel(overlay_, font, kWhite);
}

void PetMenu::TakeBackdrop() {
    backdrop_stale_ = false;
    if (backdrop_pixels_ == nullptr) {
        uint32_t stride = lv_draw_buf_width_to_stride(LV_HOR_RES, LV_COLOR_FORMAT_RGB565);
        uint32_t size = stride * LV_VER_RES;
        backdrop_pixels_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (backdrop_pixels_ == nullptr) {
            ESP_LOGW(TAG, "No memory for the menu's backdrop: shading the screen instead");
            return;
        }
        lv_draw_buf_init(&backdrop_buf_, LV_HOR_RES, LV_VER_RES, LV_COLOR_FORMAT_RGB565, stride,
                         backdrop_pixels_, size);
    }
    has_backdrop_ = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565,
                                                 &backdrop_buf_) == LV_RESULT_OK;
    if (!has_backdrop_) {
        ESP_LOGW(TAG, "Screen snapshot failed: shading the screen instead");
        return;
    }
    Desaturate(&backdrop_buf_);
    lv_image_set_src(backdrop_, &backdrop_buf_);
    lv_obj_invalidate(backdrop_);
}

std::string PetMenu::LabelText() const {
    const PetMenuItem& item = Current();
    switch (mode_) {
        case Mode::kConfirm:
            return "¿" + item.label + "?";
        case Mode::kAdjust:
            return item.label + " " + std::to_string(item.get()) + " %";
        case Mode::kChoose: {
            auto options = item.options();
            int option = item.get();
            return "< " + (option >= 0 && option < (int)options.size() ? options[option] : "") + " >";
        }
        default:
            if (item.kind == PetMenuItem::kToggle) {
                return item.label + (item.get() ? ": sí" : ": no");
            }
            return item.label;
    }
}

void PetMenu::RenderIcons() {
    bool shown = mode_ == Mode::kList || mode_ == Mode::kConfirm || mode_ == Mode::kInfo;
    for (int i = 0; i < (int)items_.size(); i++) {
        SetShown(discs_[i], shown);
        SetShown(icons_[i], shown);
        if (!shown) {
            continue;
        }
        const PetMenuItem& item = items_[i];
        bool selected = i == selected_;
        uint32_t disc = !selected ? kDisc : mode_ == Mode::kConfirm ? kConfirm : kDiscOn;
        const lv_image_dsc_t* icon = item.kind == PetMenuItem::kToggle && !item.get() ? item.icon_off : item.icon;
        SetArt(discs_[i], &pet_disc, ItemCentre(i), disc);
        SetArt(icons_[i], icon, ItemCentre(i), selected ? kWhite : kIcon);
    }
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
    // Choosing a background hides Bixo and leaves the screen clear to see each option.
    bool choosing = mode_ == Mode::kChoose;
    if (!choosing && backdrop_stale_) {
        TakeBackdrop();
    }
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    SetShown(backdrop_, has_backdrop_ && !choosing);
    lv_obj_set_style_bg_opa(overlay_, has_backdrop_ || choosing ? LV_OPA_TRANSP : LV_OPA_70, 0);

    RenderIcons();

    bool confirm = mode_ == Mode::kConfirm;
    if (mode_ == Mode::kInfo) {
        SetShown(label_pill_, false);
        SetShown(label_, false);
    } else {
        ShowPill(label_pill_, label_, LabelText(), confirm ? kConfirm : kPlate, kLabelY + kPillH / 2);
    }
    if (confirm) {
        ShowPill(hint_pill_, hint_, Current().warning.empty() ? "Pulsa otra vez" : Current().warning,
                 kPlate, kHintCentreY);
    } else {
        SetShown(hint_pill_, false);
        SetShown(hint_, false);
    }

    bool info = mode_ == Mode::kInfo;
    SetShown(info_box_, info);
    SetShown(info_label_, info);
    if (info) {
        lv_label_set_text(info_label_, Current().info().c_str());
        lv_obj_update_layout(info_label_);
        int width = (lv_obj_get_width(info_label_) + 2 * kInfoPad) / kBlock * kBlock;
        int height = (lv_obj_get_height(info_label_) + 2 * kInfoPad) / kBlock * kBlock;
        lv_obj_clean(info_box_);
        lv_obj_set_size(info_box_, width, height);
        FillPixelRect(info_box_, width, height, 3, kPlate);
        lv_obj_center(info_box_);
        lv_obj_center(info_label_);
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
    backdrop_stale_ = true;
    Render();
    KeepOpen();
}

void PetMenu::Close() {
    if (!open_) {
        return;
    }
    ESP_LOGI(TAG, "Menu closed");
    open_ = false;
    LeaveMode();
    esp_timer_stop(close_timer_);
    Render();
}

void PetMenu::LeaveMode() {
    if (mode_ == Mode::kAdjust) {
        ring_->Hide();
    } else if (mode_ == Mode::kChoose) {
        if (Current().choosing) {
            Current().choosing(false);
        }
        backdrop_stale_ = true;  // Bixo is back, maybe on a new background
    }
    mode_ = Mode::kList;
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
            ring_->Show(value, Current().color, true);
            break;
        }
        case Mode::kChoose: {
            int count = Current().options().size();
            if (count > 0) {
                Current().set(((Current().get() + steps) % count + count) % count);
            }
            break;
        }
        case Mode::kList: {
            int count = items_.size();
            selected_ = ((selected_ + steps) % count + count) % count;
            break;
        }
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
                    ring_->Show(item.get(), item.color, true);
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
                case PetMenuItem::kToggle:
                    item.set(item.get() ? 0 : 1);
                    break;
            }
            break;
        case Mode::kConfirm:
            Close();
            item.action();
            return;
        default:  // done adjusting or choosing, or leaving an info page
            LeaveMode();
            break;
    }
    Render();
}

std::optional<int> PetMenu::ItemAt(int x, int y) const {
    for (int i = 0; i < (int)items_.size(); i++) {
        lv_point_t centre = ItemCentre(i);
        if (std::hypot(x - centre.x, y - centre.y) <= kDiscRadius + kTapSlop) {
            return i;
        }
    }
    return std::nullopt;
}

bool PetMenu::OnLabel(int x, int y) const {
    lv_area_t area;
    lv_obj_get_coords(label_pill_, &area);
    return x >= area.x1 - kTapSlop && x <= area.x2 + kTapSlop &&
           y >= area.y1 - kTapSlop && y <= area.y2 + kTapSlop;
}

void PetMenu::Tap(int x, int y) {
    switch (mode_) {
        case Mode::kInfo:
            Select();
            return;
        case Mode::kList:
        case Mode::kConfirm: {
            if (auto item = ItemAt(x, y)) {
                if (*item == selected_) {
                    Select();
                } else {  // another icon: go to it (and drop a pending confirmation)
                    KeepOpen();
                    mode_ = Mode::kList;
                    selected_ = *item;
                    Render();
                }
            } else if (OnLabel(x, y)) {
                Select();
            } else {
                Close();
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
