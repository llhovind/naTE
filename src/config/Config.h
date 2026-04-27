#pragma once
#include <cstdint>
#include <string>

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb& o) const { return !(*this == o); }
};

struct AppConfig {
    int columns        = 80;
    int rows           = 24;
    int fontSize       = 12;
    int scrollbackLines = 100'000;
    int ptyLineWidth    = 1024;
    Rgb textColour{ 0,   0,   0   };
    Rgb bgColour  { 255, 255, 204 };

    static AppConfig load(const std::string& path);
};
