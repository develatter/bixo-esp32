#ifndef PET_BACKGROUND_H
#define PET_BACKGROUND_H

// The scenery behind Bixo. The assets partition lists the backgrounds in index.json
// ("backgrounds", in menu order); each file holds the frames of a loop as a grid of square
// cells, plus Bixo's shadow on the ground, which is left out while he is hidden. They are painted
// into a full-screen RGB565 image under the sprite, and an animated one only repaints, and only
// invalidates, the cells that change from one frame to the next.

#include <lvgl.h>

#include <cstdint>
#include <string>
#include <vector>

class PetBackground {
public:
    struct Info {
        std::string name;
        std::string label;
        std::string file;
        lv_color_t text;   // subtitles
        lv_color_t panel;  // the translucent band behind them
    };

    PetBackground() = default;
    ~PetBackground();

    // Reads the list from the assets. Call with the display locked, once they are applied.
    void Load(lv_obj_t* parent);
    const std::vector<Info>& list() const { return list_; }
    int Find(const std::string& name) const;
    int current() const { return current_; }

    // Shows list()[index]. Call with the display locked.
    bool Show(int index);
    // Stops the animation while the screen sleeps.
    void SetPaused(bool paused);
    // Bixo's shadow, for when he is on screen. Call with the display locked.
    void SetShadow(bool shown);

private:
    struct Header {
        int cell, origin, cols, rows, frames, ms;
        int shadow_row, shadow_first, shadow_end;
        uint8_t shadow_rgb[3];
    };

    std::vector<Info> list_;
    int current_ = -1;
    Header header_ = {};
    const uint8_t* cells_ = nullptr;  // header_.frames x rows x cols RGB888, in the assets mmap
    int frame_ = 0;
    bool paused_ = false;
    bool shadow_ = true;

    uint16_t* pixels_ = nullptr;
    lv_image_dsc_t dsc_ = {};
    lv_obj_t* image_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    // Paints `frame`; with `from` >= 0, only the cells that differ from that frame.
    void Paint(int frame, int from);
    void NextFrame();
    const uint8_t* Cell(int frame, int row, int col) const;
};

#endif  // PET_BACKGROUND_H
