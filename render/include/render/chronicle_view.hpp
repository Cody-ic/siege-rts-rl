#ifndef RENDER_CHRONICLE_VIEW_HPP
#define RENDER_CHRONICLE_VIEW_HPP
#include <vector>
#include <string_view>
#include "raylib.h"
#include "render/text.hpp"
#include "game/chronicle.hpp"
namespace render {
class ChronicleView {
public:
    bool open = false;
    void update(Vector2 viewport, int reached_wave);
    void draw(const FontSet& font, Vector2 viewport, int reached_wave);
    void preview(int chapter,int ending=0,bool bottom=false) noexcept;
    void preview_appendix(int appendix,bool bottom=false,bool emphasis=false) noexcept;
    game::ChronicleAppendix appendices;
    bool decision_enabled = false;
    game::ChronicleChoice choice = game::ChronicleChoice::None;
    void present_choice(game::ChronicleChoice value) noexcept;
    void preview_choice(game::ChronicleChoice value, bool confirmed) noexcept {
        if(confirmed) {present_choice(value);transition_=2.8f;}
        else {pending_=value;decision_enabled=true;choice=game::ChronicleChoice::None;}
    }
    game::ChronicleChoice take_choice() noexcept {
        const auto result=requested_; requested_=game::ChronicleChoice::None; return result;
    }
    static std::vector<std::string_view> strings();
private:
    int chapter_ = 0;
    int ending_ = 0;
    int appendix_ = 0;
    bool preview_emphasis_ = false;
    game::ChronicleChoice pending_ = game::ChronicleChoice::None;
    float transition_ = -1.0f;
    float scroll_ = 0, max_scroll_ = 0;
    game::ChronicleChoice requested_=game::ChronicleChoice::None;
};
}
#endif
