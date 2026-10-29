#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace macinput {
constexpr std::size_t MaxLine = 31; // excludes LF; CR is not accepted
constexpr int InactivitySeconds = 15;
enum class Kind { Invalid, Key, Release, Ping };
struct Command { Kind kind = Kind::Invalid; bool down = false; unsigned evdev = 0; };

inline Command parse(std::string_view line) {
    if (line.size() > MaxLine) return {};
    if (line == "release") return {Kind::Release};
    if (line == "ping") return {Kind::Ping};
    if (line.size() < 7 || line.substr(0, 4) != "key " ||
        (line[4] != '0' && line[4] != '1') || line[5] != ' ') return {};
    const auto number = line.substr(6);
    if (number.size() > 3 || (number.size() > 1 && number[0] == '0')) return {};
    unsigned value = 0;
    for (char c : number) {
        if (c < '0' || c > '9') return {};
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > 767) return {};
    return {Kind::Key, line[4] == '1', value};
}

struct Key { unsigned evdev; std::uint16_t mac; };
// Apple HIToolbox Events.h virtual physical positions, not character translation.
// Linux input-event-codes.h evdev identities. No guessed Hangul/Hanja shortcut.
inline constexpr Key Keys[] = {
    {1,0x35}, {2,0x12}, {3,0x13}, {4,0x14}, {5,0x15}, {6,0x17},
    {7,0x16}, {8,0x1a}, {9,0x1c}, {10,0x19}, {11,0x1d}, {12,0x1b},
    {13,0x18}, {14,0x33}, {15,0x30}, {16,0x0c}, {17,0x0d}, {18,0x0e},
    {19,0x0f}, {20,0x11}, {21,0x10}, {22,0x20}, {23,0x22}, {24,0x1f},
    {25,0x23}, {26,0x21}, {27,0x1e}, {28,0x24}, {29,0x3b}, {30,0x00},
    {31,0x01}, {32,0x02}, {33,0x03}, {34,0x05}, {35,0x04}, {36,0x26},
    {37,0x28}, {38,0x25}, {39,0x29}, {40,0x27}, {41,0x32}, {42,0x38},
    {43,0x2a}, {44,0x06}, {45,0x07}, {46,0x08}, {47,0x09}, {48,0x0b},
    {49,0x2d}, {50,0x2e}, {51,0x2b}, {52,0x2f}, {53,0x2c}, {54,0x3c},
    {55,0x43}, {56,0x3a}, {57,0x31}, {58,0x39},
    {59,0x7a}, {60,0x78}, {61,0x63}, {62,0x76}, {63,0x60}, {64,0x61},
    {65,0x62}, {66,0x64}, {67,0x65}, {68,0x6d},
    {71,0x59}, {72,0x5b}, {73,0x5c}, {74,0x4e}, {75,0x56}, {76,0x57},
    {77,0x58}, {78,0x45}, {79,0x53}, {80,0x54}, {81,0x55}, {82,0x52},
    {83,0x41}, {86,0x0a}, // ISO section key
    {87,0x67}, {88,0x6f}, {89,0x5e}, // JIS underscore (RO)
    {93,0x68}, {94,0x66}, {95,0x5f}, // JIS Kana, Eisu, keypad comma
    {96,0x4c}, {97,0x3e}, {98,0x4b}, {100,0x3d},
    {102,0x73}, {103,0x7e}, {104,0x74}, {105,0x7b}, {106,0x7c},
    {107,0x77}, {108,0x7d}, {109,0x79}, {110,0x72}, {111,0x75},
    {117,0x51}, {124,0x5d}, // JIS yen
    {125,0x37}, {126,0x36},
    {183,0x69}, {184,0x6b}, {185,0x71}, {186,0x6a}, {187,0x40},
    {188,0x4f}, {189,0x50}, {190,0x5a}, {464,0x3f} // Fn
};
inline int virtualKey(unsigned evdev) {
    for (const auto &key : Keys) if (key.evdev == evdev) return key.mac;
    return -1;
}
inline bool modifier(unsigned key) {
    return key == 29 || key == 42 || key == 54 || key == 56 || key == 58 ||
           key == 97 || key == 100 || key == 125 || key == 126 || key == 464;
}
inline bool keypad(unsigned key) {
    return key == 55 || (key >= 71 && key <= 83) || key == 95 || key == 96 ||
           key == 98 || key == 117;
}
} // namespace macinput
