# ToDo — Input reliability implementation plan

**Current phase: M1 plus the M2 profile/mapping controls on an M1-based port; live activation and IME acceptance pending.** The port applies M2 commit `7e28d08` on `3d61349` rather than inheriting its M3, CapsLock, and later capture-chord ancestors. M1 verification below was recorded previously; the M2 results below belong to the original M2 ancestry, not this port. Connection capture policy, QML integration, real Wayland `active`/`inactive` listeners, the runtime RDP Super gate, and keyboard cleanup are implemented. A real activation/release-chord test remains unavailable without an authorized live session.

## M1 — Diagnose input ownership and implement focused capture

- [ ] Inspect the package/source input path and relevant existing tests before changing behavior. Record the actual Qt/Wayland/fcitx5/compositor versions and session/backend settings used for subsequent live acceptance.
- [ ] With the user's remote sessions available and live testing authorized, trace canvas focus, QML host shortcut precedence, native key metadata, IME consumption/commit, and backend delivery. Inspection found fcitx5 US/Hangul and `input.lua` `kb_options = caps:none`, but the Caps Lock description in `~/.config/fcitx5/config` is a comment only and `[Hotkey/TriggerKeys]` is empty. These establish neither an effective runtime/default binding nor its absence: CapsLock remains an unconfirmed candidate. Verify the effective active binding and actual raw events/IME consumption before choosing a source trigger; do not assume CapsLock, dedicated Hangul, or Right Alt input. Distinguish local compositor interception from IME interception and application forwarding. Do not log typed text/secrets. A received event is not proof of a remote IME transition.
- [x] Reuse Gliff's shortcut-inhibit path for embedded desktop capture; implement connection/group `keyCapture`, SSH off, and runtime RDP capture permission separate from stored `grabKeyboard`.
- [x] Wire actual `active`/`inactive` listeners and independent eligibility fallback; expose requested-versus-actual state and editor controls.
- [x] Register the generic session release chord through Keymap/SessionView and retain explicit rearm. Activation-dependent runtime release remains a live acceptance item below.
- [x] Implement tracked-key/modifier cleanup and stale-repeat suppression without synthesizing mouse releases; exercise real RdpView/socket boundaries in `rdpview-keys-test`.
- [x] Add behavioral settings/key-cleanup coverage and user documentation. The permanent native regression runs as `QT_QPA_PLATFORM=offscreen native/build/rdpview-keys-test`; full-suite results are recorded under "M1 implementation and local verification" below.
- [x] Add the capture-toggle session chord (`ctrl+alt+g`, an unused chord; `ctrl+alt+t` is taken
  by own-window-to-tab): release while captured, take the shortcuts back through the new
  `rearmShortcuts()` without a click, with a truthful `released` capture state between. The
  chord follows Settings › Keyboard like every session chord; live activation remains an item
  below.

### M1 live acceptance

- [ ] In each supported desktop protocol, compare capture off/on with local shortcuts and remote key delivery. Confirm actual activation, unsupported/refused inhibition, and compositor deactivation show truthful status.
- [ ] Verify pointer-inside/outside behavior for auto/on modes without stealing focus. Test manager fields, connection editor, popup, tab/window switch, hide/show, reconnect, and window deactivation.
- [ ] Hold modifiers/keys through escape, focus loss, and disconnect: no remote stuck keys, duplicate releases, local modifier contamination, or replay into another session. Escape remains usable while captured; recapture requires the intended rearming action.
- [ ] Verify stored RDP Super forwarding and the capture policy produce the documented effective backend gate, including restoration when capture ends; compositor inhibition alone must not leave Super silently discarded. Record observed results and limitations rather than treating protocol support as live proof.
- [ ] Press the toggle chord on a captured desktop (RDP/VNC `on`, Gliff auto): the status bar
  shows ⌨ captured → ⌨ released, compositor shortcuts work again, and the same chord takes the
  shortcuts back (⌨ captured) without a click; a key typed between the two still reaches the
  remote. RDP: verify Super delivery follows the toggled state.

### M1 implementation and local verification

Behavior landed: per-connection and group `keyCapture` (`auto`/`on`/`off`, Gliff's pointer
policy preserved in auto, SSH off); requested mode kept separate from actual state, driven by
the real Wayland inhibitor `active`/`inactive` events with independent eligibility ending
capture on focus/visibility/window/session loss (the compositor may restore shortcuts without
`inactive`); a compositor escape does not recreate the inhibitor against the user's decision;
the configured session-prefix + Esc releases active capture for any desktop protocol; RDP
runtime `capture 0|1` widens stored `grabKeyboard` without changing it, Super releases always
pass the gate; leaving actual capture lifts every tracked key and synthetic modifier exactly
once before the RDP gate closes, without blur unless the keyboard really left and without
synthesizing mouse releases, with stale repeats and duplicate late releases suppressed until a
fresh press.

- `ninja -C native/build`: succeeded, no new warnings (CMake's existing GuiPrivate coupling
  warning and absent optional Vulkan headers aside).
- `QT_QPA_PLATFORM=offscreen native/build/rdpview-keys-test`: `ok` — real RdpView over real
  Unix sockets: press/repeat/release delivery, cleanup once, stale-repeat and late-release
  suppression, fresh presses after cleanup and socket replacement, synthetic Ctrl pairing,
  session end, pointer exit with a focused window in on mode, truthful
  unsupported/off/ineligible reporting.
- `native/build/rdp-gfx-test` and `native/build/gliff-proto-test`: `ok`.
- `python3 -m unittest discover tests`, run once after integration: 393 tests, 112.8 s, OK
  (Python's own ResourceWarnings and a threaded-fork DeprecationWarning were not suppressed
  or diagnosed).
- Isolated offscreen Quickshell runs loaded the real SessionView, Store, Sessions and native
  plugin against local synthetic session sockets: RDP and VNC resolved `keyCapture=on`,
  attached live, reported `unsupported` while focused and `ineligible` after focus moved;
  SSH resolved and reported `off`. Local wiring checks, not compositor-activation proof.

No live compositor or remote pass is claimed: installed Hyprland 0.56.2 rejects `--headless`
and an isolated Aquamarine startup could not acquire a seat (logind busy) nor allocate after
the Wayland backend failed; no other compositor is installed, nothing was installed, and no
window was nested in the user's session. Real `active`/`inactive` delivery, compositor
re-enable, activation-dependent chord release, captured Gliff auto pointer transitions and
remote Super forwarding remain the live acceptance items above. All throwaway smoke
scaffolding and created crash reports were removed; the running app was untouched.

## M2 — Explicit profiles and IME-safe delivery

- [x] Add `inputProfile` with unchanged/default behavior and explicit Windows/Linux/macOS/custom selection; never infer the target OS from RDP/VNC/SSH/Gliff.
- [ ] Verify the effective active binding and actual raw events/IME consumption before choosing a source; CapsLock remains unconfirmed. Two editable source-evdev → ordered remote evdev/keysym mappings are implemented, disabled unless both fields are entered; settings are portable.
- [ ] Verify RDP Korean scancodes and VNC `0xff31`/`0xff34` delivery. Local fake RFB test now observes the exact VNC keysyms on press and release; actual remote keymap and RDP delivery remain unverified. Keep Gliff evdev/keymap semantics separate.
- [x] Make macOS input-source combinations explicitly configurable. No automatic Caps Lock/Control+Space fallback. Pair generated keys and preserve modifiers already held by the user.
- [ ] Determine the correct item-scoped desktop canvas IME behavior without setting global `QT_IM_MODULE=none`. Retain local IME for manager fields and SSH composition/committed text; prevent duplicate committed-text/raw-key delivery.
- [ ] Add regression coverage for default/missing settings, explicit/custom actions, cleanup, protocol routing, SSH committed text, and settings round trips; update user-facing documentation for the implemented controls. Socket and fake-SSH PTY tests cover text-bearing key events, but actual Qt input-method commit/preedit and duplication remain untested.

Profile/mapping local socket and settings tests do not establish remote delivery, actual IME switching,
or the user's effective local trigger. The item currently has no `ItemAcceptsInputMethod` or
`inputMethodEvent` implementation; no process-wide IME changes were made. SSH text-only and keyed
text commands have been exercised through the local fake SSH PTY, not actual fcitx composition.
Item-scoped IME and live composition acceptance remain open.
On the original M2 ancestry, `ninja -C native/build`, the offscreen native key test, and 411 Python tests passed; an isolated offscreen manager startup loaded configuration. Those results do not verify this M1-based port. No live user seat, session restart, or remote connection was used.

On the M1-based port, a fresh out-of-tree CMake/Ninja build and the offscreen `rdpview-keys-test`, `rdp-gfx-test`, and `gliff-proto-test` passed. `python3 -m unittest discover tests` passed 407 tests against the fresh build; Python reported ResourceWarnings and a threaded-fork DeprecationWarning. These local checks do not establish live IME or remote behavior.

### M2 live acceptance

- [ ] With fcitx5 enabled, type Korean and ordinary text in manager fields and SSH, including composition, dead keys, editing/cancellation, and commit. Confirm local IME remains available and text is not duplicated.
- [ ] On configured Windows/Linux/macOS desktop targets, observe an actual remote Hangul/English transition and normal subsequent typing. Verify Hanja where the target supports it. Record target IME/layout and VNC server/keymap prerequisites.
- [ ] Test explicit custom combinations, held modifiers, press/release pairs, cancellation, and tab changes. Ensure a profile never changes solely because the protocol changes and unsupported mappings are explained rather than silently substituted.

## M3 — Mouse reliability, after diagnosis

- [ ] Fix runtime-proven VNC loss of sub-120 angle deltas with independent signed per-axis residue: the local probe's `30×4` produced zero packets on either axis/sign, while 120 produced the expected wheel click. Preserve held-button masks in wheel pairs and reset residue at session boundaries.
- [ ] Preserve the inspected RDP loop: it emits every nonzero delta in signed steps clamped to ±255, including sub-120 input, so no RDP residue fix is needed. Independently verify horizontal direction and Gliff behavior without assuming they share VNC's defect.
- [ ] Inspect `angleDelta()` versus `pixelDelta()` evidence before expanding touchpad behavior. Do not claim all touchpad input is covered by an angle-delta accumulator.
- [ ] Diagnose drag using Qt Quick's existing implicit grab: accepted press, movement/release outside the canvas, scaling/coordinates, ungrab/cancellation, focus changes, and backend mask state. Fix the demonstrated failure, not an assumed missing explicit grab.
- [ ] Fix the runtime-proven VNC blur cleanup gap: button-down → `blur` → move retains mask `1`, although ordinary drag releases correctly (`1,1,1,0`). Add coherent release/reset on genuine focus/session loss, cancelled grab, and disconnect, covering missing UI mouse cleanup/ungrab handling. Preserve valid dragging across pointer exit/reentry; pointer leave alone is not cancellation. The probe does not prove the cause of the user's live drag symptom.
- [ ] Add wheel/drag regression coverage and update user-facing behavior documentation for changes actually made.

### M3 live acceptance

- [ ] On VNC, test notched wheels and touchpads: repeated small deltas, accumulated detents, reverse direction, mixed axes, scrolling while a button is held, reconnect, and session switching. Confirm no lost residue within a session and no leakage into the next.
- [ ] Test RDP and Gliff scrolling independently, including small deltas and horizontal direction; confirm RDP's inspected small-delta forwarding works without adding an accumulator.
- [ ] In each desktop backend, drag within the view, outside/reenter, across scaled edges, and then release; repeat with multiple buttons and scroll while dragging. Verify no premature release, duplicate release, or stranded mask.
- [ ] Repeat drag with focus loss, tab/window switch, cancellation, and disconnect. Distinguish expected cancellation cleanup from pointer-exit continuation.

## Future verification and completion

Only after implementation, have the integration owner run the applicable native build and Python tests, once all edits are landed:

```sh
ninja -C native/build
python3 -m unittest discover tests
```

Automated checks supplement, not replace, the live acceptance above. M1 local verification is recorded under "M1 implementation and local verification"; unexercised compositor/remote criteria remain unchecked. M2 profile/mapping controls are implemented on this port; item-scoped IME behavior and live acceptance remain open. M3 changes are not included. Do not mark live criteria passed without a real target session.
