#include "pages/map/map_view.hpp"

#include "components/components.hpp"

#include <cm0/typography.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr const char *kSearchSymbol = "\xEF\x80\x82";
constexpr int kHomeIndicatorSpace   = 40;

void make_plain(lv_obj_t *object, std::uint32_t background = 0, lv_opa_t opacity = LV_OPA_TRANSP)
{
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, int size, std::uint32_t color)
{
    auto *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, lilygo_ui_font_get(size), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

lv_obj_t *make_icon_button(lv_obj_t *parent, const char *symbol, const char *name, lv_event_cb_t callback,
                           void *user_data, std::uint32_t foreground = components::color_action,
                           std::uint32_t background = components::color_container_background)
{
    auto *button = lv_button_create(parent);
    lv_obj_set_name_static(button, name);
    lv_obj_set_size(button, 52, 52);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    auto *label = make_label(button, symbol, 22, foreground);
    lv_obj_center(label);
    return button;
}

}  // namespace

MapView::MapView(MapViewModel &view_model) noexcept : view_model_(view_model)
{
}

MapView::~MapView() noexcept
{
    destroy();
}

lv_obj_t *MapView::create(lv_obj_t *parent, const char *application_name)
{
    if (!parent) return nullptr;
    destroy();

    surface_ = components::create_page(parent);
    lv_obj_set_name_static(surface_, "map_surface");
    lv_obj_set_style_pad_bottom(surface_, kHomeIndicatorSpace, 0);
    lv_obj_add_event_cb(surface_, size_changed_event, LV_EVENT_SIZE_CHANGED, this);

    header_ = components::create_header(surface_, application_name);
    auto *cache_button = make_icon_button(header_, LV_SYMBOL_TRASH, "map_cache_button", show_clear_event, this,
                                          components::color_destructive);
    lv_obj_align(cache_button, LV_ALIGN_RIGHT_MID, 0, 0);

    search_band_ = lv_obj_create(surface_);
    lv_obj_set_name_static(search_band_, "map_search_band");
    lv_obj_set_width(search_band_, LV_PCT(100));
    lv_obj_set_height(search_band_, 72);
    make_plain(search_band_, components::color_container_background, LV_OPA_COVER);
    lv_obj_set_style_border_side(search_band_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(search_band_, 1, 0);
    lv_obj_set_style_border_color(search_band_, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_pad_hor(search_band_, 16, 0);
    lv_obj_set_style_pad_ver(search_band_, 9, 0);

    search_inner_ = lv_obj_create(search_band_);
    lv_obj_set_height(search_inner_, 52);
    make_plain(search_inner_);
    lv_obj_set_style_pad_column(search_inner_, 8, 0);
    lv_obj_set_flex_flow(search_inner_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(search_inner_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(search_inner_);

    search_input_ = lv_textarea_create(search_inner_);
    lv_obj_set_name_static(search_input_, "map_search_input");
    lv_obj_set_height(search_input_, 52);
    lv_obj_set_flex_grow(search_input_, 1);
    lv_obj_set_style_radius(search_input_, 6, 0);
    lv_obj_set_style_border_width(search_input_, 1, 0);
    lv_obj_set_style_border_color(search_input_, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_bg_color(search_input_, lv_color_hex(components::color_page_background), 0);
    lv_obj_set_style_bg_opa(search_input_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(search_input_, lilygo_ui_font_get(22), 0);
    lv_obj_set_style_text_color(search_input_, lv_color_hex(components::color_text), 0);
    lv_textarea_set_one_line(search_input_, true);
    lv_textarea_set_max_length(search_input_, 160);
    lv_textarea_set_placeholder_text(search_input_, "Search address");
    lv_obj_add_event_cb(search_input_, search_focus_event, LV_EVENT_FOCUSED, this);

    auto *search_button = make_icon_button(search_inner_, kSearchSymbol, "map_search_button", search_event, this,
                                           components::color_container_background, components::color_action);
    lv_obj_set_size(search_button, 58, 52);

    map_viewport_ = lv_obj_create(surface_);
    lv_obj_set_name_static(map_viewport_, "map_viewport");
    lv_obj_set_width(map_viewport_, LV_PCT(100));
    lv_obj_set_height(map_viewport_, 0);
    lv_obj_set_flex_grow(map_viewport_, 1);
    make_plain(map_viewport_, components::color_map_background, LV_OPA_COVER);
    lv_obj_add_flag(map_viewport_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map_viewport_, pan_event, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(map_viewport_, pan_event, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(map_viewport_, pan_event, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(map_viewport_, pan_event, LV_EVENT_PRESS_LOST, this);

    tile_layer_ = lv_obj_create(map_viewport_);
    lv_obj_set_name_static(tile_layer_, "map_tile_layer");
    lv_obj_set_size(tile_layer_, LV_PCT(100), LV_PCT(100));
    make_plain(tile_layer_);
    lv_obj_clear_flag(tile_layer_, LV_OBJ_FLAG_CLICKABLE);

    auto *marker = lv_obj_create(map_viewport_);
    lv_obj_set_name_static(marker, "map_center_marker");
    lv_obj_set_size(marker, 20, 20);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(marker, 4, 0);
    lv_obj_set_style_border_color(marker, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_color(marker, lv_color_hex(components::color_action), 0);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(marker, 8, 0);
    lv_obj_set_style_shadow_opa(marker, LV_OPA_30, 0);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(marker, LV_ALIGN_CENTER, 0, 0);

    zoom_controls_ = lv_obj_create(map_viewport_);
    lv_obj_set_name_static(zoom_controls_, "map_zoom_controls");
    lv_obj_set_size(zoom_controls_, 52, 105);
    lv_obj_set_style_radius(zoom_controls_, 6, 0);
    lv_obj_set_style_border_width(zoom_controls_, 1, 0);
    lv_obj_set_style_border_color(zoom_controls_, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_bg_color(zoom_controls_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_opa(zoom_controls_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(zoom_controls_, 0, 0);
    lv_obj_set_style_pad_row(zoom_controls_, 1, 0);
    lv_obj_set_flex_flow(zoom_controls_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(zoom_controls_, LV_OBJ_FLAG_SCROLLABLE);
    auto *zoom_in = make_icon_button(zoom_controls_, LV_SYMBOL_PLUS, "map_zoom_in", zoom_in_event, this);
    auto *zoom_out = make_icon_button(zoom_controls_, LV_SYMBOL_MINUS, "map_zoom_out", zoom_out_event, this);
    lv_obj_set_style_border_width(zoom_in, 0, 0);
    lv_obj_set_style_border_width(zoom_out, 0, 0);
    lv_obj_align(zoom_controls_, LV_ALIGN_TOP_RIGHT, -12, 12);

    locate_button_ = make_icon_button(map_viewport_, LV_SYMBOL_GPS, "map_locate_button", locate_event, this,
                                      components::color_container_background, components::color_action);
    lv_obj_align(locate_button_, LV_ALIGN_TOP_RIGHT, -12, 128);

    locate_hint_ = make_label(map_viewport_, "Return to IP location", 14, components::color_text);
    lv_obj_set_name_static(locate_hint_, "map_locate_hint");
    lv_obj_set_style_bg_color(locate_hint_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_opa(locate_hint_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(locate_hint_, 1, 0);
    lv_obj_set_style_border_color(locate_hint_, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_radius(locate_hint_, 6, 0);
    lv_obj_set_style_pad_all(locate_hint_, 8, 0);
    lv_obj_clear_flag(locate_hint_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(locate_hint_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(locate_button_, locate_hint_event, LV_EVENT_ALL, this);

    map_status_ = lv_obj_create(map_viewport_);
    lv_obj_set_name_static(map_status_, "map_status_panel");
    lv_obj_set_height(map_status_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(map_status_, 6, 0);
    lv_obj_set_style_border_width(map_status_, 1, 0);
    lv_obj_set_style_border_color(map_status_, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_bg_color(map_status_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_opa(map_status_, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(map_status_, 10, 0);
    lv_obj_set_style_pad_row(map_status_, 3, 0);
    lv_obj_set_flex_flow(map_status_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(map_status_, LV_OBJ_FLAG_SCROLLABLE);
    auto *location = make_label(map_status_, "", 14, components::color_container_title);
    lv_obj_set_name_static(location, "map_location_label");
    lv_obj_set_width(location, LV_PCT(100));
    lv_label_set_long_mode(location, LV_LABEL_LONG_DOT);
    lv_label_bind_text(location, view_model_.location_subject(), nullptr);
    auto *status = make_label(map_status_, "", 14, components::color_muted_text);
    lv_obj_set_name_static(status, "map_status_label");
    lv_obj_set_width(status, LV_PCT(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_label_bind_text(status, view_model_.status_subject(), nullptr);
    auto *meta = lv_obj_create(map_status_);
    lv_obj_set_size(meta, LV_PCT(100), 20);
    make_plain(meta);
    auto *zoom = make_label(meta, "", 14, components::color_muted_text);
    lv_label_bind_text(zoom, view_model_.zoom_subject(), "Z%d");
    lv_obj_align(zoom, LV_ALIGN_LEFT_MID, 0, 0);
    auto *cache = make_label(meta, "", 14, components::color_muted_text);
    lv_obj_set_name_static(cache, "map_cache_label");
    lv_label_bind_text(cache, view_model_.cache_subject(), nullptr);
    lv_obj_align(cache, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_align(map_status_, LV_ALIGN_BOTTOM_LEFT, 12, -12);

    attribution_ = make_label(map_viewport_, "© OpenStreetMap contributors", 14, components::color_muted_text);
    lv_obj_set_name_static(attribution_, "map_attribution");
    lv_obj_set_style_bg_color(attribution_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_opa(attribution_, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(attribution_, 6, 0);
    lv_obj_set_style_pad_ver(attribution_, 3, 0);

    keyboard_ = lv_keyboard_create(surface_);
    lv_obj_set_name_static(keyboard_, "map_keyboard");
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(keyboard_, LV_PCT(100));
    lv_obj_set_height(keyboard_, 280);
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(components::color_control_background), 0);
    lv_obj_set_style_border_width(keyboard_, 0, 0);
    lv_obj_set_style_radius(keyboard_, 0, 0);
    lv_obj_set_style_text_font(keyboard_, lilygo_ui_font_get(22), LV_PART_ITEMS);
    lv_keyboard_set_textarea(keyboard_, search_input_);
    lv_obj_add_event_cb(keyboard_, keyboard_event, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, keyboard_event, LV_EVENT_CANCEL, this);
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);

    confirmation_ = lv_obj_create(surface_);
    lv_obj_set_name_static(confirmation_, "map_cache_confirmation");
    lv_obj_add_flag(confirmation_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(confirmation_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(confirmation_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(confirmation_, LV_PCT(100), LV_PCT(100));
    make_plain(confirmation_, 0x000000, LV_OPA_40);

    dialog_ = components::create_card(confirmation_);
    lv_obj_set_name_static(dialog_, "map_cache_dialog");
    lv_obj_set_height(dialog_, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(dialog_, 24, 0);
    lv_obj_set_style_pad_row(dialog_, 14, 0);
    lv_obj_set_flex_flow(dialog_, LV_FLEX_FLOW_COLUMN);
    lv_obj_center(dialog_);
    auto *dialog_title = make_label(dialog_, "Clear map data?", 28, components::color_container_title);
    auto *dialog_copy = make_label(dialog_, "Downloaded tiles will be removed from this device.", 22,
                                   components::color_muted_text);
    lv_obj_set_width(dialog_copy, LV_PCT(100));
    lv_label_set_long_mode(dialog_copy, LV_LABEL_LONG_WRAP);
    auto *dialog_cache = make_label(dialog_, "", 14, components::color_muted_text);
    lv_label_bind_text(dialog_cache, view_model_.cache_subject(), nullptr);
    auto *actions = lv_obj_create(dialog_);
    lv_obj_set_size(actions, LV_PCT(100), 54);
    make_plain(actions);
    lv_obj_set_style_pad_column(actions, 10, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    auto *cancel = components::create_button(actions, "Cancel", cancel_clear_event, this);
    lv_obj_set_name_static(cancel, "map_cache_cancel");
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(components::color_control_background), 0);
    auto *cancel_label = lv_obj_get_child(cancel, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(components::color_text), 0);
    auto *confirm = components::create_button(actions, "Clear", confirm_clear_event, this);
    lv_obj_set_name_static(confirm, "map_cache_confirm");
    lv_obj_set_flex_grow(confirm, 1);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(components::color_destructive), 0);
    (void)dialog_title;

    timer_ = lv_timer_create(timer_event, 80, this);
    lv_obj_update_layout(surface_);
    apply_layout();
    lv_obj_update_layout(surface_);
    render_tiles();
    return surface_;
}

void MapView::destroy() noexcept
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    clear_tile_widgets();
    if (surface_) lv_obj_delete(surface_);
    surface_       = nullptr;
    header_        = nullptr;
    search_band_   = nullptr;
    search_inner_  = nullptr;
    search_input_  = nullptr;
    map_viewport_  = nullptr;
    tile_layer_    = nullptr;
    map_status_    = nullptr;
    attribution_   = nullptr;
    zoom_controls_ = nullptr;
    locate_button_ = nullptr;
    locate_hint_   = nullptr;
    keyboard_      = nullptr;
    confirmation_  = nullptr;
    dialog_        = nullptr;
    last_map_width_ = 0;
    last_map_height_ = 0;
}

void MapView::size_changed_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->apply_layout();
    if (!view->map_viewport_) return;
    lv_obj_update_layout(view->map_viewport_);
    const int width  = lv_obj_get_content_width(view->map_viewport_);
    const int height = lv_obj_get_content_height(view->map_viewport_);
    if (width > 0 && height > 0 && (width != view->last_map_width_ || height != view->last_map_height_)) {
        view->render_tiles();
    }
}

void MapView::search_focus_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (view) view->show_keyboard();
}

void MapView::search_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (view) view->submit_search();
}

void MapView::keyboard_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (!view) return;
    if (lv_event_get_code(event) == LV_EVENT_READY) view->submit_search();
    else view->hide_keyboard();
}

void MapView::zoom_in_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->view_model_.zoom_in();
    view->render_tiles();
}

void MapView::zoom_out_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->view_model_.zoom_out();
    view->render_tiles();
}

void MapView::locate_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->hide_keyboard();
    view->view_model_.return_to_ip_location();
    view->render_tiles();
}

void MapView::locate_hint_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (!view || !view->locate_hint_) return;
    switch (lv_event_get_code(event)) {
        case LV_EVENT_HOVER_OVER:
        case LV_EVENT_LONG_PRESSED:
            lv_obj_clear_flag(view->locate_hint_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_update_layout(view->map_viewport_);
            lv_obj_align_to(view->locate_hint_, view->locate_button_, LV_ALIGN_OUT_LEFT_MID, -8, 0);
            lv_obj_move_foreground(view->locate_hint_);
            break;
        case LV_EVENT_HOVER_LEAVE:
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
        case LV_EVENT_CLICKED:
            lv_obj_add_flag(view->locate_hint_, LV_OBJ_FLAG_HIDDEN);
            break;
        default:
            break;
    }
}

void MapView::pan_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (!view || !view->tile_layer_) return;
    auto *input = lv_indev_active();
    if (!input) return;
    lv_point_t point{};
    lv_indev_get_point(input, &point);
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        view->press_point_ = point;
        view->drag_x_      = 0;
        view->drag_y_      = 0;
        view->dragging_    = true;
        return;
    }
    if (!view->dragging_) return;
    view->drag_x_ = point.x - view->press_point_.x;
    view->drag_y_ = point.y - view->press_point_.y;
    if (code == LV_EVENT_PRESSING) {
        lv_obj_set_pos(view->tile_layer_, view->drag_x_, view->drag_y_);
        return;
    }
    view->dragging_ = false;
    lv_obj_set_pos(view->tile_layer_, 0, 0);
    if (std::abs(view->drag_x_) < 4 && std::abs(view->drag_y_) < 4) return;
    view->view_model_.pan_pixels(-view->drag_x_, -view->drag_y_);
    view->render_tiles();
}

void MapView::show_clear_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (!view || !view->confirmation_) return;
    view->hide_keyboard();
    lv_obj_clear_flag(view->confirmation_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(view->confirmation_);
}

void MapView::cancel_clear_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (view && view->confirmation_) lv_obj_add_flag(view->confirmation_, LV_OBJ_FLAG_HIDDEN);
}

void MapView::confirm_clear_event(lv_event_t *event)
{
    auto *view = static_cast<MapView *>(lv_event_get_user_data(event));
    if (!view) return;
    if (view->confirmation_) lv_obj_add_flag(view->confirmation_, LV_OBJ_FLAG_HIDDEN);
    view->view_model_.clear_cache();
}

void MapView::timer_event(lv_timer_t *timer)
{
    auto *view = static_cast<MapView *>(lv_timer_get_user_data(timer));
    if (view) view->process_updates();
}

void MapView::apply_layout() noexcept
{
    if (!surface_ || !header_ || !search_band_ || !search_inner_ || !map_status_ || !attribution_ || !dialog_ ||
        !keyboard_) {
        return;
    }
    const int width       = lv_obj_get_content_width(surface_);
    const int height      = lv_obj_get_content_height(surface_);
    const bool landscape  = width > height;
    const bool compact    = width < 420;
    const bool wide       = width >= 800;
    lv_obj_set_height(header_, landscape ? 64 : 76);
    lv_obj_set_height(search_band_, compact ? 64 : 70);
    lv_obj_set_width(search_inner_, wide ? 760 : LV_PCT(100));
    lv_obj_set_width(map_status_, wide ? 440 : (compact ? LV_PCT(72) : LV_PCT(64)));
    lv_obj_align(attribution_, wide || landscape ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT,
                 wide || landscape ? -12 : 12, wide || landscape ? -12 : -110);
    lv_obj_set_width(dialog_, compact ? LV_PCT(90) : 480);
    lv_obj_set_height(keyboard_, landscape ? std::min(240, std::max(180, height - 180)) : 280);
}

void MapView::submit_search()
{
    if (!search_input_) return;
    view_model_.search(lv_textarea_get_text(search_input_));
    hide_keyboard();
}

void MapView::show_keyboard()
{
    if (!keyboard_ || !search_input_) return;
    if (locate_hint_) lv_obj_add_flag(locate_hint_, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(keyboard_, search_input_);
    lv_obj_clear_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keyboard_);
}

void MapView::hide_keyboard()
{
    if (!keyboard_) return;
    lv_keyboard_set_textarea(keyboard_, nullptr);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
}

void MapView::render_tiles(bool download)
{
    if (!map_viewport_ || !tile_layer_) return;
    lv_obj_update_layout(map_viewport_);
    const int width  = lv_obj_get_content_width(map_viewport_);
    const int height = lv_obj_get_content_height(map_viewport_);
    if (width <= 0 || height <= 0) return;
    last_map_width_  = width;
    last_map_height_ = height;
    clear_tile_widgets();
    lv_obj_set_pos(tile_layer_, 0, 0);

    const auto tiles = view_model_.prepare_viewport(width, height, download);
    for (const auto &visible : tiles) {
        tile_slots_.push_back({visible.id, nullptr, {}});
        auto &slot      = tile_slots_.back();
        slot.container  = lv_obj_create(tile_layer_);
        lv_obj_set_size(slot.container, MapModel::tile_size, MapModel::tile_size);
        lv_obj_set_pos(slot.container, visible.screen_x, visible.screen_y);
        lv_obj_set_style_radius(slot.container, 0, 0);
        lv_obj_set_style_border_width(slot.container, 1, 0);
        lv_obj_set_style_border_color(slot.container, lv_color_hex(components::color_border), 0);
        lv_obj_set_style_bg_color(slot.container,
                                  lv_color_hex((visible.id.x + visible.id.y) % 2 == 0 ? components::color_map_tile_a
                                                                                     : components::color_map_tile_b),
                                  0);
        lv_obj_set_style_bg_opa(slot.container, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(slot.container, 0, 0);
        lv_obj_clear_flag(slot.container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(slot.container, LV_OBJ_FLAG_CLICKABLE);
        std::filesystem::path cached;
        if (view_model_.find_tile(visible.id, cached)) set_tile_source(visible.id, cached);
    }
    lv_obj_move_background(tile_layer_);
}

void MapView::clear_tile_widgets() noexcept
{
    if (tile_layer_) lv_obj_clean(tile_layer_);
    tile_slots_.clear();
}

void MapView::set_tile_source(const TileId &tile, const std::filesystem::path &path)
{
    for (auto &slot : tile_slots_) {
        if (!(slot.tile == tile) || !slot.container || !slot.source.empty()) continue;
        slot.source = "A:" + path.string();
        auto *image = lv_image_create(slot.container);
        lv_obj_set_size(image, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_radius(image, 0, 0);
        lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_src(image, slot.source.c_str());
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_center(image);
    }
}

void MapView::process_updates()
{
    for (int index = 0; index < 12; ++index) {
        MapPresentationEvent event;
        if (!view_model_.poll(event)) break;
        switch (event.kind) {
            case MapPresentationEventKind::ViewportChanged:
                render_tiles();
                break;
            case MapPresentationEventKind::TileReady:
                if (event.generation == view_model_.viewport_generation()) set_tile_source(event.tile, event.path);
                break;
            case MapPresentationEventKind::CacheCleared:
                render_tiles(false);
                break;
            case MapPresentationEventKind::None:
                break;
        }
    }
}
