#include "protocol.hpp"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>

namespace {
volatile sig_atomic_t stopped = 0;
void stop(int signal) { stopped = signal; }
void error(const char *message) { std::fprintf(stderr, "omaremote-mac-input: %s\n", message); }

bool permission() {
    // Neither call requests consent or opens a system dialog.
    if (!AXIsProcessTrusted()) {
        error("Accessibility access denied. In the Mac's System Settings > Privacy & Security > Accessibility, manually approve the executable or SSH hosting process identified by macOS, then reconnect. No approval was requested.");
        return false;
    }
    if (@available(macOS 10.15, *)) {
        if (!CGPreflightPostEventAccess()) {
            error("macOS denies posting keyboard events. Manually review Accessibility permission for this SSH execution context in System Settings, then reconnect.");
            return false;
        }
    }
    return true;
}

class Keyboard {
    CGEventSourceRef source_ = nullptr;
    std::array<bool, 768> held_{};
    bool caps_ = false;

    CGEventFlags flags() const {
        CGEventFlags result = caps_ ? kCGEventFlagMaskAlphaShift : 0;
        struct Modifier { unsigned key; CGEventFlags aggregate; CGEventFlags side; };
        static constexpr Modifier modifiers[] = {
            {29, kCGEventFlagMaskControl, NX_DEVICELCTLKEYMASK},
            {97, kCGEventFlagMaskControl, NX_DEVICERCTLKEYMASK},
            {42, kCGEventFlagMaskShift, NX_DEVICELSHIFTKEYMASK},
            {54, kCGEventFlagMaskShift, NX_DEVICERSHIFTKEYMASK},
            {56, kCGEventFlagMaskAlternate, NX_DEVICELALTKEYMASK},
            {100, kCGEventFlagMaskAlternate, NX_DEVICERALTKEYMASK},
            {125, kCGEventFlagMaskCommand, NX_DEVICELCMDKEYMASK},
            {126, kCGEventFlagMaskCommand, NX_DEVICERCMDKEYMASK},
            {464, kCGEventFlagMaskSecondaryFn, 0}
        };
        for (const auto &modifier : modifiers)
            if (held_[modifier.key]) result |= modifier.aggregate | modifier.side;
        // Caps Lock's latch and its physical depression are distinct Apple flags.
        if (held_[58]) result |= NX_ALPHASHIFT_STATELESS_MASK | NX_DEVICE_ALPHASHIFT_STATELESS_MASK;
        return result;
    }
public:
    Keyboard() {
        source_ = CGEventSourceCreate(kCGEventSourceStatePrivate);
        caps_ = (CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) & kCGEventFlagMaskAlphaShift) != 0;
    }
    ~Keyboard() {
        release();
        if (source_) CFRelease(source_);
    }
    bool valid() const { return source_ != nullptr; }
    bool key(bool down, unsigned code) {
        const int mac = macinput::virtualKey(code);
        if (mac < 0) { error("unsupported evdev key; configure an explicit supported physical-key mapping"); return false; }
        const bool wasDown = held_[code];
        if (!down && !wasDown) return true;
        if (down && wasDown && macinput::modifier(code)) return true;
        // Changing a CGEvent's keycode does not refresh its embedded character data.
        // Create from this physical key and current layout instead of reusing an A event.
        CGEventRef event = CGEventCreateKeyboardEvent(source_, mac, down);
        if (!event) { error("cannot create physical keyboard event"); return false; }
        // Toggle once per fresh press; repeat downs and release never toggle.
        // Keep our latch locally: posted HID updates can be asynchronous.
        if (down && code == 58) caps_ = !caps_;
        held_[code] = down;
        CGEventSetType(event, macinput::modifier(code) ? kCGEventFlagsChanged :
                       (down ? kCGEventKeyDown : kCGEventKeyUp));
        CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, down && wasDown ? 1 : 0);
        // USB keyboard types: ANSI, ISO and JIS. No layout/IME replacement.
        const int type = code == 86 ? 41 :
            (code == 89 || code == 93 || code == 94 || code == 95 || code == 124 ? 42 : 40);
        CGEventSetIntegerValueField(event, kCGKeyboardEventKeyboardType, type);
        CGEventFlags eventFlags = flags();
        if (macinput::keypad(code)) eventFlags |= kCGEventFlagMaskNumericPad;
        CGEventSetFlags(event, eventFlags);
        CGEventSetTimestamp(event, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        return true;
    }
    void release() {
        if (!source_) return;
        // Ordinary keyups retain their chord flags; modifier releases follow.
        for (unsigned code = 0; code < held_.size(); ++code)
            if (held_[code] && !macinput::modifier(code)) key(false, code);
        for (unsigned code = 0; code < held_.size(); ++code)
            if (held_[code]) key(false, code);
    }
};

bool reply(const char *text) {
    // stdout is nonblocking; a blocked SSH reader must not prevent key cleanup.
    const std::size_t length = std::strlen(text);
    const ssize_t written = write(STDOUT_FILENO, text, length);
    if (written != static_cast<ssize_t>(length)) { error("protocol output unavailable"); return false; }
    return true;
}

int serve(Keyboard &keyboard) {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::seconds(macinput::InactivitySeconds);
    std::array<char, macinput::MaxLine> line{};
    std::size_t used = 0;
    if (!reply("ready 1\n")) return 1;
    while (!stopped) {
        if (Clock::now() >= deadline) { error("15-second input heartbeat expired; releasing keys"); return 1; }
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const int result = poll(&input, 1, 100);
        if (result < 0) {
            if (errno == EINTR) continue;
            error("input polling failed"); return 1;
        }
        if (!result) continue;
        if (input.revents & (POLLERR | POLLNVAL)) { error("input pipe failed"); return 1; }
        char bytes[256];
        const ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            error("input read failed"); return 1;
        }
        if (!count) {
            if (used) { error("unterminated protocol line"); return 1; }
            return 0;
        }
        for (ssize_t i = 0; i < count && !stopped; ++i) {
            const char byte = bytes[i];
            if (byte != '\n') {
                if (byte < 0x20 || byte > 0x7e || used == line.size()) {
                    error("invalid or oversized protocol line"); return 1;
                }
                line[used++] = byte;
                continue;
            }
            const auto command = macinput::parse(std::string_view(line.data(), used));
            used = 0;
            switch (command.kind) {
            case macinput::Kind::Key:
                if (!keyboard.key(command.down, command.evdev)) return 1;
                break;
            case macinput::Kind::Release: keyboard.release(); break;
            case macinput::Kind::Ping:
                if (!reply("pong\n")) return 1;
                break;
            default: error("invalid protocol command"); return 1;
            }
            deadline = Clock::now() + std::chrono::seconds(macinput::InactivitySeconds);
        }
    }
    error("termination requested; releasing keys");
    return 128 + stopped;
}
} // namespace

int main(int argc, char **argv) {
    const bool check = argc == 2 && std::strcmp(argv[1], "--check") == 0;
    if (!check && (argc != 2 || std::strcmp(argv[1], "--stdio") != 0)) {
        error("usage: omaremote-mac-input --stdio | --check"); return 2;
    }
    if (!permission()) return 1;
    if (check) { std::puts("permission available; no events posted"); return 0; }
    struct sigaction action{};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    for (int signal : {SIGTERM, SIGINT, SIGHUP}) {
        if (sigaction(signal, &action, nullptr) != 0) { error("cannot install termination handler"); return 1; }
    }
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, nullptr) != 0) { error("cannot ignore broken-pipe signal"); return 1; }
    const int flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        error("cannot configure protocol output"); return 1;
    }
    Keyboard keyboard;
    if (!keyboard.valid()) { error("cannot create CoreGraphics keyboard event source"); return 1; }
    return serve(keyboard);
}
