#pragma once

#include <QCursor>
#include <QImage>
#include <QQuickItem>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "shortcutinhibit.h"
#include "yuvnode.h"

#include <deque>

class QSGTexture;

// Shows one remote session (omaremote-rdp or omaremote-vnc, same protocol) inside the window and
// forwards input to it.
//
// The session process owns the connection; this item attaches to its Unix socket, maps the
// framebuffer it is handed, and redraws the damage it reports. Detaching (closing the tab view,
// quitting the app) leaves the session running, and attaching again picks it up where it was.
class RdpView : public QQuickItem
{
	Q_OBJECT
	QML_ELEMENT

	Q_PROPERTY(QString socketPath READ socketPath WRITE setSocketPath NOTIFY socketPathChanged)
	// detached, connecting, connected, ended, failed
	Q_PROPERTY(QString state READ state NOTIFY stateChanged)
	Q_PROPERTY(int exitCode READ exitCode NOTIFY stateChanged)
	Q_PROPERTY(bool attached READ attached NOTIFY attachedChanged)
	Q_PROPERTY(QSize remoteSize READ remoteSize NOTIFY remoteSizeChanged)
	// A question the session is waiting on: {kind: "cert"|"auth", ...}; empty when none.
	Q_PROPERTY(QVariantMap prompt READ prompt NOTIFY promptChanged)
	// Desktop scale to ask for, in percent; 0 follows the window's device pixel ratio.
	Q_PROPERTY(int desktopScale READ desktopScale WRITE setDesktopScale NOTIFY desktopScaleChanged)
	// When false the remote desktop keeps its size and the picture is scaled to the item.
	Q_PROPERTY(bool followSize READ followSize WRITE setFollowSize NOTIFY followSizeChanged)
	// keyCaptureMode asks for the compositor's shortcuts, Super and Hyprland's own keys with
	// them: "on" while the session is live and this view has the keyboard (the pointer's place
	// is not the keyboard's), "auto" (the default) only while captureShortcuts is set and the
	// pointer is on the picture (Gliff's own behaviour), "off" never. Anything else is "auto".
	Q_PROPERTY(QString keyCaptureMode READ keyCaptureMode WRITE setKeyCaptureMode NOTIFY keyCaptureModeChanged)
	// The "auto" request: whether the session's protocol wants every key (Gliff's switch).
	Q_PROPERTY(bool captureShortcuts READ captureShortcuts WRITE setCaptureShortcuts NOTIFY captureShortcutsChanged)
	// Truth about the shortcuts right now: "off" the mode never takes them; "ineligible" this
	// view is not asking (focus, hidden, window inactive, session not live, pointer off the
	// picture, the release key latched); "unsupported" the compositor offers the protocol
	// neither; "inactive" it is asked but not held (not confirmed, not creatable, or the
	// compositor took it back — its `inactive`); "released" the release key or chord gave them
	// back and only a click, the pointer re-entering, focus or rearmShortcuts takes them again;
	// "active" the compositor's `active` event: every key, Super included, is ours. Losing the
	// keyboard restores the compositor's shortcuts without an event, so eligibility is
	// re-checked before an "active" is believed and losing the keyboard ends it here.
	Q_PROPERTY(QString captureState READ captureState NOTIFY captureStateChanged)
	// Whether the compositor's shortcuts are held right now (captureState === "active").
	Q_PROPERTY(bool shortcutsCaptured READ shortcutsCaptured NOTIFY shortcutsCapturedChanged)
	// A key that gives them back besides the session keys + Esc: shift+escape, double-escape or none.
	Q_PROPERTY(QString releaseKey MEMBER m_releaseKey NOTIFY releaseKeyChanged)
	// The session's protocol ("rdp", "vnc", "gliff", "ssh"), or "" when not told. Only "rdp"
	// is acted on: while captureState is "active" the view sends "capture 1", which lets Super
	// through to Windows until "capture 0"; the connection's stored +grab-keyboard setting
	// stays the baseline underneath and is never changed at runtime.
	Q_PROPERTY(QString sessionProtocol READ sessionProtocol WRITE setSessionProtocol NOTIFY sessionProtocolChanged)
	// User-entered mappings: source evdev code and ordered evdev:keysym remote chord.
	Q_PROPERTY(QString inputProfile MEMBER m_inputProfile)
	Q_PROPERTY(QString inputTrigger1 MEMBER m_inputTrigger1)
	Q_PROPERTY(QString inputAction1 MEMBER m_inputAction1)
	Q_PROPERTY(QString inputTrigger2 MEMBER m_inputTrigger2)
	Q_PROPERTY(QString inputAction2 MEMBER m_inputAction2)
	// Off means neither reading the desktop clipboard nor setting text received from the session.
	Q_PROPERTY(bool shareClipboard READ shareClipboard WRITE setShareClipboard NOTIFY shareClipboardChanged)

public:
	explicit RdpView(QQuickItem* parent = nullptr);
	~RdpView() override;

	QString socketPath() const { return m_socketPath; }
	void setSocketPath(const QString& path);
	QString state() const { return m_state; }
	int exitCode() const { return m_exitCode; }
	bool attached() const { return m_fd >= 0; }
	QSize remoteSize() const { return m_gpu ? m_show.size : m_image.size(); }
	QVariantMap prompt() const { return m_prompt; }
	int desktopScale() const { return m_desktopScale; }
	void setDesktopScale(int scale);
	bool followSize() const { return m_followSize; }
	void setFollowSize(bool follow);
	bool captureShortcuts() const { return m_captureShortcuts; }
	void setCaptureShortcuts(bool capture);
	QString keyCaptureMode() const { return m_keyCaptureMode; }
	void setKeyCaptureMode(const QString& mode);
	QString captureState() const;
	QString sessionProtocol() const { return m_sessionProtocol; }
	void setSessionProtocol(const QString& protocol);
	bool shortcutsCaptured() const;
	bool shareClipboard() const { return m_shareClipboard; }
	void setShareClipboard(bool share);

	// The most clipboard text passed either way, in bytes of UTF-8 (as native/common/link.hpp's
	// kMaxClipBytes), how long reading it may take, and the longest line the session may send.
	static constexpr qsizetype MaxClipBytes = 1024 * 1024;
	static constexpr int ClipTimeoutMs = 2000;
	static constexpr qsizetype MaxLineBytes = 32 * 1024 * 1024;
	// The largest cursor taken (as link.hpp's kMaxCursorSide) and how many are kept: the least
	// recently used one goes first, so a server sending new shapes cannot fill memory.
	static constexpr int MaxCursorSide = 512;
	static constexpr int MaxCursors = 64;
	// The largest desktop mapped (as link.hpp's kMaxFrameSide; addSurface holds to the same).
	static constexpr int MaxFrameSide = 8192;

	Q_INVOKABLE void answerCertificate(int answer);
	Q_INVOKABLE void answerCredentials(const QString& user, const QString& domain, const QString& password);
	Q_INVOKABLE void cancelPrompt();
	Q_INVOKABLE void sendCtrlAltDel();
	Q_INVOKABLE void disconnectSession();
	Q_INVOKABLE void releaseAllKeys();
	Q_INVOKABLE void cancelPaste(int job);
	// A lost SSH connection that is being tried again: try at once, or give it up.
	Q_INVOKABLE void reconnectNow();
	Q_INVOKABLE void cancelReconnect();
	// The shortcuts back after the release key or chord gave them up: as a click on the picture
	// would, without needing the mouse. No-op unless they were given up by the release latch
	// (a view not asking for them — focus lost, mode off — stays as it is).
	Q_INVOKABLE void rearmShortcuts();
	// Hyprland's shortcuts back, while the pointer stays on the picture (the session keys + Esc).
	Q_INVOKABLE void releaseShortcuts();
	Q_INVOKABLE int cachedCursors() const { return int(m_cursors.size()); } // for tests

signals:
	void socketPathChanged();
	void stateChanged();
	void attachedChanged();
	void remoteSizeChanged();
	void promptChanged();
	void desktopScaleChanged();
	void followSizeChanged();
	void captureShortcutsChanged();
	void keyCaptureModeChanged();
	void captureStateChanged();
	void sessionProtocolChanged();
	void releaseKeyChanged();
	void shareClipboardChanged();
	// A paste from the remote that takes a while (Gliff): bytes so far, the total (0: unknown), what.
	void pasteProgress(int job, double got, double total, const QString& what);
	void pasteDone(int job);
	// A line about the stream, once a second (Gliff: fps, bandwidth, decode time).
	void statsChanged(const QString& text);
	// SSH trying a lost connection again: phase "wait" (seconds until the next try), "trying",
	// or "off" (back, or given up); attempt counts the tries since the drop.
	void reconnectChanged(const QString& phase, int attempt, int seconds);
	void shortcutsCapturedChanged();

protected:
	QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
	void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
	void itemChange(ItemChange change, const ItemChangeData& value) override;
	void keyPressEvent(QKeyEvent* event) override;
	void keyReleaseEvent(QKeyEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseReleaseEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void hoverMoveEvent(QHoverEvent* event) override;
	void hoverEnterEvent(QHoverEvent* event) override;
	void hoverLeaveEvent(QHoverEvent* event) override;
	void mouseUngrabEvent() override;
	void wheelEvent(QWheelEvent* event) override;
	void focusInEvent(QFocusEvent* event) override;
	void focusOutEvent(QFocusEvent* event) override;

private:
	void tryAttach();
	void detach();
	void onReadable();
	void handleLine(const QByteArray& line);
	void send(const QByteArray& line);
	void mapFrame(int fd, int w, int h, int stride);
	void unmapFrame();
	void addSurface(const QList<QByteArray>& w);
	void forgetSurfaces();
	void showSurfaces(const QList<QByteArray>& w);
	void sendLater(const QByteArray& line);
	void setState(const QString& state, int code = 0);
	void scheduleSize();
	void sendSize();
	void sendClipboard(bool force = false);
	void offerClipboard(const QString& text, bool force);
	void updateCapture();
	bool asksForShortcuts() const; // the asking without the release latch (captureEligible minus it)
	bool captureEligible() const; // whether this view asks for the shortcuts, before the compositor's say
	void updateCaptureState();
	void onInhibitActivation(); // the compositor's active/inactive arrived
	void loseKeyboard(); // keys lifted and the session told it lost the keyboard
	void releaseHeldButtons(); // buttons lifted at their last remote place, once per press
	QRectF imageRect() const;
	QPoint toRemote(const QPointF& p) const;
	qreal devicePixelRatio() const;

	QString m_socketPath;
	QString m_state = QStringLiteral("detached");
	int m_exitCode = 0;
	int m_fd = -1;
	QSocketNotifier* m_notifier = nullptr;
	QByteArray m_inbuf;
	bool m_skipLine = false; // dropping the rest of an overlong line, up to its newline
	std::deque<int> m_fds;
	QTimer m_attachTimer;
	QTimer m_sizeTimer;

	QImage m_image;
	bool m_dirty = false;

	// Gliff pictures as dmabufs (yuvnode.h): the surfaces the session handed over, the newest
	// "show" and whether the render thread took it, and the one it draws (released to the
	// session once a newer one replaces it).
	bool m_gpu = false; // drawing the shown surfaces, not m_image
	bool m_gpuFailed = false; // this GPU could not import them: never ask again
	bool m_nodeGpu = false;
	QHash<quint32, DmabufSurface> m_surfaces;
	quint64 m_surfaceGeneration = 1;
	struct Show
	{
		quint64 seq = 0;
		quint32 main = 0, aux = 0;
		bool dual = false;
		QSize size;
	} m_show;
	bool m_showTaken = true;
	quint64 m_drawnSeq = 0;

	QVariantMap m_prompt;
	int m_desktopScale = 0;
	bool m_followSize = true;
	bool m_captureShortcuts = false;
	QString m_keyCaptureMode = QStringLiteral("auto");
	QString m_sessionProtocol;
	QString m_captureState = QStringLiteral("ineligible");
	bool m_released = false; // the release key gave the shortcuts back; a click or leaving undoes it
	bool m_pointerInside = false; // "auto" takes the shortcuts only with the pointer on the picture
	QString m_releaseKey = QStringLiteral("shift+escape");
	qint64 m_lastEscape = 0; // for double-escape
	ShortcutInhibit m_inhibit;
	QString m_inputProfile = QStringLiteral("default");
	QString m_inputTrigger1, m_inputAction1, m_inputTrigger2, m_inputAction2;
	QSet<quint32> m_mappedPressed;
	QSet<quint32> m_pressed;
	QHash<quint32, quint32> m_pressedKeysyms;
	// Keys lifted without their releases having arrived (the keyboard left while they were
	// held): their repeats are not new presses, and a late release doubles nothing.
	QSet<quint32> m_stalePressed;
	bool m_keyboardBlurred = false;
	// A key that came with modifiers no modifier key was pressed for (Hyprland's sendshortcut, as
	// Omarchy's Super+V/C/X use): the modifiers sent around it, and a held Super lifted for it.
	// evdev code and keysym of each, by the key's evdev code.
	struct Chord { QList<QPair<quint32, quint32>> added, lifted; };
	QHash<quint32, Chord> m_chords;
	struct CachedCursor { QCursor cursor; quint64 used = 0; };
	QHash<quint32, CachedCursor> m_cursors;
	quint64 m_cursorClock = 0;
	QString m_lastClip;
	// Text from the session while this view did not have the keyboard: set when it gets it back,
	// dropped when the desktop's clipboard changes first (whichever changed last wins, as Gliff).
	// A server never rewrites the clipboard while the user works elsewhere.
	QString m_heldClip;
	bool m_hasHeldClip = false;
	void applyHeldClip();
	bool m_shareClipboard = false;
	bool m_clipReading = false; // a read is under way; one more follows it when asked meanwhile
	bool m_clipAgain = false;
	bool m_clipAgainForce = false;
	// Buttons pressed on the remote through this view, and where the pointer was last sent: a
	// drag may legitimately run past the item's edge, so leaving lifts nothing, but a lost
	// grab, keyboard, window or session lifts them (a remote button left down drags alone).
	Qt::MouseButtons m_buttons = Qt::NoButton;
	QPoint m_lastRemotePos;
};
