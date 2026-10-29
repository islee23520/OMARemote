#pragma once

// Keyboard shortcuts inhibit (Wayland's keyboard-shortcuts-inhibit-unstable-v1): while a window
// holds an inhibitor, the compositor hands it every key, Super and Hyprland's own shortcuts
// included. Gliff tabs hold one while the picture has the focus, as Gliff's own client does.

#include <functional>

class QWindow;
struct zwp_keyboard_shortcuts_inhibitor_v1;

class ShortcutInhibit
{
  public:
	~ShortcutInhibit() { release(); }

	// Whether the compositor offers the protocol at all (false off Wayland); looked up once.
	static bool supported();

	// Creates the inhibitor for window; false when it could not be made (no surface or seat
	// yet). Creation alone inhibits nothing: the compositor's `active` event does, and until it
	// comes activated() stays false. Holding one already for the same window is a no-op.
	bool inhibit(QWindow* window);
	void release();
	bool held() const { return m_inhibitor != nullptr; }
	// The compositor's own word: it has sent `active` and no `inactive` since. It sends
	// `active` every time the shortcuts become inhibited for the surface — the first request,
	// or the user re-enabling a deactivated inhibitor through the compositor itself — and
	// `inactive` when it restores its own shortcuts (the user's way out, where the compositor
	// has one).
	bool activated() const { return m_activated; }

	// Fired on that `active`/`inactive`, from the event loop that dispatched it. One caveat has
	// no event at all: a surface that is destroyed, unmapped or loses the keyboard becomes
	// irrelevant and the compositor restores its shortcuts silently — the view keeps its own
	// eligibility check beside this and lets go by itself then.
	std::function<void()> onActivatedChanged;

  private:
	// The wayland listener (active/inactive), with access to our state.
	static void activatedEvent(void* data, zwp_keyboard_shortcuts_inhibitor_v1* inhibitor);
	static void deactivatedEvent(void* data, zwp_keyboard_shortcuts_inhibitor_v1* inhibitor);

	zwp_keyboard_shortcuts_inhibitor_v1* m_inhibitor = nullptr;
	QWindow* m_window = nullptr;
	bool m_activated = false;
};
