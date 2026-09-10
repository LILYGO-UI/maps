#ifndef LILYGO_UI_MAPS_PAGES_MAP_MAP_VIEW_HPP
#define LILYGO_UI_MAPS_PAGES_MAP_MAP_VIEW_HPP

#include "pages/map/map_view_model.hpp"

#include <lvgl.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>

class MapView {
public:
    explicit MapView(MapViewModel &view_model) noexcept;
    ~MapView() noexcept;

    lv_obj_t *create(lv_obj_t *parent, const char *application_name);
    void destroy() noexcept;

private:
    struct TileSlot {
        TileId tile;
        lv_obj_t *container = nullptr;
        std::string source;
    };

    static void size_changed_event(lv_event_t *event);
    static void search_focus_event(lv_event_t *event);
    static void search_event(lv_event_t *event);
    static void keyboard_event(lv_event_t *event);
    static void zoom_in_event(lv_event_t *event);
    static void zoom_out_event(lv_event_t *event);
    static void locate_event(lv_event_t *event);
    static void locate_hint_event(lv_event_t *event);
    static void pan_event(lv_event_t *event);
    static void show_clear_event(lv_event_t *event);
    static void cancel_clear_event(lv_event_t *event);
    static void confirm_clear_event(lv_event_t *event);
    static void timer_event(lv_timer_t *timer);

    void apply_layout() noexcept;
    void submit_search();
    void show_keyboard();
    void hide_keyboard();
    void render_tiles(bool download = true);
    void clear_tile_widgets() noexcept;
    void set_tile_source(const TileId &tile, const std::filesystem::path &path);
    void process_updates();

    MapViewModel &view_model_;
    lv_obj_t *surface_       = nullptr;
    lv_obj_t *header_        = nullptr;
    lv_obj_t *search_band_   = nullptr;
    lv_obj_t *search_inner_  = nullptr;
    lv_obj_t *search_input_  = nullptr;
    lv_obj_t *map_viewport_  = nullptr;
    lv_obj_t *tile_layer_    = nullptr;
    lv_obj_t *map_status_    = nullptr;
    lv_obj_t *attribution_   = nullptr;
    lv_obj_t *zoom_controls_ = nullptr;
    lv_obj_t *locate_button_ = nullptr;
    lv_obj_t *locate_hint_   = nullptr;
    lv_obj_t *keyboard_      = nullptr;
    lv_obj_t *confirmation_  = nullptr;
    lv_obj_t *dialog_        = nullptr;
    lv_timer_t *timer_       = nullptr;
    std::deque<TileSlot> tile_slots_;
    lv_point_t press_point_{};
    int drag_x_          = 0;
    int drag_y_          = 0;
    int last_map_width_  = 0;
    int last_map_height_ = 0;
    bool dragging_       = false;
};

#endif
