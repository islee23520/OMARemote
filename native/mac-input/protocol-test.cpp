#include "protocol.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
void require(bool condition, const char *name) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}
}
int main() {
    using namespace macinput;
    require(parse("ping").kind == Kind::Ping, "ping");
    require(parse("release").kind == Kind::Release, "release");
    for (unsigned code = 0; code <= 767; ++code) {
        for (unsigned down = 0; down <= 1; ++down) {
            const auto line = "key " + std::to_string(down) + " " + std::to_string(code);
            const auto command = parse(line);
            require(command.kind == Kind::Key && command.evdev == code && command.down == (down != 0), "key round trip");
        }
    }
    const char *invalid[] = {
        "", " ping", "ping ", "Ping", "ping\r", "release 0", "key", "key 1",
        "key 1 ", "key 1 01", "key 1 -1", "key 1 +1", "key 2 30", "key 10 30",
        "key 1 768", "key 1 999", "key 1 1000", "key 1 4294967296",
        "key 1 30 ", "key 1 30\n", "key 1 30 extra", "key\t1 30", "key 1\t30",
        "key 1  30", "key 1 a", "key 1 3a", "key 1 3\x7f"
    };
    for (const auto *line : invalid) require(parse(line).kind == Kind::Invalid, "reject malformed command");
    require(parse(std::string(MaxLine + 1, 'x')).kind == Kind::Invalid, "reject oversized line");
    require(parse(std::string_view("ping\0", 5)).kind == Kind::Invalid, "reject embedded NUL");
    for (const auto &key : Keys) {
        require(key.evdev <= 767 && key.mac <= 127, "mapping range");
        require(virtualKey(key.evdev) == key.mac, "mapping lookup");
        unsigned evdevCount = 0, macCount = 0;
        for (const auto &other : Keys) {
            evdevCount += other.evdev == key.evdev;
            macCount += other.mac == key.mac;
        }
        require(evdevCount == 1 && macCount == 1, "no aliases or duplicate keys");
    }
    require(virtualKey(30) == 0 && virtualKey(44) == 6, "ANSI physical letters");
    require(virtualKey(86) == 10, "ISO section");
    require(virtualKey(89) == 94 && virtualKey(124) == 93, "JIS RO and yen");
    require(virtualKey(93) == 104 && virtualKey(94) == 102, "JIS Kana and Eisu");
    require(virtualKey(122) == -1 && virtualKey(123) == -1, "Hangul/Hanja require explicit mapping");
    require(virtualKey(69) == -1 && virtualKey(99) == -1 && virtualKey(191) == -1, "no guessed NumLock PrintScreen or F21");
    require(virtualKey(0) == -1 && virtualKey(767) == -1 && virtualKey(9999) == -1, "unsupported keys");
    const unsigned modifiers[] = {29,42,54,56,58,97,100,125,126,464};
    for (unsigned key : modifiers) require(modifier(key) && virtualKey(key) >= 0, "supported modifiers");
    require(!modifier(30) && !modifier(122), "ordinary keys not modifiers");
    require(keypad(55) && keypad(96) && keypad(117) && !keypad(30), "keypad flags");
    std::puts("All mac-input protocol/map tests passed.");
}
