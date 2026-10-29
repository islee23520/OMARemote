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
	using RdpView::mousePressEvent;
	using RdpView::mouseReleaseEvent;
	using RdpView::mouseMoveEvent;
	using RdpView::mouseUngrabEvent;
	using RdpView::wheelEvent;
	using RdpView::toggleShortcuts;
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
// it is a repeat, and the modifiers the compositor would report with it. The virtual key is the
// keysym Qt would carry (0 or 0xffffff when the layout option leaves the key without one).
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
		QKeyEvent down = key(58, Qt::Key_CapsLock, true);
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
		QKeyEvent caps = key(58, Qt::Key_CapsLock, true);
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
		v.setSessionProtocol("rdp");
		v.setProperty("inputTrigger1", "58");
		v.setProperty("inputAction1", "29:65507+183:65482");
		v.keyPressEvent(&caps); v.keyReleaseEvent(&capsUp);
		expect(s.heard() == "key 1 58 65509\nkey 0 58\n", "unsupported RDP action is never partly emitted");
	}
	{
		Session s;
		expect(s.start(path), "macos hangul session listens");
		TestView v;
		expect(s.acceptFrom(&v, path), "macos hangul view attaches");
		v.setSessionProtocol("vnc");
		v.setProperty("inputProfile", "macos");
		s.heard();
		QKeyEvent hangul = key(122, 0, true, false, Qt::NoModifier, 65329);
		QKeyEvent hangulUp = key(122, 0, false);
		v.keyPressEvent(&hangul); v.keyReleaseEvent(&hangulUp);
		expect(s.heard() == "key 1 29 65507\nkey 1 57 32\nkey 0 57\nkey 0 29\n",
		       "macOS VNC turns the Hangul key into Control+Space");
		QKeyEvent a = key(30, Qt::Key_A, true, false, Qt::NoModifier, 97);
		QKeyEvent aUp = key(30, Qt::Key_A, false);
		v.keyPressEvent(&a); v.keyReleaseEvent(&aUp);
		expect(s.heard() == "key 1 30 97\nkey 0 30\n", "macOS VNC leaves Latin keys physical");
		v.setProperty("macInputHelper", true);
		v.keyPressEvent(&hangul); v.keyReleaseEvent(&hangulUp);
		expect(s.heard() == "key 1 122 65329\nkey 0 122\n", "Mac helper receives physical Hangul without the legacy chord");
		QKeyEvent helperHanja = key(123, 0, true, false, Qt::NoModifier, 65332);
		QKeyEvent helperHanjaUp = key(123, 0, false);
		v.keyPressEvent(&helperHanja); v.keyReleaseEvent(&helperHanjaUp);
		expect(s.heard() == "key 1 123 65332\nkey 0 123\n", "Mac helper receives physical Hanja");
		v.setProperty("inputTrigger1", "122");
		v.setProperty("inputAction1", "42:65505+57:32");
		v.keyPressEvent(&hangul); v.keyReleaseEvent(&hangulUp);
		expect(s.heard() == "key 1 42 65505\nkey 1 57 32\nkey 0 57\nkey 0 42\n", "helper preserves explicit user mapping");
		v.setProperty("inputTrigger1", "");
		v.setProperty("inputAction1", "");
		v.setProperty("macInputHelper", false);
		v.keyPressEvent(&hangul); v.keyReleaseEvent(&hangulUp);
		expect(s.heard() == "key 1 29 65507\nkey 1 57 32\nkey 0 57\nkey 0 29\n", "disabling helper restores the legacy chord");
		v.setProperty("inputProfile", "default");
		v.keyPressEvent(&hangul); v.keyReleaseEvent(&hangulUp);
		expect(s.heard() == "key 1 122 65329\nkey 0 122\n", "default never rewrites the Hangul key");
		v.setProperty("inputProfile", "windows");
		v.keyPressEvent(&hangul); v.keyReleaseEvent(&hangulUp);
		expect(s.heard() == "key 1 122 65329\nkey 0 122\n", "Windows still receives XK_Hangul");
		v.setProperty("inputProfile", "macos");
		v.setProperty("inputTrigger1", "58");
		v.setProperty("inputAction1", "42:65505+57:32");
		QKeyEvent caps = key(58, Qt::Key_CapsLock, true);
		QKeyEvent capsUp = key(58, Qt::Key_CapsLock, false);
		v.keyPressEvent(&caps); v.keyReleaseEvent(&capsUp);
		expect(s.heard() == "key 1 42 65505\nkey 1 57 32\nkey 0 57\nkey 0 42\n",
		       "an explicit macOS mapping is not replaced by Control+Space");
		v.setProperty("inputTrigger1", "122");
		v.setProperty("inputAction1", "not-a-chord");
		v.setProperty("inputTrigger2", "123");
		v.setProperty("inputAction2", "56:65513+57:32");
		QKeyEvent hanja = key(123, 0, true, false, Qt::NoModifier, 65332);
		QKeyEvent hanjaUp = key(123, 0, false);
		v.keyPressEvent(&hanja); v.keyReleaseEvent(&hanjaUp);
		expect(s.heard() == "key 1 56 65513\nkey 1 57 32\nkey 0 57\nkey 0 56\n",
		       "a malformed first mapping does not swallow the second");
		v.keyPressEvent(&hangul); v.keyReleaseEvent(&hangulUp);
		expect(s.heard() == "key 1 122 65329\nkey 0 122\n",
		       "a mapped Hangul key is not rewritten when its action is unusable");
	}

	for (const QString& mode : { QStringLiteral("on"), QStringLiteral("auto") })
	{
		Session s;
		expect(s.start(path), "the mouse session listens");
		QQuickWindow window;
		TestView v;
		expect(s.acceptFrom(&v, path), "the mouse view attaches");
		frame(s, v);
		v.setKeyCaptureMode(mode);
		expect(focusInView(v, window), "the mouse view owns a focused window");
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
		TestView v;
		expect(s.acceptFrom(&v, path), "the cleanup view attaches");
		frame(s, v);
		mouse(v, QEvent::MouseButtonPress, Qt::LeftButton);
		mouse(v, QEvent::MouseButtonPress, Qt::RightButton);
		mouse(v, QEvent::MouseMove, Qt::NoButton, QPointF(65, 75));
		s.heard();
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
		expect(lines.count("button 1 0 65 75\n") == 1 &&
		           lines.count("button 2 0 65 75\n") == 1 && lines.count("button ") == 2,
		       "cleanup lifts each button once at the last sent position and suppresses late ups");
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
			expect(s.heard().count("button 1 0") == 1,
			       "live replacement releases the button to the old session");
		mouse(v, QEvent::MouseButtonRelease, Qt::LeftButton);
		v.mouseUngrabEvent();
		expect(!next.heard().contains("button "), "no stale release reaches a replacement session");
		mouse(v, QEvent::MouseButtonPress, Qt::LeftButton);
		mouse(v, QEvent::MouseButtonRelease, Qt::LeftButton);
		lines = next.heard();
		expect(lines.count("button 1 1") == 1 && lines.count("button 1 0") == 1,
		       "fresh mouse input reaches the replacement session");
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

	// Release and rearm are no-ops without a real inhibitor activation; only the compositor
	// can confirm the capture needed by releaseShortcuts().
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

	// The Ctrl+Alt+G chord, offscreen so no compositor is ever on the other end: the intent
	// (asking or not) is still visible in the "released" state and the toggle's answers, and
	// the compositor's own truth never comes from here. Repeat handling is supplied explicitly.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		QQuickWindow window;
		expect(s.acceptFrom(&v, path), "the view attaches");
		// "auto" on a plain RDP session: the pointer never asks, the chord does.
		v.setSessionProtocol(QStringLiteral("rdp"));
		expect(focusInView(v, window), "the view has the keyboard");
		expect(v.captureState() == QLatin1String("ineligible"),
		       "auto without the pointer does not ask");
		expect(v.toggleShortcuts(), "the toggle asks on an auto RDP session");
		expect(v.captureState() == QLatin1String("unsupported"),
		       "a chord request asks the compositor's protocol (unsupported here)");
		expect(v.toggleShortcuts(true), "a repeated toggle is consumed");
		expect(v.captureState() == QLatin1String("unsupported"),
		       "a repeated toggle does not release the request");
		expect(v.toggleShortcuts(), "the next physical toggle releases the request");
		expect(v.captureState() == QLatin1String("released"),
		       "the chord's release shows as released");
		v.rearmShortcuts();
		expect(v.captureState() == QLatin1String("ineligible"),
		       "rearm clears the latch, back to the stored policy (auto asks nothing here)");
		// "off" is off: the toggle acts on nothing and leaves G unmatched.
		v.setKeyCaptureMode(QStringLiteral("off"));
		expect(v.captureState() == QLatin1String("off"), "off is off before the chord");
		expect(!v.toggleShortcuts(), "the toggle does not act in off");
		expect(v.captureState() == QLatin1String("off"), "off is off after the chord");
		// The chord's G takes them again from a release, and the say-so dies with the context:
		// eligibility is re-checked every time.
		v.setKeyCaptureMode(QStringLiteral("auto"));
		v.releaseShortcuts();
		expect(v.captureState() == QLatin1String("ineligible"),
		       "a release in auto with the pointer out is just ineligible");
		expect(v.toggleShortcuts(), "the toggle asks again");
		expect(v.captureState() == QLatin1String("unsupported"),
		       "the request stands until the next toggle or a context loss");
		// Without a session there is no context: nothing acts, nothing swallows G.
		v.setSocketPath(QString());
		expect(waitUntil([&] { return !v.attached(); }), "the view detaches");
		expect(!v.toggleShortcuts(), "the toggle does not act without a session");
		expect(v.captureState() == QLatin1String("ineligible"), "no session is ineligible");
	}
	// A released override is rearmed by each of the prior ownership-return actions.
	{
		Session s;
		expect(s.start(path), "the stand-in session listens");
		TestView v;
		QQuickWindow window;
		expect(s.acceptFrom(&v, path), "the view attaches");
		v.setSessionProtocol(QStringLiteral("rdp"));
		v.setKeyCaptureMode(QStringLiteral("on"));
		expect(focusInView(v, window), "the view has the keyboard");
		expect(v.toggleShortcuts() && v.captureState() == QLatin1String("released"),
		       "G releases an asking view");
		QHoverEvent enter(QEvent::HoverEnter, QPointF(10, 10), QPointF(), QPointF(10, 10));
		v.hoverEnterEvent(&enter);
		expect(v.captureState() == QLatin1String("unsupported"),
		       "pointer re-entry rearms the stored capture policy");
		expect(v.toggleShortcuts() && v.captureState() == QLatin1String("released"),
		       "G releases capture again before click rearm");
		mouse(v, QEvent::MouseButtonPress, Qt::LeftButton);
		expect(v.captureState() == QLatin1String("unsupported"),
		       "click rearms the stored capture policy");
		expect(v.toggleShortcuts() && v.captureState() == QLatin1String("released"),
		       "G releases capture again before focus rearm");
		QFocusEvent out(QEvent::FocusOut);
		v.focusOutEvent(&out);
		QFocusEvent in(QEvent::FocusIn);
		v.focusInEvent(&in);
		expect(v.captureState() == QLatin1String("unsupported"),
		       "keyboard focus return rearms the stored capture policy");
	}

	if (failures == 0)
		printf("ok\n");
	return failures == 0 ? 0 : 1;
}
