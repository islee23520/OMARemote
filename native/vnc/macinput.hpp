#pragma once

// Keyboard-only SSH child. Like Gliff, use argv, private pipes and a separate session;
// unlike RFB writes, these pipes must never block framebuffer processing.
#include "../common/link.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

class MacInput
{
  public:
	std::vector<std::string> args;
	std::string error;
	bool ready = false;

	bool enabled() const { return !args.empty(); }

	bool start()
	{
		int pipes[6] = { -1, -1, -1, -1, -1, -1 };
		for (int i = 0; i < 6; i += 2)
			if (pipe2(pipes + i, O_CLOEXEC) != 0)
			{
				for (int fd : pipes)
					if (fd >= 0) close(fd);
				return fail("cannot create SSH pipes");
			}
		child = fork();
		if (child == 0)
		{
			dup2(pipes[0], STDIN_FILENO);
			dup2(pipes[3], STDOUT_FILENO);
			dup2(pipes[5], STDERR_FILENO);
			for (int fd : pipes) close(fd);
			setsid();
			std::vector<char*> argv;
			for (auto& arg : args) argv.push_back(arg.data());
			argv.push_back(nullptr);
			execvp(argv[0], argv.data());
			_exit(127);
		}
		if (child < 0)
		{
			for (int fd : pipes) close(fd);
			return fail("cannot start SSH child");
		}
		close(pipes[0]); close(pipes[3]); close(pipes[5]);
		input = pipes[1]; output = pipes[2]; diagnostics = pipes[4];
		for (int fd : { input, output, diagnostics })
			if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0)
				return fail("cannot make SSH pipes nonblocking");
		started = oma::now_ms();
		return true;
	}

	bool key(bool down, uint32_t code)
	{
		return queue("key " + std::to_string(int(down)) + " " + std::to_string(code) + "\n");
	}

	void release()
	{
		if (input < 0 || !ready) return;
		// Discard unsent input; finish only a partially written line before release.
		if (offset)
			pending.resize(pending.find('\n', offset) + 1);
		else
			pending.clear();
		pending += "release\n";
		queuedAt = oma::now_ms();
		awaitingPong = false;
	}

	bool pump()
	{
		if (!error.empty()) return false;
		char buf[2048];
		// Bound work per iteration even if a broken child continually writes stderr.
		for (int i = 0; i < 8 && diagnostics >= 0; ++i)
		{
			const ssize_t n = read(diagnostics, buf, sizeof(buf));
			if (n > 0)
			{
				tail.append(buf, size_t(n));
				if (tail.size() > 4096) tail.erase(0, tail.size() - 4096);
			}
			else if (n == 0) { close(diagnostics); diagnostics = -1; }
			else if (errno != EINTR && errno != EAGAIN) return fail("cannot read SSH stderr");
			else break;
		}
		for (int i = 0; i < 8; ++i)
		{
			const ssize_t n = read(output, buf, sizeof(buf));
			if (n == 0) return fail("SSH/helper closed its output");
			if (n < 0)
			{
				if (errno == EINTR) continue;
				if (errno == EAGAIN) break;
				return fail("cannot read SSH/helper output");
			}
			received.append(buf, size_t(n));
			size_t nl;
			while ((nl = received.find('\n')) != std::string::npos)
			{
				if (nl > 256) return fail("helper protocol line too long");
				const std::string line = received.substr(0, nl);
				received.erase(0, nl + 1);
				if (!ready && line == "ready 1") { ready = true; lastPing = oma::now_ms(); }
				else if (ready && line == "pong") awaitingPong = false;
				else return fail("unexpected helper response: " + line);
			}
			if (received.size() > 256) return fail("helper protocol line too long");
		}
		int status;
		const pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child) { child = -1; return fail("SSH/helper exited"); }
		if (result < 0 && errno != EINTR) return fail("cannot monitor SSH child");
		const uint64_t now = oma::now_ms();
		if (!ready && now - started >= 10000) return fail("timed out waiting for ready 1");
		if (ready && awaitingPong && now - lastPing >= 10000) return fail("helper heartbeat timed out");
		if (ready && !awaitingPong && now - lastPing >= 5000)
		{
			if (!queue("ping\n")) return false;
			lastPing = now;
			awaitingPong = true;
		}
		if (!flush()) return false;
		if (!pending.empty() && now - queuedAt >= 5000) return fail("SSH/helper input stalled");
		return true;
	}

	void stop()
	{
		if (input >= 0)
		{
			release();
			const uint64_t end = oma::now_ms() + 250;
			while (!pending.empty() && oma::now_ms() < end)
			{
				if (!flush()) break;
				pollfd fd = { input, POLLOUT, 0 };
				poll(&fd, 1, 10);
			}
			close(input); input = -1;
		}
		if (child > 0 && !reap(250))
		{
			kill(-child, SIGTERM);
			kill(child, SIGTERM); // setsid may not yet have run
			if (!reap(500))
			{
				kill(-child, SIGKILL);
				kill(child, SIGKILL);
				while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
				child = -1;
			}
		}
		for (int fd : { output, diagnostics }) if (fd >= 0) close(fd);
		output = diagnostics = -1;
	}

  private:
	pid_t child = -1;
	int input = -1, output = -1, diagnostics = -1;
	std::string pending, received, tail;
	size_t offset = 0;
	uint64_t started = 0, lastPing = 0, queuedAt = 0;
	bool awaitingPong = false;

	bool fail(const std::string& reason)
	{
		if (error.empty())
		{
			// EOF may race the last stderr write; collect it before reporting the failure.
			char buf[2048];
			for (int i = 0; i < 8 && diagnostics >= 0; ++i)
			{
				const ssize_t n = read(diagnostics, buf, sizeof(buf));
				if (n <= 0) break;
				tail.append(buf, size_t(n));
				if (tail.size() > 4096) tail.erase(0, tail.size() - 4096);
			}
			error = "Mac input helper: " + reason +
			        ". Check SSH BatchMode access, remote omaremote-mac-input installation and Accessibility approval";
			if (!tail.empty()) error += "; SSH stderr: " + tail;
			oma::log("%s", error.c_str());
		}
		return false;
	}

	bool queue(const std::string& line)
	{
		if (!error.empty()) return false;
		if (pending.size() + line.size() > 65536) return fail("keyboard queue overflow");
		if (pending.empty()) queuedAt = oma::now_ms();
		pending += line;
		return true;
	}

	bool flush()
	{
		if (offset == pending.size()) { pending.clear(); offset = 0; return true; }
		const ssize_t n = write(input, pending.data() + offset, pending.size() - offset);
		if (n > 0)
		{
			offset += size_t(n);
			// Retain only the current partially written line and unsent lines.
			const size_t last = pending.rfind('\n', offset - 1);
			if (last != std::string::npos) { pending.erase(0, last + 1); offset -= last + 1; }
			if (pending.empty()) offset = 0;
			return true;
		}
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) return true;
		return fail("cannot write SSH/helper input");
	}

	bool reap(uint64_t timeout)
	{
		const uint64_t end = oma::now_ms() + timeout;
		do
		{
			const pid_t result = waitpid(child, nullptr, WNOHANG);
			if (result == child || (result < 0 && errno == ECHILD)) { child = -1; return true; }
			usleep(10000);
		} while (oma::now_ms() < end);
		return false;
	}
};
