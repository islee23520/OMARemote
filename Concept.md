# Concept — Focused remote input, IME profiles, and mouse reliability

This fork (`islee23520/OMARemote`, tracking `RFdeGroot/OMARemote`) addresses three user-reported problems in the running v0.9.15 package: keys reaching the local Wayland session instead of a remote canvas, unreliable remote Hangul/English switching with local fcitx5, and unreliable drag/scroll in VNC.

**M1 and the M2 profile/mapping module are implemented; full live acceptance pending.** M3 wheel-residue and pointer-cleanup changes have landed; the reported live touchpad cause remains unconfirmed. In the user's Mac VNC session Control+Space changes the input-source indicator to Korean, but subsequent typing remains Latin. CapsLock evdev 58 reaches Qt and VNC; that does not establish successful CapsLock switching. See [ToDo.md](ToDo.md) and the current helper module below.

## Evidence and limits

The existing path is QML session view → C++ `RdpView` → session process → protocol library. `ui/SessionView.qml` owns session focus and host/session chord handling; `ui/Keymap.qml` is the shared shortcut table. `native/plugin/rdpview.cpp` forwards native scan code, keysym, and text, and has focus-loss key cleanup. Backends differ: RDP uses evdev/scancodes, VNC uses keysyms, SSH uses terminal keys/text, and Gliff uses evdev plus a keymap. They must not receive one undifferentiated IME transformation.

### Keyboard capture

- The existing `grabKeyboard` setting is an **RDP-specific Super forwarding flag**, passed as FreeRDP `/+grab-keyboard`. It is not a general compositor keyboard grab and must remain visibly distinct from the new capture request. RDP discards evdev 125/126 unless effective `FreeRDP_GrabKeyboard` permits them; future capture must synchronize that backend gate as well as requesting compositor inhibition.
- Gliff already uses `native/plugin/shortcutinhibit.cpp` and `RdpView::updateCapture()` for Wayland keyboard-shortcuts inhibition, gated by pointer containment and active focus. Reuse this mechanism for supported embedded desktop protocols rather than adding a competing grab implementation.
- `ShortcutInhibit::held` means object existence; the implementation now listens to the protocol's actual `active`/`inactive` events. Eligibility independently ends activation on focus/window/visibility loss, where the compositor may send no `inactive`. No live compositor event delivery has been exercised here. Inhibition still does not override local IME interception or another window's focus.
- Local fcitx5 interception, QML shortcut precedence, and focus transitions are diagnosis candidates, not established root causes for every lost key. A session canvas needs protocol-appropriate input-method behavior; disabling the IME process-wide would break manager fields and SSH composition.

### Hangul and profiles

Local configuration inspection found US and Hangul input methods in fcitx5 and `kb_options = caps:none` in `input.lua`. In `~/.config/fcitx5/config`, the Caps Lock description is a comment only and `[Hotkey/TriggerKeys]` is empty. CapsLock is an **unconfirmed trigger candidate**: these observations establish neither an effective runtime/default binding nor its absence. Verify the effective active binding and actual raw events/IME consumption before choosing a source trigger; do not assume CapsLock, a dedicated Hangul key, or Right Alt.

- No protocol identifies the remote operating system. RDP can target non-Windows systems; VNC and SSH can target macOS, Windows, or Linux. Never derive an OS from `protocol`.
- The existing RDP path includes Korean evdev/scancode mappings. That is evidence of an available transport path, not proof the running session receives the key or the remote IME switches.
- The X keysyms are `XK_Hangul = 0xff31` and `XK_Hangul_Hanja = 0xff34`; `0x0f31`/`0x0f32` are not substitutes. VNC server/keymap support still matters.
- macOS input-source shortcuts are configurable. Caps Lock and Control+Space are examples, not a universal automatic mapping or fallback sequence.
- The implemented VNC-only behavior for an explicitly selected `macos` profile maps Hangul/Hanja evdev keys 122/123 to Control+Space only when no user mapping targets that key. A user mapping targeting that key takes precedence even if its action is unusable; without mappings, other profiles retain native delivery. The user confirmed input-source switching via Control+Space, not Korean composition: ordinary typing still produces Latin. No Unicode-syllable substitution or item-level IME flag experiment is retained.
- SSH is a terminal, not a remote desktop IME interface. Retain local IME composition and committed text by default; do not reinterpret an SSH target OS as a reason to disable it.

### Mouse

- **Runtime-proven VNC wheel defect:** a local loopback RFB probe of `native/build/omaremote-vnc` sent four `wheel` deltas of 30 and received zero pointer packets, on both axes and in both directions. A delta of 120 emitted the expected wheel-button mask (8/16/32/64) followed by release. The backend discards each event's sub-120 remainder rather than accumulating it. This isolated probe contacted no remote host and does not reproduce the user's physical touchpad/UI path.
- **RDP code inspection rules out the same defect:** `native/rdp/main.cpp:421–434` loops while `delta != 0`, clamps each signed step to ±255, and subtracts the emitted step. It preserves all nonzero deltas, including values below 120. No RDP wheel-residue fix is indicated by this code.
- **Runtime-proven VNC cleanup gap; user drag cause still unproven:** ordinary backend drag produced masks `1,1,1,0`, and scrolling while left was held preserved it (`9→1`, `33→1`). But button-down → `blur` → move retained mask `1`: backend blur does not clear held buttons. UI focus loss currently releases keys, not buttons, and there is no `mouseUngrabEvent` handler. These are concrete cleanup gaps, not proof of the cause of the reported live drag failure. Qt Quick already supplies an implicit grab after an accepted press; UI grab transitions have not been tested.
- A pointer leaving the canvas alone must **not** release remote buttons: a legitimate drag can continue outside the item under the implicit grab. True cancellation, focus loss, tab/session deactivation, or disconnect need coherent cleanup.

`RdpView` forwards only `angleDelta()`. Pixel-only scroll could therefore be swallowed [INFERENCE]; no physical device was tested. The local probe script, server/process, and socket were removed after the investigation; no permanent product changes or live key injection were made.

## Decisions

1. **Focused desktop capture:** offer a per-connection `keyCapture` request (`auto`, `on`, `off`) for supported embedded RDP/VNC/Gliff canvases. Preserve Gliff's current `auto` behavior; keep other protocols' automatic behavior unchanged until validated. `on` requests inhibition only while the remote canvas actually owns input focus; it does not override editor/pop-up/window focus. Surface requested mode separately from actual capture state. Always provide a host escape and clear stuck-key state when capture/focus is lost. SSH is not silently opted into desktop capture.
2. **Explicit input profiles:** persist an optional user-selected `inputProfile` (`default`, `windows`, `linux`, `macos`, `custom`) and separate source-trigger/remote-action mappings, including configurable key combinations. Choose the source trigger only after verifying the effective active binding and actual raw events/IME consumption, then map it only to the explicitly selected remote IME action; do not assume an incoming Hangul keysym or a CapsLock-specific solution. `default` preserves existing protocol behavior without guessing an OS. Profile presets are editable guidance, not remote discovery. Preserve current connections and the RDP Super flag.
3. **Scoped IME behavior:** desktop raw-key mode must not globally set `QT_IM_MODULE=none`; manager text fields retain fcitx5, and SSH retains local composition/text input. Determine and verify the Qt item-level desktop canvas behavior before implementation. Keep composed text distinct from raw desktop toggle keys to prevent duplicate delivery.
4. **Evidence-led mouse work:** fix runtime-proven VNC fractional wheel loss with per-axis residue and valid RFB press/release pairs. Close the demonstrated VNC blur/button cleanup gap and handle genuine grab loss, without releasing on pointer leave. Preserve RDP's existing small-delta forwarding; do not add a VNC-style accumulator there. Continue diagnosing the user's drag symptom and Qt Quick grab lifecycle before attributing its cause.

## Non-goals

No global exclusive Wayland grab, compositor configuration changes, new remote protocol, guessed remote OS, forced macOS shortcut, or process-wide IME disablement. Capture does not promise to override compositor-reserved escape shortcuts or unsupported compositor policy. Item-scoped IME and live M2/M3 acceptance remain open.

## Implementation map

See [Design.md](Design.md) for settings, editor/status UI, Keymap integration, lifecycle, and protocol boundaries. See [ToDo.md](ToDo.md) for ordered milestones and live acceptance. The stack is QML/quickshell, a Qt6 C++ plugin, Python session management, and protocol-specific native processes. New host shortcuts belong in `ui/Keymap.qml` and the README key tables when implemented. Project documentation is English.

## Current module — SSH Mac keyboard helper

Live user verification established that Control+Space changes the Mac input-source indicator but ordinary VNC keys still produce Latin text. A client-only composition fix is not established. The user selected an optional Mac-side physical-key helper.

Stack: existing Qt/QML and Python settings, C++ VNC SSH subprocess, and a standalone macOS Objective-C++ helper using CoreGraphics physical virtual-key events. SSH provides authentication and encryption; no listening port or new credential store. A connection-local `vncMacInputHost` SSH alias explicitly enables keyboard-only routing; display, mouse and clipboard remain VNC. Empty leaves all existing behavior unchanged. No remote install, Accessibility approval, or server setting changes occur automatically. Helper readiness and protocol agreement are required, with no silent keyboard fallback.


## Primary references

- [Wayland keyboard-shortcuts-inhibit protocol](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml): compositor policy and activated/deactivated lifecycle.
- [Qt QQuickItem mousePressEvent](https://doc.qt.io/qt-6/qquickitem.html#mousePressEvent), [mouseUngrabEvent](https://doc.qt.io/qt-6/qquickitem.html#mouseUngrabEvent), and [QWheelEvent](https://doc.qt.io/qt-6/qwheelevent.html): implicit grab/cancellation and wheel units.
- [RFB RFC 6143 §7.5.4–7.5.5](https://www.rfc-editor.org/rfc/rfc6143.html#section-7.5.4): key and pointer events.
- [X.Org keysym definitions](https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/blob/master/include/X11/keysymdef.h): Hangul/Hanja keysyms.
- [FreeRDP scancode definitions](https://github.com/FreeRDP/FreeRDP/blob/master/include/freerdp/scancode.h): Korean scancode names; [FreeRDP input implementation](https://github.com/FreeRDP/FreeRDP/blob/master/libfreerdp/core/input.c): protocol input transport.
- [Fcitx5 Wayland setup](https://fcitx-im.org/wiki/Using_Fcitx_5_on_Wayland) and [Qt input method events](https://doc.qt.io/qt-6/qinputmethodevent.html): local composition and committed text are distinct from raw keys.
- [Apple input-source settings](https://support.apple.com/guide/mac-help/change-input-sources-settings-mchl84525d76/mac): user-configurable input switching.

These references describe platform semantics; they do not substitute for validating this application's running sessions.
