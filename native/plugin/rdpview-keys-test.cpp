// RdpView's keyboard state machine against a stand-in session socket: what is sent for presses,
// repeats and releases, and what the cleanups (focus loss, the pointer leaving, the socket dying,
// the session ending) must and must not do. Prints FAIL lines, exit 1. Needs
// QT_QPA_PLATFORM=offscreen; the Wayland side (shortcut inhibit) has no part here, so nothing
// reports capture: these are the keyboard's boundaries, not the compositor's.
#include "rdpview.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QTemporaryDir>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <fcntl.h>
#include <QElapsedTimer>
#include <cstdio>
#include <cstring>
#include <functional>

static int failures = 0;

static void expect(bool ok, const char* what)
{
	if (!ok)
	{
		printf("FAIL %s\n", what);
		std::exit(1);
	}
}

// The view under test, with the protected event handlers reachable (the window would call them).
class TestView : public RdpView
{
  public:
	using RdpView::focusInEvent;
	using RdpView::focusOutEvent;
	using RdpView::hoverEnterEvent;
	using RdpView::hoverLeaveEvent;
	using RdpView::keyPressEvent;
	using RdpView::keyReleaseEvent;
};

// One Unix-socket session: what the view sent, and a way to speak as the session process.
class Session
{
  public:
	~Session() { close(); }
	bool start(const QString& path)
	{
		::unlink(path.toLocal8Bit().constData()); // a socket left by an earlier block
		m_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		sockaddr_un addr = {};
		addr.sun_family = AF_UNIX;
		const QByteArray p = path.toLocal8Bit();
		memcpy(addr.sun_path, p.constData(), static_cast<size_t>(p.size()));
		return m_fd >= 0 && bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
		       listen(m_fd, 1) == 0;
	}
	// Waits for the view to attach (it connects on its own); false when it never comes.
	bool acceptFrom(RdpView* view, const QString& path)
	{
		view->setSocketPath(path);
		const int flags = fcntl(m_fd, F_GETFL);
		fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
		QElapsedTimer since;
		since.start();
		for (;;)
		{
			m_client = accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
			if (m_client >= 0)
				return true;
			if (since.elapsed() > 5000)
				return false;
			QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		}
	}
	void say(const char* line)
	{
		QByteArray data(line);
		data += '\n';
		::send(m_client, data.constData(), static_cast<size_t>(data.size()), MSG_NOSIGNAL);
	}
	QByteArray heard()
	{
		char buf[4096];
		ssize_t n;
		while ((n = recv(m_client, buf, sizeof(buf), MSG_DONTWAIT)) > 0)
			m_in += QByteArray(buf, int(n));
		return std::exchange(m_in, QByteArray());
	}
	void close()
	{
		if (m_client >= 0)
			::close(m_client);
		if (m_fd >= 0)
			::close(m_fd);
		m_client = m_fd = -1;
	}

  private:
	int m_fd = -1, m_client = -1;
	QByteArray m_in;
};

static bool waitUntil(const std::function<bool()>& done, int ms = 3000)
{
	QElapsedTimer since;
	since.start();
	while (!done())
	{
		if (since.elapsed() >= ms)
			return false;
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
	}
	return true;
}

// press/release as a real key event: the evdev code (the native scan code is evdev + 8), whether
// it is a repeat, and the modifiers the compositor would report with it.
static QKeyEvent key(int evdev, int keyc, bool press, bool repeat = false,
                     Qt::KeyboardModifiers mods = Qt::NoModifier)
{
	return QKeyEvent(press ? QEvent::KeyPress : QEvent::KeyRelease, keyc, mods, evdev + 8, 0, 0,
	                 QString(), repeat);
}

// The window outlives the view: its item parent must still exist when the view detaches.
static bool focusInView(TestView& v, QQuickWindow& w)
{
	w.resize(300, 200);
	v.setParentItem(w.contentItem());
	w.show();
	v.forceActiveFocus();
	return waitUntil([&] { return v.hasActiveFocus() && w.isActive(); });
}

int main(int argc, char** argv)
{
	QGuiApplication app(argc, argv);
	QTemporaryDir dir;
	const QString path = dir.filePath("session.sock");
	QByteArray lines;

	// A key press is sent once, repeats go as presses, the release as a release.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "the view attaches");
		v.setKeyCaptureMode(QStringLiteral("on"));
		QKeyEvent a = key(30, Qt::Key_A, true);
		v.keyPressEvent(&a);
		QKeyEvent a2 = key(30, Qt::Key_A, true, true);
		v.keyPressEvent(&a2);
		QKeyEvent aUp = key(30, Qt::Key_A, false);
		v.keyReleaseEvent(&aUp);
		lines = s.heard();
		expect(lines.count("key 1 30") == 2, "a press and its repeat are forwarded");
		expect(lines.count("key 0 30") == 1, "the release is forwarded once");
	}

	// Focus loss lifts everything once and says blur; what is held over it comes back as
	// repeats, which are not presses, and a late release doubles nothing.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "the view attaches");
		QKeyEvent a = key(30, Qt::Key_A, true);
		v.keyPressEvent(&a);
		QKeyEvent ctrl = key(29, Qt::Key_Control, true);
		v.keyPressEvent(&ctrl);
		s.heard(); // the presses are not what this block is about
		QFocusEvent out(QEvent::FocusOut);
		v.focusOutEvent(&out);
		v.focusOutEvent(&out);
		QKeyEvent aRep = key(30, Qt::Key_A, true, true);
		v.keyPressEvent(&aRep);
		QKeyEvent aLate = key(30, Qt::Key_A, false);
		v.keyReleaseEvent(&aLate);
		lines = s.heard();
		expect(lines.count("key 0 30") == 1 && lines.count("key 0 29") == 1,
		       "held keys lift once on blur");
		expect(lines.count("blur") == 1, "blur is said once");
		expect(lines.count("key 1") == 0, "repeats of lifted keys are not presses");
		QKeyEvent aNew = key(30, Qt::Key_A, true);
		v.keyPressEvent(&aNew);
		lines = s.heard();
		expect(lines.count("key 1 30") == 1, "a fresh press after blur is forwarded");
	}

	// The pointer leaving in "on" mode lifts nothing: the pointer is not the keyboard. (In
	// "auto" the pointer leaving lifts the keys, but only while a compositor really holds the
	// shortcuts, which offscreen never offers; that path is the live smoke's, not this one's.)
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		QQuickWindow window;
		TestView v;
		expect(s.acceptFrom(&v, path), "the view attaches");
		v.setKeyCaptureMode(QStringLiteral("on"));
		expect(focusInView(v, window), "the view has the keyboard");
		QKeyEvent a = key(30, Qt::Key_A, true);
		v.keyPressEvent(&a);
		QHoverEvent enter(QEvent::HoverEnter, QPointF(10, 10), QPointF(), QPointF(10, 10));
		v.hoverEnterEvent(&enter);
		QHoverEvent leave(QEvent::HoverLeave, QPointF(), QPointF(), QPointF(10, 10));
		v.hoverLeaveEvent(&leave);
		lines = s.heard();
		expect(lines.count("key 0") == 0 && lines.count("blur") == 0,
		       "pointer leave lifts nothing in on mode");
		expect(v.captureState() == QLatin1String("unsupported"),
		       "a focused asking view without the protocol says unsupported");
		QKeyEvent aUp = key(30, Qt::Key_A, false);
		v.keyReleaseEvent(&aUp);
		expect(s.heard().count("key 0 30") == 1, "the release goes out after the pointer left");
	}

	// A modifier that came with a key it was never pressed for (Hyprland's sendshortcut) goes
	// up with it, once.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "the view attaches");
		QKeyEvent c = key(46, Qt::Key_C, true, false, Qt::ControlModifier);
		v.keyPressEvent(&c);
		lines = s.heard();
		expect(lines.count("key 1 29") == 1, "a missing Ctrl is pressed around the key");
		QKeyEvent cUp = key(46, Qt::Key_C, false);
		v.keyReleaseEvent(&cUp);
		v.releaseAllKeys();
		lines = s.heard();
		expect(lines.count("key 0 29") == 1, "the added Ctrl lifts once, not twice");
	}

	// The socket dying, and a new session on the same view: the old keys never reach the new
	// one as presses, a fresh key does.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "the view attaches");
		QKeyEvent a = key(30, Qt::Key_A, true);
		v.keyPressEvent(&a);
		s.close();
		expect(waitUntil([&] { return !v.attached(); }), "the view detaches when the socket dies");
		Session s2;
		expect(s2.start(path), "the second stand-in session listens");
		v.setSocketPath(QString());
		v.setSocketPath(path);
		expect(s2.acceptFrom(&v, path), "the view attaches again");
		QKeyEvent aRep = key(30, Qt::Key_A, true, true);
		v.keyPressEvent(&aRep);
		QKeyEvent d = key(32, Qt::Key_D, true);
		v.keyPressEvent(&d);
		lines = s2.heard();
		expect(lines.count("key 1 30") == 0,
		       "a key held over the swap presses nothing on the new session");
		expect(lines.count("key 1 32") == 1, "a fresh key reaches the new session");
	}

	// The session ending lifts the keys and says blur while the socket still carries lines.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "the view attaches");
		QKeyEvent a = key(30, Qt::Key_A, true);
		v.keyPressEvent(&a);
		s.heard();
		s.say("state ended 0");
		expect(waitUntil([&] { return v.state() == QLatin1String("ended"); }),
		       "the view takes the ended state");
		lines = s.heard();
		expect(lines.count("key 0 30") == 1 && lines.count("blur") == 1,
		       "the end lifts the keys and says blur");
	}

	// Off is off, and nothing here (no Wayland, no focus) reports the shortcuts held.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "the view attaches");
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }),
		       "the view takes the connected state");
		v.setKeyCaptureMode(QStringLiteral("off"));
		expect(v.captureState() == QLatin1String("off"), "off is off");
		expect(!v.shortcutsCaptured(), "shortcuts are never captured without a compositor");
		v.setKeyCaptureMode(QStringLiteral("on"));
		expect(v.captureState() == QLatin1String("ineligible"), "no focus is ineligible");
		expect(!v.shortcutsCaptured(), "ineligible is not captured");
	}

	// The release chord's counterpart. Without a compositor nothing is ever captured, so both
	// release and rearm are no-ops whatever the state says; the latch itself only a live
	// compositor can leave (releaseShortcuts runs on "active").
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		QQuickWindow window;
		expect(s.acceptFrom(&v, path), "the view attaches");
		v.setKeyCaptureMode(QStringLiteral("on"));
		expect(focusInView(v, window), "the view has the keyboard");
		expect(v.captureState() == QLatin1String("unsupported"),
		       "a focused asking view without the protocol says unsupported");
		v.releaseShortcuts();
		expect(v.captureState() == QLatin1String("unsupported"),
		       "release without capture changes nothing");
		v.rearmShortcuts();
		expect(v.captureState() == QLatin1String("unsupported"),
		       "rearm without the release latch changes nothing");
		QKeyEvent b = key(48, Qt::Key_B, true);
		v.keyPressEvent(&b);
		lines = s.heard();
		expect(lines.count("key 1 48") == 1, "a key still types on the remote");
	}

	if (failures == 0)
		printf("ok\n");
	return failures == 0 ? 0 : 1;
}
