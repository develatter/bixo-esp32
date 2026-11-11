#ifndef PET_UI_H
#define PET_UI_H

// Bixo's on-screen controls for the round SenseCAP Watcher screen: a ring around the edge that
// shows a level (volume, brightness), the subtitles and a circular icon menu driven by the wheel
// and the touch screen.

#include <esp_timer.h>
#include <lvgl.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pet_icons.h"

class Display;

// Press Start 2P at 16 px (font_pixel_16.c), falling back to the theme font for anything it
// lacks (emoji, CJK). The fallback follows the current theme, so call it again after SetTheme.
const lv_font_t* PixelFont(Display* display);
// The same at 12 px (font_pixel_12.c), for the menu's labels.
const lv_font_t* SmallPixelFont(Display* display);

// Twenty pixel-art segments along the edge of the screen, lit clockwise from the top, one per 5 %.
// Shows on Show() and hides itself after a while, unless held (the menu holds it while a value
// is being adjusted) until Hide().
class LevelRing {
public:
    explicit LevelRing(Display* display);
    ~LevelRing();
    void Show(int value, uint32_t color, bool hold = false);
    void Hide();

private:
    Display* display_;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* segments_[PET_RING_SEGMENTS] = {};
    esp_timer_handle_t hide_timer_ = nullptr;
};

// Subtitles in the free space under Bixo: a shaded band with three lines of text. A reply
// builds up sentence by sentence and scrolls up a line at a time as it is spoken; the wheel or a
// finger scrolls it back. Once the reply is over (Finish) it hides itself after a few seconds
// without input. Call everything with the display locked.
class PetSubtitles {
public:
    PetSubtitles(Display* display, lv_obj_t* screen);
    ~PetSubtitles();

    // "assistant" text adds to the reply on screen; anything else replaces it. Empty clears.
    void Set(const std::string& role, const std::string& text);
    // The reply is over: hide after linger_ms without input (short if it was heard, long enough
    // to read if it was only text).
    void Finish(int linger_ms);
    void Clear();
    bool Shown() const;
    const std::string& Text() const { return text_; }
    // Wheel notches: +1 shows the next line.
    void Scroll(int lines);
    // A finger on the text: Drag() takes how far it has moved since DragStart(), in px.
    bool InText(int x, int y) const;
    void DragStart();
    void Drag(int dy);
    void SetColors(lv_color_t panel, lv_color_t text);
    void SetHidden(bool hidden);

private:
    Display* display_;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* fade_ = nullptr;
    lv_obj_t* band_ = nullptr;
    lv_obj_t* box_ = nullptr;
    lv_obj_t* label_ = nullptr;
    lv_obj_t* up_ = nullptr;
    lv_obj_t* down_ = nullptr;
    lv_obj_t* track_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    std::string text_;
    std::string role_;
    bool finished_ = false;
    bool dismissed_ = false;  // cleared mid-reply: the rest of it stays hidden
    bool hidden_ = false;     // the pet is hidden to look at a background
    bool following_ = true;   // scrolling with the speech, until the user scrolls
    int lines_ = 0;
    int top_ = 0;             // first visible line
    int drag_top_ = 0;
    int64_t last_step_us_ = 0;
    int64_t last_input_us_ = 0;
    int linger_ms_ = 0;

    void Tick();
    void Layout();
    void ScrollTo(int top, bool animate);
    void ShowTrack();
    void UpdateVisibility();
    int MaxTop() const { return lines_ > kLines ? lines_ - kLines : 0; }

    static constexpr int kLines = 3;
};

struct PetMenuItem {
    enum Kind {
        kAction,   // runs at once
        kConfirm,  // asks for a second press first
        kAdjust,   // the wheel or a tap changes a 0-100 value, shown on the ring
        kInfo,     // shows a text until the next input
        kChoose,   // the wheel or a tap steps through options, each applied at once
        kToggle,   // a press switches it on or off; the menu stays open
    };
    Kind kind;
    std::string label;
    const lv_image_dsc_t* icon = nullptr;                 // A8 art (pet_icons.h)
    const lv_image_dsc_t* icon_off = nullptr;             // kToggle: the icon while off
    std::function<void()> action = nullptr;               // kAction, kConfirm
    std::function<int()> get = nullptr;                   // kAdjust; kChoose: the option; kToggle: 0/1
    std::function<void(int)> set = nullptr;               // kAdjust, kChoose, kToggle
    std::function<std::vector<std::string>()> options = nullptr;  // kChoose
    std::function<void(bool)> choosing = nullptr;        // kChoose: entering, leaving
    std::function<std::string()> info = nullptr;          // kInfo
    std::string warning;                                  // kConfirm: what it does, instead of "Pulsa otra vez"
    uint32_t color = 0xFFD65C;                            // kAdjust ring colour
};

// Icons around the edge of the screen over a desaturated still of it; the selected one sits on
// a blue disc and its name shows under Bixo. Call everything from the main task
// (Application::Schedule): the class is not thread safe, only its LVGL access is.
class PetMenu {
public:
    PetMenu(Display* display, LevelRing* ring, std::vector<PetMenuItem> items);
    ~PetMenu();

    bool IsOpen() const { return open_; }
    void Open();
    void Close();
    // Wheel notches: +1 moves to the next icon clockwise.
    void Move(int steps);
    // Wheel click, or a tap on the selected icon or its label.
    void Select();
    // A tap at (x, y): on an icon selects it; off the icons and the label closes the menu.
    void Tap(int x, int y);

private:
    enum class Mode { kList, kConfirm, kAdjust, kInfo, kChoose };

    Display* display_;
    LevelRing* ring_;
    std::vector<PetMenuItem> items_;
    bool open_ = false;
    Mode mode_ = Mode::kList;
    int selected_ = 0;
    bool backdrop_stale_ = true;
    bool has_backdrop_ = false;
    esp_timer_handle_t close_timer_ = nullptr;

    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* backdrop_ = nullptr;
    lv_draw_buf_t backdrop_buf_ = {};
    uint8_t* backdrop_pixels_ = nullptr;
    std::vector<lv_obj_t*> discs_;
    std::vector<lv_obj_t*> icons_;
    lv_obj_t* label_pill_ = nullptr;
    lv_obj_t* label_ = nullptr;
    lv_obj_t* hint_pill_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    lv_obj_t* info_box_ = nullptr;
    lv_obj_t* info_label_ = nullptr;

    void Build();
    void Render();
    void RenderIcons();
    // The screen as it is, greyed and darkened, behind the icons.
    void TakeBackdrop();
    void KeepOpen();
    // Back to the icons from adjusting or choosing.
    void LeaveMode();
    lv_point_t ItemCentre(int index) const;
    std::optional<int> ItemAt(int x, int y) const;
    bool OnLabel(int x, int y) const;
    std::string LabelText() const;
    const PetMenuItem& Current() const { return items_[selected_]; }
};

#endif  // PET_UI_H
