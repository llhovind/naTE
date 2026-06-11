#pragma once

#include "config/Color.h"
#include "document/Document.h"
#include <nlohmann/json.hpp>
#include <codecvt>
#include <locale>
#include <string>

// JSON serialisation helpers for document types.
// Used by ScrollbackWriter (write path) and JsonScrollbackRepository (read path).

namespace term::db {

inline std::string U32ToUtf8(const std::u32string& s)
{
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
    return conv.to_bytes(s);
}

inline std::u32string Utf8ToU32(const std::string& s)
{
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
    return conv.from_bytes(s);
}

inline nlohmann::json SerialiseRgb(const Rgb& rgb)
{
    return nlohmann::json{{"r", rgb.r}, {"g", rgb.g}, {"b", rgb.b}};
}

inline Rgb DeserialiseRgb(const nlohmann::json& j)
{
    return {
        static_cast<uint8_t>(j.value("r", 0)),
        static_cast<uint8_t>(j.value("g", 0)),
        static_cast<uint8_t>(j.value("b", 0))
    };
}

inline nlohmann::json SerialiseStyle(const Style& s)
{
    nlohmann::json j = nlohmann::json::object();
    if (s.fg != -1)    j["fg"] = s.fg;
    if (s.bg != -1)    j["bg"] = s.bg;
    if (s.bold)        j["bo"] = true;
    if (s.dim)         j["di"] = true;
    if (s.italic)      j["it"] = true;
    if (s.underline)   j["ul"] = true;
    if (s.reverse)     j["rv"] = true;
    if (s.fgRgb)       j["fr"] = SerialiseRgb(*s.fgRgb);
    if (s.bgRgb)       j["br"] = SerialiseRgb(*s.bgRgb);
    return j;
}

inline Style DeserialiseStyle(const nlohmann::json& j)
{
    Style s;
    s.fg        = j.value("fg", -1);
    s.bg        = j.value("bg", -1);
    s.bold      = j.value("bo", false);
    s.dim       = j.value("di", false);
    s.italic    = j.value("it", false);
    s.underline = j.value("ul", false);
    s.reverse   = j.value("rv", false);
    if (j.contains("fr")) s.fgRgb = DeserialiseRgb(j.at("fr"));
    if (j.contains("br")) s.bgRgb = DeserialiseRgb(j.at("br"));
    return s;
}

inline nlohmann::json SerialiseStyleRun(const StyleRun& sr)
{
    return nlohmann::json{
        {"p", sr.start},
        {"l", sr.length},
        {"s", SerialiseStyle(sr.style)}
    };
}

inline StyleRun DeserialiseStyleRun(const nlohmann::json& j)
{
    return {
        j.value("p", size_t{0}),
        j.value("l", size_t{0}),
        DeserialiseStyle(j.value("s", nlohmann::json::object()))
    };
}

// Serialises a DocLine to a compact NDJSON object.
// When saveStyles is false the "s" array is omitted (saves ~50% space).
inline nlohmann::json SerialiseDocLine(const DocLine& line, bool saveStyles)
{
    nlohmann::json j;
    j["t"] = U32ToUtf8(line.text);
    if (saveStyles && !line.styles.empty()) {
        auto arr = nlohmann::json::array();
        for (const auto& sr : line.styles)
            arr.push_back(SerialiseStyleRun(sr));
        j["s"] = std::move(arr);
    }
    return j;
}

inline DocLine DeserialiseDocLine(const nlohmann::json& j)
{
    DocLine line;
    line.text = Utf8ToU32(j.value("t", std::string{}));
    if (j.contains("s")) {
        for (const auto& srj : j.at("s"))
            line.styles.push_back(DeserialiseStyleRun(srj));
    }
    return line;
}

} // namespace term::db
