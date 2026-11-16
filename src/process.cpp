#include "process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <vector>

namespace nix_sym {
namespace {

void set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

} // namespace

ProcessResult run_process(const std::vector<std::string>& argv, int timeout_seconds,
                          const std::string& stdin_data) {
  ProcessResult result;
  if (argv.empty()) {
    return result;
  }

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  int in_pipe[2] = {-1, -1};
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0 || (!stdin_data.empty() && pipe(in_pipe) != 0)) {
    if (out_pipe[0] >= 0)
      close(out_pipe[0]);
    if (out_pipe[1] >= 0)
      close(out_pipe[1]);
    if (err_pipe[0] >= 0)
      close(err_pipe[0]);
    if (err_pipe[1] >= 0)
      close(err_pipe[1]);
    if (in_pipe[0] >= 0)
      close(in_pipe[0]);
    if (in_pipe[1] >= 0)
      close(in_pipe[1]);
    return result;
  }

  std::vector<char*> args;
  args.reserve(argv.size() + 1);
  for (const auto& arg : argv) {
    args.push_back(const_cast<char*>(arg.c_str()));
  }
  args.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    if (in_pipe[0] >= 0)
      close(in_pipe[0]);
    if (in_pipe[1] >= 0)
      close(in_pipe[1]);
    return result;
  }

  if (pid == 0) {
    if (!stdin_data.empty()) {
      dup2(in_pipe[0], STDIN_FILENO);
    }
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    if (!stdin_data.empty()) {
      close(in_pipe[0]);
      close(in_pipe[1]);
    }
    execvp(args[0], args.data());
    _exit(127);
  }

  result.started = true;
  close(out_pipe[1]);
  close(err_pipe[1]);
  if (!stdin_data.empty()) {
    close(in_pipe[0]);
  }

  if (!stdin_data.empty()) {
    size_t written = 0;
    while (written < stdin_data.size()) {
      ssize_t n = write(in_pipe[1], stdin_data.data() + written, stdin_data.size() - written);
      if (n > 0) {
        written += static_cast<size_t>(n);
      } else if (n < 0 && (errno == EINTR)) {
        continue;
      } else {
        break;
      }
    }
    close(in_pipe[1]);
  }

  set_nonblocking(out_pipe[0]);
  set_nonblocking(err_pipe[0]);
  bool out_open = true;
  bool err_open = true;
  bool child_done = false;
  int status = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  char buffer[8192];

  while (out_open || err_open || !child_done) {
    if (!child_done) {
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        child_done = true;
      } else if (waited < 0 && errno != EINTR) {
        child_done = true;
      }
    }

    struct pollfd fds[2];
    int count = 0;
    if (out_open) {
      fds[count++] = {out_pipe[0], POLLIN, 0};
    }
    if (err_open) {
      fds[count++] = {err_pipe[0], POLLIN, 0};
    }
    if (count == 0 && child_done) {
      break;
    }

    int timeout_ms = 100;
    if (std::chrono::steady_clock::now() >= deadline) {
      if (!child_done) {
        kill(pid, SIGKILL);
        result.timed_out = true;
      } else {
        break;
      }
    }

    int polled = count > 0 ? poll(fds, static_cast<nfds_t>(count), timeout_ms) : 0;
    if (polled > 0) {
      for (int i = 0; i < count; ++i) {
        if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
          continue;
        }
        int fd = fds[i].fd;
        bool is_out = (fd == out_pipe[0]);
        while (true) {
          ssize_t n = read(fd, buffer, sizeof(buffer));
          if (n > 0) {
            (is_out ? result.out : result.err).append(buffer, static_cast<size_t>(n));
          } else if (n == 0) {
            if (is_out) {
              out_open = false;
            } else {
              err_open = false;
            }
            break;
          } else if (errno == EINTR) {
            continue;
          } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          } else {
            if (is_out) {
              out_open = false;
            } else {
              err_open = false;
            }
            break;
          }
        }
      }
    }

    if (std::chrono::steady_clock::now() >= deadline && !child_done) {
      kill(pid, SIGKILL);
      result.timed_out = true;
    }
  }

  while (!child_done) {
    pid_t waited = waitpid(pid, &status, 0);
    if (waited == pid) {
      child_done = true;
    } else if (waited < 0 && errno != EINTR) {
      break;
    }
  }

  close(out_pipe[0]);
  close(err_pipe[0]);
  if (!stdin_data.empty()) {
    close(in_pipe[1]);
  }

  result.finished = true;
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.signal = WTERMSIG(status);
    result.exit_code = 128 + result.signal;
  }
  return result;
}

} // namespace nix_sym
