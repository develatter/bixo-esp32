#ifndef PET_UI_H
#define PET_UI_H

// Bixo's on-screen controls for the round SenseCAP Watcher screen: a ring around the edge that
// shows a level (volume, brightness) and a picker menu driven by the wheel and the touch screen.

#include <esp_timer.h>
#include <lvgl.h>

#include <functional>
#include <string>
#include <vector>

class Display;

// Press Start 2P at 16 px (font_pixel_16.c), falling back to the theme font for anything it
// lacks (emoji, CJK). The fallback follows the current theme, so call it again after SetTheme.
const lv_font_t* PixelFont(Display* display);

// Arc along the edge of the screen, full at 100. Shows on Show() and hides itself after a while.
class LevelRing {
public:
    explicit LevelRing(Display* display);
    ~LevelRing();
    void Show(int value, uint32_t color);

private:
    Display* display_;
    lv_obj_t* arc_ = nullptr;
    esp_timer_handle_t hide_timer_ = nullptr;
};

struct PetMenuItem {
    enum Kind {
        kAction,   // runs at once
        kConfirm,  // asks "¿Seguro?" first
        kAdjust,   // the wheel or a tap changes a 0-100 value, shown on the ring
        kInfo,     // shows a text until the next input
        kChoose,   // the wheel or a tap steps through options, each applied at once
    };
    Kind kind;
    std::string label;
    std::function<void()> action = nullptr;               // kAction, kConfirm
    std::function<int()> get = nullptr;                   // kAdjust; kChoose: the option
    std::function<void(int)> set = nullptr;               // kAdjust, kChoose
    std::function<std::vector<std::string>()> options = nullptr;  // kChoose
    std::function<void(bool)> choosing = nullptr;        // kChoose: entering, leaving
    std::function<std::string()> info = nullptr;          // kInfo
    uint32_t color = 0xFFD65C;                            // kAdjust ring colour
};

// Picker: the selected item in the middle, its neighbours above and below. Call everything from
// the main task (Application::Schedule): the class is not thread safe, only its LVGL access is.
class PetMenu {
public:
    PetMenu(Display* display, LevelRing* ring, std::vector<PetMenuItem> items);
    ~PetMenu();

    bool IsOpen() const { return open_; }
    void Open();
    void Close();
    // One wheel notch or a tap above (+1) / below (-1) the middle.
    void Move(int steps);
    // Wheel click or a tap on the middle.
    void Select();
    // A tap at screen row y.
    void Tap(int y);

private:
    enum class Mode { kList, kConfirm, kAdjust, kInfo, kChoose };

    Display* display_;
    LevelRing* ring_;
    std::vector<PetMenuItem> items_;
    bool open_ = false;
    Mode mode_ = Mode::kList;
    int selected_ = 0;
    esp_timer_handle_t close_timer_ = nullptr;

    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* prev_pill_ = nullptr;
    lv_obj_t* prev_label_ = nullptr;
    lv_obj_t* pill_ = nullptr;
    lv_obj_t* current_label_ = nullptr;
    lv_obj_t* next_pill_ = nullptr;
    lv_obj_t* next_label_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* info_label_ = nullptr;

    void Build();
    void Render();
    void KeepOpen();
    void LeaveChoosing();
    const PetMenuItem& Current() const { return items_[selected_]; }
};

#endif  // PET_UI_H
