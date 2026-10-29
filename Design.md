# Design — Remote input ownership and protocol-specific delivery

**Status: M1, M2 profile/mapping controls, and M3 wheel-residue/button cleanup implemented; live acceptance pending.** IME behavior and the reported live touchpad cause remain unverified. See [ToDo.md](ToDo.md).

## Settings and compatibility

Add connection settings through the existing Python connection validation/storage and QML editor/store paths; preserve them through edit, duplicate, import/export, and sync using existing conventions.

| Setting | Proposed meaning |
| --- | --- |
| `keyCapture` | `auto` (missing-field default), `on`, or `off`; a request, never a claim of active capture. |
| `inputProfile` | `default` (missing-field default), `windows`, `linux`, `macos`, or `custom`; explicitly selected, never inferred from protocol. |
| IME trigger/action mapping | Separate a verified source trigger (CapsLock is only an unconfirmed candidate) from remote Hangul/Hanja actions: existing/native behavior, supported named toggle key, or an explicit ordered key combination. Presets populate editable values; do not require the source to emit a dedicated Hangul key or make the mapping CapsLock-specific. |

Mappings are two optional source evdev codes and ordered evdev:keysym pairs; default profile leaves all input unchanged. Validation rejects malformed settings. Existing RDP `grabKeyboard` remains separate from capture. Item-scoped IME behavior remains open.

### Optional Mac physical-key helper

`vncMacInputHost` is a connection-only, machine-local SSH destination/alias, empty by default. It is excluded from groups, export, sync and imported activation. Nonempty requires VNC and an explicit `macos` profile. The editor explains SSH key/agent authentication, remote helper installation, and manual Accessibility approval. No automatic target discovery.

Python constructs `ssh -T -o BatchMode=yes -o ServerAliveInterval=5 -o ServerAliveCountMax=2 -- <host> omaremote-mac-input --stdio`; the native VNC backend owns its pipes/process. Remote executable is fixed, not user shell text. The remote helper is installed manually on the SSH command PATH.

Protocol v1 is bounded ASCII lines: helper announces `ready 1`, client sends `key <0|1> <evdev>`, `release`, and `ping`; helper answers `pong`. Keys carry physical identity, not typed text. All key/CAD paths use the selected transport exclusively, including keysym-zero events. Qt's legacy macOS Hangul/Hanja-to-Control+Space default is bypassed in helper mode; explicit user mappings remain applicable. Unsupported keys, malformed lines, permission denial, process exit or handshake mismatch fail visibly without RFB fallback.

The helper tracks injected keys and left/right modifier flags; EOF, explicit release, termination and expired liveness release every held key. Native UI detach/focus loss and VNC shutdown release helper keys. Parent queues are bounded and nonblocking so SSH cannot freeze framebuffer processing. Tests distinguish physical-helper routing from legacy RFB packets; only live Mac composition establishes Korean success.

Helper readiness precedes RFB initialization so SSH startup does not delay consumption of the first framebuffer response. Credential prompts pump helper heartbeats. The existing synchronous libvncclient decoder and its configured read timeout are retained; a long blocked/decode operation can starve heartbeat processing and visibly end the helper session. The remote lease still releases held keys; no independent decoder-liveness guarantee is claimed.



## Editor, status, and Keymap

In `ui/Editor.qml`, group the proposed controls under input settings:

- Capture request with help explaining compositor support, canvas focus, and the difference from RDP Super forwarding. Show only applicable desktop capture controls; do not silently enable capture for SSH.
- Explicit profile selector with `Default / unchanged` as the initial value. Explain that the profile is a user choice, not OS detection.
- Editable source-trigger and Hangul/Hanja action mappings, with a custom combination editor using established shortcut-field conventions. Choose the source only after verifying the effective active binding and actual raw events/IME consumption; CapsLock remains an unconfirmed candidate and need not be the remote action. The explicit macOS profile on VNC supplies Control+Space for Hangul/Hanja evdev keys 122/123 only when no user mapping targets that key. A user mapping targeting that key takes precedence even if its action is unusable. The Mac's configured shortcut must match; live switching and subsequent Korean typing remain unverified.
- Explain server prerequisites where relevant, including VNC keymap support for Hangul/Hanja. A configured action is not proof of remote IME state.

In the session UI/status area, show **requested mode** separately from **actual state**: off, waiting for canvas focus, unavailable/unsupported, requested but not activated, active, or compositor-deactivated. Derive active only from protocol activation. Do not display a remote Hangul/English state that has not been observed. Provide a visible release-capture action and an accessible explanation when capture is unavailable.

Register the host capture/release shortcuts in `ui/Keymap.qml`, with their descriptions/help and eventual README key-table entries. Check existing shortcuts for conflicts before selecting bindings. A documented host escape must be reachable before remote forwarding, work while capture is active, and release capture without depending on the remote process. Do not promise that the compositor's reserved escape chord is configurable or delivered to the app. Manager editing/search/prompt shortcuts continue to work when their fields own focus.

## Capture ownership and lifecycle

The implementation reuses `native/plugin/shortcutinhibit.cpp` and `RdpView::updateCapture()`. `held()` tracks object existence; actual `active`/`inactive` listeners determine activation. Focus loss/unmapping may restore compositor shortcuts without `inactive`, so the view also independently gates activation on current eligibility.

Eligibility requires all of: a supported embedded desktop session, a live surface/connection, active application window and session, and the remote canvas owning active focus. Editor fields, dialogs, popups, other tabs/windows, hidden sessions, and disconnected canvases are ineligible. An explicit `on` request never steals focus from them.

- `off`: do not request inhibition.
- `auto`: retain Gliff's existing pointer-inside plus active-focus heuristic. Other protocols retain existing automatic behavior until validated; explicit `on` enables their supported capture path.
- `on`: request inhibition while eligible, including pointer exit if the canvas remains focused. Pointer position alone is not focus ownership.

For RDP, requesting compositor inhibition is insufficient: the backend drops evdev 125/126 when effective `FreeRDP_GrabKeyboard` is false. Synchronize that effective gate with the eligible capture policy while retaining the stored RDP Super preference as a distinct setting. Specify and test restoration of the effective gate when capture ends; do not leave an unexplained UI/backend mismatch.

Handle inhibitor activation and deactivation separately from creation/destruction. On focus loss, tab/window deactivation, disconnect, capture release, or destruction, release tracked remote keys/modifiers exactly once, clear tracking, and send the existing backend blur/reset as appropriate. Prevent a held local key or repeat from being forwarded as a fresh remote press after ownership changes; require a fresh press where necessary. Do not replay queued input into the next session. Escape must clean up its own modifiers as well as release the inhibitor. Deactivation must not immediately recreate an inhibitor against a user's escape; explicit rearming or the documented ownership transition is required.

Keyboard cleanup and pointer cleanup are separate lifecycles. Capture release can require keyboard cleanup without proving the mouse drag was cancelled. Pointer leave must not synthesize a remote mouse release.

## IME and backend boundaries

Local inspection found US and Hangul input methods in fcitx5 and `kb_options = caps:none` in `input.lua`. The Caps Lock description in `~/.config/fcitx5/config` is a comment only, and `[Hotkey/TriggerKeys]` is empty; neither establishes an effective runtime/default binding or its absence. CapsLock is only an unconfirmed candidate. Verify the effective active binding before choosing a source trigger, without assuming CapsLock, a dedicated Hangul key, or Right Alt. Trace actual raw events and IME consumption with fcitx5 enabled: native scan code, keysym, accepted status, focus, and composition/commit path. Limit temporary diagnostics to event metadata, not typed text, credentials, clipboard contents, or persistent keylogging. Remove diagnostic scaffolding after implementation.

For raw desktop input, choose item-scoped Qt behavior that prevents local composition from consuming intended remote toggle keys without disabling input methods in manager controls. Do not set global `QT_IM_MODULE=none`. Absence of an existing `inputMethodEvent` handler is not evidence that all input bypasses local IME processing.

| Backend | Delivery contract |
| --- | --- |
| RDP | Preserve evdev/scancode delivery; existing Korean evdev 122/123 mappings are available. Verify the remote layout/IME and actual emitted sequence. Do not switch every key to Unicode to solve toggle delivery. |
| VNC | Preserve native keysyms, including `0xff31` (Hangul) and `0xff34` (Hangul_Hanja), with paired press/release. Only the explicit macOS profile supplies the Hangul/Hanja Control+Space chord when no user mapping targets that key; a matching user mapping takes precedence even if its action is unusable. Do not substitute locally composed syllables as Unicode keysyms. Remote shortcut/keymap support and live macOS behavior remain unverified. |
| Gliff | Preserve evdev/keymap semantics; do not send VNC keysyms into its transport. Validate configured combinations against the remote layout. |
| SSH | Retain local IME composition and committed terminal text, including dead keys and Korean. Do not inject desktop IME toggle sequences or disable local IME merely because a profile names an OS. If existing composition is incomplete, inspect Qt-to-terminal committed-text delivery before designing its fix. |

Custom combinations must preserve existing user-held modifiers: release only synthetic presses owned by that action, pair all generated keys, and clear owned state on cancellation. Suppress duplicate ordinary/raw delivery for a consumed toggle; ordinary typing must remain unchanged. Caps Lock is stateful and must be explicitly selected and tested, not used as an automatic fallback. Never emit both Caps Lock and Control+Space hoping one works.

## Mouse: proven fix, then diagnosis

`RdpView::wheelEvent` currently forwards only `angleDelta()`; pixel-only input loss is an inference, not a physical-device result. A local loopback RFB probe of `native/build/omaremote-vnc` confirmed that four deltas of 30 produce zero pointer packets on either axis/sign, while 120 produces the expected button mask 8/16/32/64 and release. For VNC, keep independent signed horizontal/vertical remainders per session. Add incoming angle units, emit one wheel click per full 120 units, and retain the remainder, including direction reversals. Preserve held buttons in wheel pairs (already observed as `9→1` and `33→1` with left held). Reset residue on disconnect/session replacement; validate signs and physical input separately.

RDP's inspected loop (`native/rdp/main.cpp:421–434`) runs while `delta != 0`, emits signed steps clamped to ±255, and subtracts each emitted step. It therefore preserves sub-120 deltas and needs no VNC-style residue fix. Preserve this path and its horizontal convention unless independent evidence demonstrates a different defect. Keep Gliff's existing axis semantics independently validated.

The same local VNC probe observed ordinary drag masks `1,1,1,0`, but button-down → `blur` → move retained mask `1`. Fix this concrete blur cleanup gap; the UI currently releases keys on focus loss, not buttons, and has no `mouseUngrabEvent` handler. Define one coherent release/reset path for actual focus/session loss, disconnect, and cancelled grab so UI and backend state agree without duplicate releases. This does not establish the cause of the user's drag failure: trace accepted press → implicit Qt Quick grab → moves → release, coordinates, cancellation, focus, and exit/reentry. Preserve valid drags outside the item; do not synthesize button release on pointer leave or inhibitor deactivation alone. No live UI grab transition was exercised by the backend probe.

## References

Primary protocol/platform URLs and their evidentiary limits are collected in [Concept.md](Concept.md#primary-references). Repository landmarks: `ui/SessionView.qml`, `ui/Editor.qml`, `ui/Keymap.qml`, `native/plugin/rdpview.cpp`, `native/plugin/shortcutinhibit.cpp`, `native/{rdp,vnc,ssh,gliff}/main.cpp`, and `bin/omaremote-session`.
