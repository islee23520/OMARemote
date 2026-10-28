// RdpView's keyboard and mouse state machines against a stand-in session socket: presses,
// repeats, releases and cleanup on lost focus, grabs, visibility and sessions. Prints FAIL
// lines, exit 1. Needs QT_QPA_PLATFORM=offscreen; compositor shortcut capture is out of scope.
#include "rdpview.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFocusEvent>
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
static void expectWire(const QByteArray& actual, const QByteArray& expected, const char* what)
{
	if (actual != expected)
		fprintf(stderr, "Actual wire: %s\nExpected wire: %s\n", actual.constData(), expected.constData());
	expect(actual == expected, what);
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
	using RdpView::mousePressEvent;
	using RdpView::mouseReleaseEvent;
	using RdpView::mouseMoveEvent;
	using RdpView::mouseUngrabEvent;
	using RdpView::wheelEvent;
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
	bool frame()
	{
		QTemporaryFile file;
		if (!file.open() || !file.resize(100 * 100 * 4))
			return false;
		char line[] = "frame 100 100 400\n";
		iovec io { line, sizeof(line) - 1 };
		alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
		msghdr msg {};
		msg.msg_iov = &io;
		msg.msg_iovlen = 1;
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);
		cmsghdr* c = CMSG_FIRSTHDR(&msg);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int));
		const int fd = file.handle();
		memcpy(CMSG_DATA(c), &fd, sizeof(fd));
		return sendmsg(m_client, &msg, MSG_NOSIGNAL) == ssize_t(sizeof(line) - 1);
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
// it is a repeat, the modifiers, and the native virtual key the compositor would report.
static QKeyEvent key(int evdev, int keyc, bool press, bool repeat = false,
                     Qt::KeyboardModifiers mods = Qt::NoModifier, int virtualKey = 0)
{
	return QKeyEvent(press ? QEvent::KeyPress : QEvent::KeyRelease, keyc, mods, evdev + 8,
	                 virtualKey, 0, QString(), repeat);
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

static void mouse(TestView& v, QEvent::Type type, Qt::MouseButton button,
                  const QPointF& pos = QPointF(20, 30))
{
	QMouseEvent event(type, pos, pos, button,
	                  type == QEvent::MouseButtonRelease ? Qt::NoButton : button, Qt::NoModifier);
	if (type == QEvent::MouseButtonPress)
		v.mousePressEvent(&event);
	else if (type == QEvent::MouseButtonRelease)
		v.mouseReleaseEvent(&event);
	else
		v.mouseMoveEvent(&event);
}

static void frame(Session& s, TestView& v)
{
	v.setSize(QSizeF(100, 100));
	expect(s.frame(), "the session sends a real framebuffer fd");
	expect(waitUntil([&] { return v.remoteSize() == QSize(100, 100); }),
	       "the view maps the framebuffer");
}

int main(int argc, char** argv)
{
	QGuiApplication app(argc, argv);
	QTemporaryDir dir;
	const QString path = dir.filePath("session.sock");
	QByteArray lines;
	// Mapping is opt-in, pairs every synthetic key, and never releases a held modifier.
	for (const QString& protocol : {QStringLiteral("vnc"), QStringLiteral("rdp"), QStringLiteral("gliff"), QStringLiteral("ssh")})
	{
		Session s;
		expect(s.start(path), "mapping session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "mapping view attaches");
		v.setSessionProtocol(protocol);
		v.setProperty("inputTrigger1", "58");
		v.setProperty("inputAction1", "29:65507+57:32");
		QKeyEvent down = key(58, Qt::Key_CapsLock, true, false, Qt::NoModifier, 65509);
		QKeyEvent up = key(58, Qt::Key_CapsLock, false);
		v.keyPressEvent(&down); v.keyReleaseEvent(&up);
		lines = s.heard();
		expect(lines.contains("key 1 58 65509\n") && lines.contains("key 0 58\n"), "default passes key unchanged");
		v.setProperty("inputProfile", "custom");
		v.keyPressEvent(&down); v.keyReleaseEvent(&up);
		lines = s.heard();
		if (protocol == QLatin1String("ssh"))
			expect(lines.contains("key 1 58 65509\n"), "SSH retains original key");
		else
		{
			expect(lines == "key 1 29 65507\nkey 1 57 32\nkey 0 57\nkey 0 29\n", "configured chord is paired in order");
			QKeyEvent ctrl = key(97, Qt::Key_Control, true, false, Qt::ControlModifier, 65508);
			QKeyEvent ctrlUp = key(97, Qt::Key_Control, false);
			v.keyPressEvent(&ctrl); s.heard();
			v.keyPressEvent(&down);
			QKeyEvent repeat = key(58, Qt::Key_CapsLock, true, true);
			v.keyPressEvent(&repeat);
			v.releaseAllKeys();
			v.keyReleaseEvent(&up); v.keyReleaseEvent(&ctrlUp);
			lines = s.heard();
			expect(lines == "key 1 57 32\nkey 0 57\nkey 0 97\n", "held right control preserved and cleanup suppresses late releases");
			QKeyEvent shortcut = key(30, Qt::Key_A, true, false, Qt::ControlModifier, 97);
			QKeyEvent shortcutUp = key(30, Qt::Key_A, false);
			v.keyPressEvent(&shortcut); s.heard();
			v.keyPressEvent(&down); v.keyReleaseEvent(&up);
			v.keyReleaseEvent(&shortcutUp);
			lines = s.heard();
			expect(lines == "key 1 57 32\nkey 0 57\nkey 0 30\nkey 0 29\n", "mapping leaves existing synthetic control held");
		}
	}
	{
		Session s;
		expect(s.start(path), "review regression session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "review regression view attaches");
		v.setSessionProtocol("vnc");
		v.setProperty("inputProfile", "custom");
		v.setProperty("inputTrigger1", "58");
		v.setProperty("inputAction1", "56:65513+57:32");
		QKeyEvent altgr = key(100, Qt::Key_AltGr, true, false, Qt::NoModifier, 65027);
		QKeyEvent caps = key(58, Qt::Key_CapsLock, true, false, Qt::NoModifier, 65509);
		QKeyEvent capsUp = key(58, Qt::Key_CapsLock, false);
		v.keyPressEvent(&altgr); s.heard();
		v.keyPressEvent(&caps); v.keyReleaseEvent(&capsUp);
		expect(s.heard() == "key 1 56 65513\nkey 1 57 32\nkey 0 57\nkey 0 56\n", "AltGr does not substitute for left Alt");
		v.releaseAllKeys(); s.heard();
		QKeyEvent ctrl = key(29, Qt::Key_Control, true, false, Qt::ControlModifier, 65507);
		QKeyEvent ctrlUp = key(29, Qt::Key_Control, false);
		v.setProperty("inputAction1", "100:65507+57:32");
		v.keyPressEvent(&ctrl); s.heard();
		v.keyPressEvent(&caps); v.keyReleaseEvent(&capsUp);
		expect(s.heard() == "key 1 57 32\nkey 0 57\n", "VNC held keysym remains held despite different evdev");
		v.keyReleaseEvent(&ctrlUp); s.heard();
		v.setProperty("inputTrigger1", "29");
		v.setProperty("inputAction1", "122:65329");
		v.keyPressEvent(&ctrl); s.heard();
		QKeyEvent a = key(30, Qt::Key_A, true, false, Qt::ControlModifier, 97);
		QKeyEvent aUp = key(30, Qt::Key_A, false);
		v.keyPressEvent(&a); v.keyReleaseEvent(&aUp); v.keyReleaseEvent(&ctrlUp);
		expect(s.heard() == "key 1 30 97\nkey 0 30\n", "mapped Ctrl is not synthesized for subsequent keys");
	}
	{
		Session s;
		expect(s.start(path), "unsupported RDP mapping session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "unsupported RDP mapping view attaches");
		v.setSessionProtocol("rdp");
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "RDP mapping session connects");
		s.heard(); // discard connection setup before checking key wire output
		v.setProperty("inputProfile", "custom");
		v.setProperty("inputTrigger1", "58");
		v.setProperty("inputAction1", "29:65507+183:65482");
		QKeyEvent unsupported = key(58, Qt::Key_CapsLock, true, false, Qt::NoModifier, 65509);
		v.keyPressEvent(&unsupported);
		QKeyEvent unsupportedUp = key(58, Qt::Key_CapsLock, false);
		v.keyReleaseEvent(&unsupportedUp);
		const QByteArray wire = s.heard();
		expectWire(wire, "key 1 58 65509\nkey 0 58\n", "unsupported RDP action falls back unchanged without partial emission");
	}
	// SSH receives unkeyed text separately, and keyed text only on its key line.
	{
		Session s;
		expect(s.start(path), "the SSH text session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "the SSH text view attaches");
		v.setSessionProtocol(QStringLiteral("ssh"));
		s.heard(); // attach/protocol setup may send control lines
		QKeyEvent unkeyed(QEvent::KeyPress, 0, Qt::NoModifier, 0, 0, 0,
		                  QString::fromUtf8("\xc3\xa9"));
		v.keyPressEvent(&unkeyed);
		expect(s.heard() == "text w6k=\n", "SSH unkeyed UTF-8 text sends exactly one base64 text line");
		QKeyEvent empty(QEvent::KeyPress, 0, Qt::NoModifier, 0, 0, 0, QString());
		v.keyPressEvent(&empty);
		expect(s.heard().isEmpty(), "an unkeyed event without text sends nothing");
		QKeyEvent ordinary(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 38, 97, 0,
		                   QStringLiteral("a"));
		v.keyPressEvent(&ordinary);
		expect(s.heard() == "key 1 30 97 YQ==\n", "SSH ordinary keyed text sends exactly one encoded key line");
		QKeyEvent ordinaryUp = key(30, Qt::Key_A, false);
		v.keyReleaseEvent(&ordinaryUp);
		s.heard();
		QKeyEvent composed(QEvent::KeyPress, Qt::Key_E, Qt::NoModifier, 26, 101, 0,
		                   QString::fromUtf8("\xc3\xa9"));
		v.keyPressEvent(&composed);
		expect(s.heard() == "key 1 18 101 w6k=\n", "SSH keyed compose text sends exactly one encoded key line");
	}
	for (const QString& mode : { QStringLiteral("on"), QStringLiteral("auto") })
	{
		Session s;
		expect(s.start(path), "the mouse session listens");
		QQuickWindow window;
		TestView v;
		expect(s.acceptFrom(&v, path), "the mouse view attaches");
		v.setSessionProtocol(QStringLiteral("vnc"));
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "the mouse session connects");
		frame(s, v);
		v.setKeyCaptureMode(mode);
		expect(focusInView(v, window), "the mouse view owns a focused window");
		s.heard(); // exclude setup focus and blur messages
		mouse(v, QEvent::MouseButtonPress, Qt::LeftButton);
		QWheelEvent wheel(QPointF(40, 50), QPointF(40, 50), QPoint(), QPoint(0, 120),
		                  Qt::LeftButton, Qt::NoModifier, Qt::NoScrollPhase, false);
		v.wheelEvent(&wheel);
		QHoverEvent leave(QEvent::HoverLeave, QPointF(), QPointF(), QPointF(40, 50));
		v.hoverLeaveEvent(&leave);
		lines = s.heard();
		expect(lines.count("button 1 1 20 30\n") == 1, "the drag press reaches the session");
		expect(lines.count("wheel 120 0 40 50\n") == 1, "wheel is forwarded while held");
		expect(!lines.contains("button 1 0") && !lines.contains("blur"),
		       "pointer leave and wheel preserve a held button in on and auto modes");
		mouse(v, QEvent::MouseButtonRelease, Qt::LeftButton, QPointF(60, 70));
		expect(s.heard().count("button 1 0 60 70\n") == 1,
		       "physical release finishes the drag after pointer leave");
	}

	for (bool ungrab : { false, true })
	{
		Session s;
		expect(s.start(path), "the cleanup session listens");
		QQuickWindow window;
		TestView v;
		expect(s.acceptFrom(&v, path), "the cleanup view attaches");
		v.setSessionProtocol(QStringLiteral("vnc"));
		frame(s, v);
		expect(focusInView(v, window), "the cleanup view owns a focused window");
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "the cleanup session connects");
		s.heard();
		mouse(v, QEvent::MouseButtonPress, Qt::LeftButton, QPointF(65, 75));
		mouse(v, QEvent::MouseButtonPress, Qt::RightButton, QPointF(65, 75));
		s.heard(); // exclude the presses; verify only cleanup below
		QFocusEvent out(QEvent::FocusOut);
		for (int i = 0; i < 2; ++i)
		{
			if (ungrab)
				v.mouseUngrabEvent();
			else
				v.focusOutEvent(&out);
		}
		mouse(v, QEvent::MouseButtonRelease, Qt::LeftButton);
		mouse(v, QEvent::MouseButtonRelease, Qt::RightButton);
		lines = s.heard();
		expectWire(lines, QByteArray("button 1 0 65 75\nbutton 2 0 65 75\n") +
		                      (ungrab ? QByteArray() : QByteArray("blur\n")),
		           "cleanup emits exact per-button releases at last sent position");
		expect(lines.count("blur\n") == (ungrab ? 0 : 1),
		       "focus loss blurs once, mouse ungrab never blurs");
		mouse(v, QEvent::MouseButtonPress, Qt::LeftButton);
		mouse(v, QEvent::MouseButtonRelease, Qt::LeftButton);
		lines = s.heard();
		expect(lines.count("button 1 1 20 30\n") == 1 &&
		           lines.count("button 1 0 20 30\n") == 1,
		       "fresh mouse input works after cleanup");
	}

	// No key was pressed: these lifecycle paths must still lift mouse input.
	for (int cleanup = 0; cleanup < 3; ++cleanup)
	{
		Session s;
		expect(s.start(path), "the lifecycle session listens");
		QQuickWindow window;
		TestView v;
		expect(s.acceptFrom(&v, path), "the lifecycle view attaches");
		frame(s, v);
		expect(focusInView(v, window), "the lifecycle view owns a focused window");
		v.setSessionProtocol(QStringLiteral("vnc"));
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "the lifecycle session connects");
		mouse(v, QEvent::MouseButtonPress, Qt::MiddleButton);
		s.heard();
		if (cleanup == 0)
			v.setVisible(false);
		else if (cleanup == 1)
		{
			s.say("state ended 0");
			expect(waitUntil([&] { return v.state() == QLatin1String("ended"); }),
			       "the mouse-only session ends");
		}
		else
			v.setSocketPath(QString());
		lines = s.heard();
		expect(lines.count("button 3 0 20 30\n") == 1 && lines.count("blur\n") == 1,
		       "hide, session end and explicit detach lift mouse-only input and blur once");
		mouse(v, QEvent::MouseButtonRelease, Qt::MiddleButton);
		expect(!s.heard().contains("button "), "lifecycle cleanup suppresses a late release");
	}

	for (bool died : { false, true })
	{
		Session s;
		expect(s.start(path), "the old mouse session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "the old mouse view attaches");
		frame(s, v);
		mouse(v, QEvent::MouseButtonPress, Qt::LeftButton);
		s.heard();
		if (died)
		{
			s.close();
			expect(waitUntil([&] { return !v.attached(); }), "socket death detaches the mouse view");
		}
		const QString replacement = dir.filePath("replacement.sock");
		Session next;
		expect(next.start(replacement), "the replacement mouse session listens");
		expect(next.acceptFrom(&v, replacement), "the mouse view attaches to the replacement");
		if (!died)
			expectWire(s.heard(), "button 1 0 20 30\n",
			           "live replacement lifts the held button on old session before detach");
		mouse(v, QEvent::MouseButtonRelease, Qt::LeftButton);
		v.mouseUngrabEvent();
		expect(!next.heard().contains("button "), "no stale release reaches a replacement session");
		mouse(v, QEvent::MouseButtonPress, Qt::LeftButton);
		mouse(v, QEvent::MouseButtonRelease, Qt::LeftButton);
		lines = next.heard();
		expect(lines.count("button 1 1") == 1 && lines.count("button 1 0") == 1,
		       "fresh mouse input reaches the replacement session");
	}
	// A key press is sent once, repeats go as presses, the release as a release.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "the view attaches");
		v.setKeyCaptureMode(QStringLiteral("on"));
		QKeyEvent a = key(30, Qt::Key_A, true);
		QCoreApplication::sendEvent(&v, &a);
		QKeyEvent a2 = key(30, Qt::Key_A, true, true);
		QCoreApplication::sendEvent(&v, &a2);
		QKeyEvent aUp = key(30, Qt::Key_A, false);
		QCoreApplication::sendEvent(&v, &aUp);
		lines = s.heard();
		expect(lines.count("key 1 30") == 2, "a press and its repeat are forwarded");
		expect(lines.count("key 0 30") == 1, "the release is forwarded once");
	}
	// A key with no keysym (kb_options = caps:none: nativeVirtualKey 0xffffff, Qt key 0, no
	// text) still tells VNC what it is when we know the code: CapsLock goes as XK_Caps_Lock.
	// Unknown zero-keysyms keep the 0 they came with — the VNC side drops those.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "the view attaches");
		v.setKeyCaptureMode(QStringLiteral("on"));
		QKeyEvent caps = key(58, 0, true, false, Qt::NoModifier, 0xffffff);
		v.keyPressEvent(&caps);
		QKeyEvent capsUp = key(58, 0, false, false, Qt::NoModifier, 0xffffff);
		v.keyReleaseEvent(&capsUp);
		lines = s.heard();
		expect(lines.count("key 1 58 65509\n") == 1,
		       "keysym-less CapsLock is sent as XK_Caps_Lock (0xffe5 = 65509)");
		expect(lines.count("key 0 58\n") == 1, "the CapsLock release is forwarded");
		QKeyEvent capsZero = key(58, 0, true);
		v.keyPressEvent(&capsZero);
		QKeyEvent capsZeroUp = key(58, 0, false);
		v.keyReleaseEvent(&capsZeroUp);
		expect(s.heard().contains("key 1 58 65509\n"), "zero-keysym CapsLock uses XK_Caps_Lock");
		QKeyEvent other = key(70, 0, true);
		v.keyPressEvent(&other);
		QKeyEvent otherUp = key(70, 0, false);
		v.keyReleaseEvent(&otherUp);
		lines = s.heard();
		expect(lines.count("key 1 70 0\n") == 1,
		       "an untabled zero-keysym key stays zero (VNC drops it, as before)");
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
		expect(focusInView(v, window), "the pointer test owns a focused window");
		v.setSessionProtocol("vnc");
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "focused capture test session connects");
		v.setKeyCaptureMode(QStringLiteral("on"));
		s.heard(); // discard focus and connection setup notifications
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
		       "a focused asking view without compositor support says unsupported");
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

	// Session-local toggles work without compositor support but never claim actual capture.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		QQuickWindow window;
		expect(s.acceptFrom(&v, path), "the view attaches");
		v.setSessionProtocol(QStringLiteral("vnc"));
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "the view is connected");
		v.setKeyCaptureMode(QStringLiteral("auto"));
		expect(focusInView(v, window), "the view has the keyboard");
		expect(v.captureState() == QLatin1String("ineligible"), "VNC Auto starts without a request");
		expect(v.toggleShortcuts(), "eligible VNC toggle is accepted");
		expect(v.captureState() == QLatin1String("unsupported") && !v.shortcutsCaptured(),
		       "VNC request is unsupported without a compositor and never claims active");
		expect(v.toggleShortcuts(), "eligible toggle remains accepted");
		expect(v.captureState() == QLatin1String("released"), "second Auto toggle releases pending request");
		QKeyEvent late = key(48, Qt::Key_B, false);
		v.keyReleaseEvent(&late);
		expect(s.heard().count("key 0 48") == 0, "untracked B release is ignored without a prior press");
		v.toggleShortcuts();
		expect(v.captureState() == QLatin1String("unsupported"), "third Auto toggle requests again");
		v.setKeyCaptureMode(QStringLiteral("on"));
		expect(v.captureState() == QLatin1String("unsupported"), "On follows config as a pending request");
		v.releaseShortcuts();
		expect(v.captureState() == QLatin1String("released"), "release cancels pending On request");
		v.releaseShortcuts();
		expect(v.captureState() == QLatin1String("released"), "release with no request remains Released");
		v.setKeyCaptureMode(QStringLiteral("auto"));
		v.toggleShortcuts();
		expect(v.captureState() == QLatin1String("unsupported"), "Auto can be explicitly requested after release");
		QQuickWindow otherWindow;
		otherWindow.show();
		otherWindow.requestActivate();
		expect(waitUntil([&] { return otherWindow.isActive() && !window.isActive(); }),
		       "another window becomes active and deactivates the view window");
		expect(v.captureState() == QLatin1String("ineligible"), "window deactivation resets transient request");
		window.requestActivate();
		expect(waitUntil([&] { return window.isActive() && !otherWindow.isActive(); }),
		       "the view window is reactivated after the deactivation assertion");
	}
	// A pending Auto request consumes the second Escape but still lifts the first remotely held Escape.
	{
		Session s;
		expect(s.start(path), "pending Escape cleanup session listens");
		TestView v;
		QQuickWindow window;
		expect(s.acceptFrom(&v, path), "pending Escape cleanup view attaches");
		v.setSessionProtocol(QStringLiteral("vnc"));
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "pending Escape cleanup session connects");
		v.setKeyCaptureMode(QStringLiteral("auto"));
		v.setProperty("releaseKey", QStringLiteral("double-escape"));
		expect(focusInView(v, window), "pending Escape cleanup view focuses");
		s.heard();
		v.toggleShortcuts();
		QKeyEvent first = key(1, Qt::Key_Escape, true, false, Qt::NoModifier, 65307);
		v.keyPressEvent(&first);
		expectWire(s.heard(), "key 1 1 65307\n", "first Escape reaches the remote");
		QKeyEvent firstUp = key(1, Qt::Key_Escape, false);
		v.keyReleaseEvent(&firstUp);
		QKeyEvent second = key(1, Qt::Key_Escape, true, false, Qt::NoModifier, 65307);
		v.keyPressEvent(&second);
		QKeyEvent secondUp = key(1, Qt::Key_Escape, false);
		v.keyReleaseEvent(&secondUp);
		expectWire(s.heard(), "key 0 1\n", "double-Escape lifts the first remote press once");
		expect(v.captureState() == QLatin1String("released"), "double-Escape cancels pending capture request");
	}
	// Gliff reconnect resynchronizes backend focus after connected-state cleanup without focusIn.
	{
		Session s;
		expect(s.start(path), "Gliff reconnect focus session listens");
		TestView v;
		QQuickWindow window;
		expect(s.acceptFrom(&v, path), "Gliff reconnect focus view attaches");
		v.setSessionProtocol(QStringLiteral("gliff"));
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "Gliff session connects");
		expect(focusInView(v, window), "Gliff view focuses");
		s.heard();
		s.say("state connecting");
		expect(waitUntil([&] { return v.state() == QLatin1String("connecting"); }), "Gliff reconnect begins");
		s.heard();
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "Gliff reconnect completes");
		lines = s.heard();
		expectWire(lines, "focus 2\n", "focused Gliff canvas restores backend Num Lock focus after reconnect");
	}

	// Capture intent is distinct from compositor activation; offscreen cannot report active.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		QQuickWindow window;
		expect(s.acceptFrom(&v, path), "the view attaches");
		v.setSessionProtocol(QStringLiteral("vnc"));
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "the view is connected");
		v.setKeyCaptureMode(QStringLiteral("auto"));
		expect(focusInView(v, window), "the view has the keyboard");
		v.toggleShortcuts();
		v.focusOutEvent(new QFocusEvent(QEvent::FocusOut));
		expect(v.captureState() == QLatin1String("ineligible"), "focus loss returns to FollowConfig");
		v.focusInEvent(new QFocusEvent(QEvent::FocusIn));
		expect(v.captureState() == QLatin1String("ineligible"), "focus return does not auto-request VNC capture");
		v.toggleShortcuts();
		mouse(v, QEvent::MouseButtonPress, Qt::LeftButton);
		s.heard();
		v.setKeyCaptureMode(QStringLiteral("off"));
		expect(v.captureState() == QLatin1String("off"), "mode Off disables capture");
		expect(s.heard().count("button 1 0") == 0 && !v.shortcutsCaptured(),
		       "mode Off does not cancel the active drag");
		v.mouseUngrabEvent();
		expect(s.heard().count("button 1 0") == 1, "actual mouse ungrab releases the drag exactly once");
		v.setKeyCaptureMode(QStringLiteral("auto"));
		v.toggleShortcuts();
		s.say("state connecting");
		expect(waitUntil([&] { return v.state() == QLatin1String("connecting"); }), "connected to connecting is observed");
		expect(v.captureState() == QLatin1String("ineligible"), "reconnect transition resets request");
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "reconnect completes");
		expect(v.captureState() == QLatin1String("ineligible"), "Auto reconnect does not inherit request");
		v.setSessionProtocol(QStringLiteral("ssh"));
		expect(v.captureState() == QLatin1String("off"), "protocol replacement disables capture state");
		v.setSessionProtocol(QStringLiteral("vnc"));
		expect(v.captureState() == QLatin1String("ineligible"), "returning to desktop protocol starts at FollowConfig");
	}

	// On requests follow configuration; a pending request can be cancelled without an active event.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		QQuickWindow window;
		expect(s.acceptFrom(&v, path), "the view attaches");
		v.setSessionProtocol(QStringLiteral("vnc"));
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "On capture session connects");
		v.setKeyCaptureMode(QStringLiteral("on"));
		expect(focusInView(v, window), "the view has the keyboard");
		expect(v.captureState() == QLatin1String("unsupported"), "On requests capture by configuration");
		v.releaseShortcuts();
		expect(v.captureState() == QLatin1String("released"), "release cancels pending On request");
		v.releaseShortcuts();
		expect(v.captureState() == QLatin1String("released"), "release with no request remains Released");
		QKeyEvent b = key(48, Qt::Key_B, true);
		v.keyPressEvent(&b);
		expect(s.heard().count("key 1 48") == 1, "a key still types on remote after pending release");
	}

	// Both desktop protocols have Auto toggles and real RdpView key delivery. Offscreen has no compositor.
	for (const QString& protocol : {QStringLiteral("vnc"), QStringLiteral("rdp")})
	{
		Session s;
		expect(s.start(path), "desktop capture session listens");
		QQuickWindow window;
		TestView v;
		expect(s.acceptFrom(&v, path), "desktop capture view attaches");
		v.setSessionProtocol(protocol);
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "desktop session becomes connected");
		v.setKeyCaptureMode(QStringLiteral("auto"));
		expect(focusInView(v, window), "desktop canvas receives focus");
		s.heard();
		QKeyEvent b = key(48, Qt::Key_B, true, false, Qt::NoModifier, 98);
		QCoreApplication::sendEvent(&v, &b);
		QKeyEvent bUp = key(48, Qt::Key_B, false, false, Qt::NoModifier, 98);
		QCoreApplication::sendEvent(&v, &bUp);
		expectWire(s.heard(), "key 1 48 98\nkey 0 48\n", "real key press and release reach connected session");
		expect(v.captureState() == QLatin1String("ineligible"), "desktop Auto starts without capture request");
		expect(v.toggleShortcuts() && v.captureState() == QLatin1String("unsupported"), "desktop Auto requests capture without compositor");
		expect(v.toggleShortcuts() && v.captureState() == QLatin1String("released"), "second toggle cancels pending request");
		expect(v.toggleShortcuts() && v.captureState() == QLatin1String("unsupported"), "third toggle requests again");
		v.setVisible(false);
		expect(v.captureState() == QLatin1String("ineligible"), "hide clears the transient capture request");
		v.setVisible(true);
		expect(v.captureState() == QLatin1String("ineligible"), "show does not restore transient Auto request");
	}
	for (bool endSession : {false, true})
	{
		Session s;
		expect(s.start(path), "capture cleanup session listens");
		QQuickWindow window;
		TestView v;
		expect(s.acceptFrom(&v, path), "capture cleanup view attaches");
		v.setSessionProtocol(QStringLiteral("rdp"));
		s.say("state connected");
		expect(waitUntil([&] { return v.state() == QLatin1String("connected"); }), "capture cleanup session connects");
		v.setKeyCaptureMode(QStringLiteral("auto"));
		expect(focusInView(v, window), "capture cleanup view focuses");
		v.toggleShortcuts();
		if (endSession)
		{
			s.say("state ended 0");
			expect(waitUntil([&] { return v.state() == QLatin1String("ended"); }), "capture cleanup session ends");
		}
		else
			v.setSocketPath(QString());
		expect(v.captureState() == QLatin1String("ineligible"), "end or detach clears transient capture request");
	}
	if (failures == 0)
		printf("ok\n");
	return failures == 0 ? 0 : 1;
}
