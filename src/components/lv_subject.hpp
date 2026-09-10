#ifndef LILYGO_UI_TEMPLATE_LV_SUBJECT_HPP
#define LILYGO_UI_TEMPLATE_LV_SUBJECT_HPP

#include <lvgl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

class IntSubject {
public:
    explicit IntSubject(std::int32_t value, std::int32_t minimum = std::numeric_limits<std::int32_t>::min(),
                        std::int32_t maximum = std::numeric_limits<std::int32_t>::max())
    {
        lv_subject_init_int(&subject_, value);
        lv_subject_set_min_value_int(&subject_, minimum);
        lv_subject_set_max_value_int(&subject_, maximum);
    }

    ~IntSubject() noexcept
    {
        lv_subject_deinit(&subject_);
    }

    IntSubject(const IntSubject &)            = delete;
    IntSubject &operator=(const IntSubject &) = delete;
    IntSubject(IntSubject &&)                 = delete;
    IntSubject &operator=(IntSubject &&)      = delete;

    void set(std::int32_t value) noexcept
    {
        lv_subject_set_int(&subject_, value);
    }

    [[nodiscard]] std::int32_t value() const noexcept
    {
        return lv_subject_get_int(const_cast<lv_subject_t *>(&subject_));
    }

    [[nodiscard]] lv_subject_t *get() noexcept
    {
        return &subject_;
    }

private:
    lv_subject_t subject_{};
};

template <std::size_t Capacity>
class StringSubject {
    static_assert(Capacity > 0, "A StringSubject needs storage for a terminator");
    static_assert(Capacity <= 0x00ffffffU, "LVGL string Subject capacity exceeds its 24-bit size field");

public:
    explicit StringSubject(std::string_view value = {})
    {
        std::array<char, Capacity> initial{};
        copy(initial, value);
        lv_subject_init_string(&subject_, current_.data(), previous_.data(), current_.size(), initial.data());
    }

    ~StringSubject() noexcept
    {
        lv_subject_deinit(&subject_);
    }

    StringSubject(const StringSubject &)            = delete;
    StringSubject &operator=(const StringSubject &) = delete;
    StringSubject(StringSubject &&)                 = delete;
    StringSubject &operator=(StringSubject &&)      = delete;

    void set(std::string_view value) noexcept
    {
        std::array<char, Capacity> next{};
        copy(next, value);
        lv_subject_copy_string(&subject_, next.data());
    }

    [[nodiscard]] const char *value() const noexcept
    {
        return lv_subject_get_string(const_cast<lv_subject_t *>(&subject_));
    }

    [[nodiscard]] lv_subject_t *get() noexcept
    {
        return &subject_;
    }

private:
    static void copy(std::array<char, Capacity> &destination, std::string_view source) noexcept
    {
        const std::size_t null_position = source.find('\0');
        if (null_position != std::string_view::npos) source = source.substr(0, null_position);
        const std::size_t length = std::min(source.size(), destination.size() - 1U);
        if (length != 0U) std::memcpy(destination.data(), source.data(), length);
        destination[length] = '\0';
    }

    lv_subject_t subject_{};
    std::array<char, Capacity> current_{};
    std::array<char, Capacity> previous_{};
};

#endif
