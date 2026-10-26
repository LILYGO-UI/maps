#include "services/gnss_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>

namespace gnss_protocol {
namespace {

std::string trim(const std::string &value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? std::string{}
                                      : value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}

std::vector<std::string> split(const std::string &value)
{
    std::vector<std::string> fields;
    std::size_t begin = 0;
    for (;;) {
        const auto end = value.find(',', begin);
        fields.push_back(trim(value.substr(begin, end == std::string::npos ? end : end - begin)));
        if (end == std::string::npos) return fields;
        begin = end + 1;
    }
}

bool number(const std::string &value, double &result)
{
    if (value.empty()) return false;
    // Modem numbers always use a decimal dot, regardless of the UI's numeric locale.
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    input >> std::noskipws >> result;
    return input && input.peek() == std::char_traits<char>::eof() && std::isfinite(result);
}

bool coordinate(const std::string &value, const std::string &hemisphere, bool latitude, double &result)
{
    if (latitude ? hemisphere != "N" && hemisphere != "S" : hemisphere != "E" && hemisphere != "W") return false;
    // NMEA coordinates are unsigned degrees and minutes, not decimal degrees or exponents.
    const auto decimal = value.find('.');
    if ((decimal == std::string::npos ? value.size() : decimal) != (latitude ? 4u : 5u) ||
        !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) || ch == '.'; }))
        return false;
    double raw = 0;
    if (!number(value, raw) || raw < 0) return false;
    const double degrees = std::floor(raw / 100.0);
    const double minutes = raw - degrees * 100.0;
    result               = degrees + minutes / 60.0;
    if (minutes >= 60 || result > (latitude ? 90 : 180)) return false;
    if (hemisphere == "S" || hemisphere == "W") result = -result;
    return true;
}

Position nmea(const std::string &line)
{
    const auto star = line.find('*');
    if (line.size() < 9 || star == std::string::npos || star + 3 != line.size()) return {};
    const auto hex = line.substr(star + 1);
    if (!std::all_of(hex.begin(), hex.end(), [](unsigned char ch) { return std::isxdigit(ch); })) return {};
    unsigned checksum = 0;
    for (std::size_t index = 1; index < star; ++index) checksum ^= static_cast<unsigned char>(line[index]);
    if (checksum != std::strtoul(hex.c_str(), nullptr, 16)) return {};
    const auto fields = split(line.substr(1, star - 1));
    if (fields.empty() || fields[0].size() != 5) return {};
    const auto type = fields[0].substr(2);
    Position result;
    if (type == "GGA" && fields.size() >= 10) {
        double quality = 0;
        if (!number(fields[6], quality) || quality < 0 || quality > 8 || quality != std::floor(quality)) return {};
        // Estimated, manual and simulated GGA solutions are not satellite fixes.
        result.kind = quality > 0 && quality <= 5 && coordinate(fields[2], fields[3], true, result.point.latitude) &&
                              coordinate(fields[4], fields[5], false, result.point.longitude)
                          ? PositionKind::Fix
                          : PositionKind::Invalid;
    } else if (type == "RMC" && fields.size() >= 10) {
        if (fields[2] != "A" && fields[2] != "V") return {};
        const bool mode_valid = fields.size() <= 12 || fields[12].empty() || fields[12] == "A" || fields[12] == "D" ||
                                fields[12] == "R" || fields[12] == "F" || fields[12] == "P";
        result.kind = fields[2] == "A" && mode_valid && coordinate(fields[3], fields[4], true, result.point.latitude) &&
                              coordinate(fields[5], fields[6], false, result.point.longitude)
                          ? PositionKind::Fix
                          : PositionKind::Invalid;
    }
    return result;
}

}  // namespace

Position parse_position(const std::string &bytes)
{
    auto line = trim(bytes);
    if (line.rfind("#XGNSSNMEA:", 0) == 0) line = trim(line.substr(11));
    if (!line.empty() && line.front() == '$') return nmea(line);
    if (line.rfind("#XGNSSPOS:", 0) != 0) return {};
    const auto fields = split(line.substr(10));
    Position result;
    // The SLM position notification itself signifies a valid fix.
    if (fields.size() >= 2 && number(fields[0], result.point.latitude) && number(fields[1], result.point.longitude) &&
        std::abs(result.point.latitude) <= 90 && std::abs(result.point.longitude) <= 180)
        result.kind = PositionKind::Fix;
    return result;
}

std::vector<int> integer_response(const std::string &bytes, const std::string &prefix)
{
    const auto line = trim(bytes);
    if (line.rfind(prefix, 0) != 0) return {};
    std::vector<int> values;
    for (const auto &field : split(line.substr(prefix.size()))) {
        double value = 0;
        if (!number(field, value) || value != std::floor(value) || value < 0 || value > 65535) return {};
        values.push_back(static_cast<int>(value));
    }
    return values;
}

}  // namespace gnss_protocol
