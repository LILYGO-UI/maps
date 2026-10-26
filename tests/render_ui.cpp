#include "app.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kPartialRows = 80;

std::vector<lv_color32_t> framebuffer;
int display_width = 0;

void flush_display(lv_display_t *display, const lv_area_t *area, std::uint8_t *pixels)
{
    const int width    = area->x2 - area->x1 + 1;
    const auto *source = reinterpret_cast<const lv_color32_t *>(pixels);
    for (int y = area->y1; y <= area->y2; ++y) {
        const auto source_offset = static_cast<std::size_t>(y - area->y1) * static_cast<std::size_t>(width);
        const auto destination_offset =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(display_width) + static_cast<std::size_t>(area->x1);
        std::copy_n(source + source_offset, width, framebuffer.begin() + destination_offset);
    }
    lv_display_flush_ready(display);
}

bool write_ppm(const char *path, int width, int height)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (const auto &pixel : framebuffer) {
        file.put(static_cast<char>(pixel.red));
        file.put(static_cast<char>(pixel.green));
        file.put(static_cast<char>(pixel.blue));
    }
    return file.good();
}

bool intersects(lv_obj_t *left, lv_obj_t *right)
{
    lv_area_t left_area{};
    lv_area_t right_area{};
    lv_obj_get_coords(left, &left_area);
    lv_obj_get_coords(right, &right_area);
    return !(left_area.x2 < right_area.x1 || right_area.x2 < left_area.x1 || left_area.y2 < right_area.y1 ||
             right_area.y2 < left_area.y1);
}

bool contains(lv_obj_t *parent, lv_obj_t *child)
{
    lv_area_t parent_area{};
    lv_area_t child_area{};
    lv_obj_get_coords(parent, &parent_area);
    lv_obj_get_coords(child, &child_area);
    return child_area.x1 >= parent_area.x1 && child_area.y1 >= parent_area.y1 &&
           child_area.x2 <= parent_area.x2 && child_area.y2 <= parent_area.y2;
}

void pump(int milliseconds, bool real_time = false)
{
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 20) {
        lv_tick_inc(20);
        lv_timer_handler();
        if (real_time) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

class KeypadInput {
public:
    KeypadInput(lv_obj_t *input, lv_obj_t *next)
    {
        group = lv_group_create();
        indev = lv_indev_create();
        assert(group && indev);
        lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_group(indev, group);
        lv_indev_set_user_data(indev, this);
        lv_indev_set_read_cb(indev, [](lv_indev_t *device, lv_indev_data_t *data) {
            auto *keypad = static_cast<KeypadInput *>(lv_indev_get_user_data(device));
            data->key   = keypad->key;
            data->state = keypad->state;
        });
        lv_group_add_obj(group, input);
        lv_group_add_obj(group, next);
        lv_group_focus_obj(input);
    }

    ~KeypadInput()
    {
        lv_indev_delete(indev);
        lv_group_delete(group);
    }

    void press(std::uint32_t value)
    {
        key   = value;
        state = LV_INDEV_STATE_PRESSED;
        lv_indev_read(indev);
        state = LV_INDEV_STATE_RELEASED;
        lv_indev_read(indev);
    }

    lv_group_t *group = nullptr;

private:
    lv_indev_t *indev = nullptr;
    std::uint32_t key = 0;
    lv_indev_state_t state = LV_INDEV_STATE_RELEASED;
};

}  // namespace

int main(int argc, char **argv)
{
    assert(argc == 4);
    const int width  = std::atoi(argv[1]);
    const int height = std::atoi(argv[2]);
    assert(width >= 320 && height >= 240);
    const char *online_value = std::getenv("LILYGO_MAPS_RENDER_ONLINE");
    const bool online        = online_value && std::string(online_value) == "1";
    const char *online_query = std::getenv("LILYGO_MAPS_RENDER_QUERY");
    if (!online) ::setenv("LILYGO_MAPS_DISABLE_NETWORK", "1", 1);
    ::setenv("LILYGO_MAPS_DISABLE_GNSS", "1", 1);
    ::setenv("LILYGO_MAPS_CACHE_DIR", "/tmp/lilygo-maps-render-cache/openstreetmap", 1);
    std::error_code ignored;
    if (!online) std::filesystem::remove_all("/tmp/lilygo-maps-render-cache", ignored);

    display_width = width;
    framebuffer.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::vector<lv_color32_t> draw_buffer(static_cast<std::size_t>(width) * kPartialRows);

    lv_init();
    auto *display = lv_display_create(width, height);
    assert(display);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                           static_cast<std::uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    lv_display_set_default(display);

    cm0_app_context_t context{};
    context.root           = lv_screen_active();
    const auto *descriptor = cm0_app_get_descriptor();
    descriptor->open(&context);
    lv_obj_update_layout(context.root);
    pump(online ? (online_query && *online_query ? 3000 : 12000) : 200, online);

    auto *surface       = lv_obj_find_by_name(context.root, "map_surface");
    auto *header        = lv_obj_find_by_name(context.root, "app_header");
    auto *title         = lv_obj_find_by_name(context.root, "app_title");
    auto *search_band   = lv_obj_find_by_name(context.root, "map_search_band");
    auto *search_input  = lv_obj_find_by_name(context.root, "map_search_input");
    auto *search_button = lv_obj_find_by_name(context.root, "map_search_button");
    auto *viewport      = lv_obj_find_by_name(context.root, "map_viewport");
    auto *tile_layer    = lv_obj_find_by_name(context.root, "map_tile_layer");
    auto *marker        = lv_obj_find_by_name(context.root, "map_center_marker");
    auto *zoom_controls = lv_obj_find_by_name(context.root, "map_zoom_controls");
    auto *zoom_in       = lv_obj_find_by_name(context.root, "map_zoom_in");
    auto *zoom_out      = lv_obj_find_by_name(context.root, "map_zoom_out");
    auto *locate        = lv_obj_find_by_name(context.root, "map_locate_button");
    auto *locate_hint   = lv_obj_find_by_name(context.root, "map_locate_hint");
    auto *status        = lv_obj_find_by_name(context.root, "map_status_panel");
    auto *attribution   = lv_obj_find_by_name(context.root, "map_attribution");
    auto *cache_button  = lv_obj_find_by_name(context.root, "map_cache_button");
    auto *confirmation  = lv_obj_find_by_name(context.root, "map_cache_confirmation");
    auto *keyboard      = lv_obj_find_by_name(context.root, "map_keyboard");
    assert(!header && !title);
    assert(surface && search_band && search_input && search_button && viewport && tile_layer &&
           marker && zoom_controls && zoom_in && zoom_out && locate && locate_hint && status && attribution &&
           cache_button && confirmation && keyboard);
    const auto verify_layout = [&](int expected_width, int expected_height) {
        assert(lv_obj_get_width(viewport) == expected_width);
        assert(lv_obj_get_height(viewport) == expected_height - 40);
        assert(lv_obj_get_height(viewport) == lv_obj_get_content_height(surface));
        assert(lv_obj_get_parent(search_band) == viewport);
        assert(lv_obj_get_index(search_band) > lv_obj_get_index(tile_layer));
        assert(lv_obj_get_style_bg_opa(search_band, LV_PART_MAIN) == LV_OPA_TRANSP);
        assert(contains(viewport, search_band));
        assert(contains(search_band, search_input));
        assert(contains(search_band, search_button));
        assert(intersects(search_input, tile_layer));
        bool tiles_under_search = false;
        for (std::uint32_t index = 0; index < lv_obj_get_child_count(tile_layer); ++index) {
            tiles_under_search |= intersects(search_input, lv_obj_get_child(tile_layer, index));
        }
        assert(tiles_under_search);
        for (auto *control : {zoom_controls, locate, cache_button}) {
            assert(lv_obj_is_visible(control));
            assert(contains(viewport, control));
            assert(!intersects(search_band, control));
            assert(!intersects(control, status));
            assert(!intersects(control, attribution));
        }
        assert(!intersects(search_input, search_button));
        assert(!intersects(status, attribution));
        assert(!intersects(locate, zoom_controls));
        assert(!intersects(cache_button, zoom_controls));
        assert(!intersects(cache_button, locate));
    };
    verify_layout(width, height);
    assert(lv_obj_has_flag(locate_hint, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == nullptr);
    assert(lv_obj_get_child_count(tile_layer) > 0);

    if (online) {
        if (online_query && *online_query) {
            lv_textarea_set_text(search_input, online_query);
            lv_obj_send_event(search_button, LV_EVENT_CLICKED, nullptr);
            pump(12000, true);
        }
        lv_refr_now(display);
        assert(write_ppm(argv[3], width, height));
        descriptor->close();
        lv_display_delete(display);
        lilygo_ui_fonts_deinit();
        lv_deinit();
        return 0;
    }

    const auto before = std::string(lv_label_get_text(lv_obj_get_child(zoom_in, 0)));
    assert(!before.empty());
    lv_obj_send_event(zoom_in, LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(zoom_out, LV_EVENT_CLICKED, nullptr);

    lv_textarea_set_text(search_input, "   ");
    lv_obj_send_event(search_button, LV_EVENT_CLICKED, nullptr);
    auto *status_label = lv_obj_find_by_name(context.root, "map_status_label");
    assert(status_label);
    assert(std::string(lv_label_get_text(status_label)) == "Enter an address to search");

    {
        KeypadInput keypad(search_input, search_button);
        lv_textarea_set_text(search_input, "");
        for (const char character : std::string("Pais")) keypad.press(character);
        assert(std::string(lv_textarea_get_text(search_input)) == "Pais");
        assert(lv_obj_has_state(search_input, LV_STATE_FOCUSED));
        assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        assert(lv_keyboard_get_textarea(keyboard) == nullptr);
        keypad.press(LV_KEY_LEFT);
        keypad.press(LV_KEY_LEFT);
        assert(lv_textarea_get_cursor_pos(search_input) == 2);
        keypad.press('r');
        keypad.press(LV_KEY_RIGHT);
        keypad.press(LV_KEY_RIGHT);
        assert(lv_textarea_get_cursor_pos(search_input) == 5);
        for (const char character : std::string(" 2x")) keypad.press(character);
        keypad.press(LV_KEY_BACKSPACE);
        assert(std::string(lv_textarea_get_text(search_input)) == "Paris 2");

        // Resize the existing page in both directions while an editor owns focus.
        // The pixel count stays constant, so the existing draw buffer can be reused.
        for (const bool rotated : {true, false}) {
            const int resized_width  = rotated ? height : width;
            const int resized_height = rotated ? width : height;
            display_width = resized_width;
            lv_display_set_resolution(display, resized_width, resized_height);
            lv_obj_update_layout(context.root);
            pump(100);
            verify_layout(resized_width, resized_height);
            assert(std::string(lv_textarea_get_text(search_input)) == "Paris 2");
            assert(lv_group_get_focused(keypad.group) == search_input);
            assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        }

        unsigned ready_count = 0;
        auto *ready_observer = lv_obj_add_event_cb(search_input, [](lv_event_t *event) {
            ++*static_cast<unsigned *>(lv_event_get_user_data(event));
        }, LV_EVENT_READY, &ready_count);
        keypad.press(LV_KEY_ENTER);
        assert(ready_count == 1);
        assert(std::string(lv_label_get_text(status_label)) == "Searching for address...");
        assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        assert(lv_keyboard_get_textarea(keyboard) == nullptr);
        for (int index = 0; index < 7; ++index) keypad.press(LV_KEY_BACKSPACE);
        assert(std::string(lv_textarea_get_text(search_input)).empty());
        keypad.press(LV_KEY_ENTER);
        assert(ready_count == 2);
        assert(std::string(lv_label_get_text(status_label)) == "Enter an address to search");
        lv_obj_remove_event_dsc(search_input, ready_observer);

        // Pointer reactivation restores software input even when focus never moved.
        lv_obj_send_event(search_input, LV_EVENT_CLICKED, nullptr);
        assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        assert(lv_keyboard_get_textarea(keyboard) == search_input);
        lv_obj_send_event(search_input, LV_EVENT_CANCEL, nullptr);
        assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        assert(lv_keyboard_get_textarea(keyboard) == nullptr);
        lv_obj_send_event(search_input, LV_EVENT_CLICKED, nullptr);
        assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        keypad.press(LV_KEY_NEXT);
        assert(lv_group_get_focused(keypad.group) == search_button);
        assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        assert(lv_keyboard_get_textarea(keyboard) == nullptr);
        keypad.press(LV_KEY_PREV);
        assert(lv_group_get_focused(keypad.group) == search_input);
        assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        assert(lv_keyboard_get_textarea(keyboard) == nullptr);
    }

    lv_obj_send_event(search_input, LV_EVENT_CLICKED, nullptr);
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == search_input);
    lv_textarea_set_text(search_input, "London");
    lv_obj_send_event(keyboard, LV_EVENT_READY, nullptr);
    assert(std::string(lv_label_get_text(status_label)) == "Searching for address...");
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == nullptr);
    lv_obj_send_event(search_input, LV_EVENT_CLICKED, nullptr);
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == search_input);
    lv_obj_send_event(keyboard, LV_EVENT_CANCEL, nullptr);
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == nullptr);
    assert(std::string(lv_textarea_get_text(search_input)) == "London");
    lv_textarea_set_text(search_input, "");

    assert(std::string(lv_label_get_text(locate_hint)) == "Return to current location");
    for (const auto hide_event : {LV_EVENT_HOVER_LEAVE, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_send_event(locate, LV_EVENT_HOVER_OVER, nullptr);
        lv_obj_update_layout(context.root);
        assert(lv_obj_is_visible(locate_hint));
        assert(contains(viewport, locate_hint));
        assert(!intersects(locate_hint, locate));
        lv_obj_send_event(locate, hide_event, nullptr);
        assert(lv_obj_has_flag(locate_hint, LV_OBJ_FLAG_HIDDEN));
    }
    lv_obj_send_event(locate, LV_EVENT_LONG_PRESSED, nullptr);
    assert(lv_obj_is_visible(locate_hint));
    lv_obj_send_event(search_input, LV_EVENT_FOCUSED, nullptr);
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == search_input);
    lv_obj_send_event(locate, LV_EVENT_CLICKED, nullptr);
    assert(lv_obj_has_flag(locate_hint, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == nullptr);
    assert(std::string(lv_label_get_text(status_label)) == "Finding your current location...");

    lv_obj_send_event(cache_button, LV_EVENT_CLICKED, nullptr);
    assert(!lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    auto *cancel = lv_obj_find_by_name(context.root, "map_cache_cancel");
    assert(cancel);
    lv_obj_send_event(cancel, LV_EVENT_CLICKED, nullptr);
    assert(lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(cache_button, LV_EVENT_CLICKED, nullptr);
    auto *confirm = lv_obj_find_by_name(context.root, "map_cache_confirm");
    assert(confirm);
    lv_obj_send_event(confirm, LV_EVENT_CLICKED, nullptr);
    pump(200);
    assert(lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));

    lv_refr_now(display);
    const bool has_contrast = std::any_of(framebuffer.cbegin() + 1, framebuffer.cend(), [](const auto &pixel) {
        return pixel.red != framebuffer[0].red || pixel.green != framebuffer[0].green ||
               pixel.blue != framebuffer[0].blue;
    });
    assert(has_contrast);
    assert(write_ppm(argv[3], width, height));

    descriptor->close();
    lv_display_delete(display);
    lilygo_ui_fonts_deinit();
    lv_deinit();
    std::filesystem::remove_all("/tmp/lilygo-maps-render-cache", ignored);
    return 0;
}
