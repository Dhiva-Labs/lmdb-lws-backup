#include "alert.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

#include "log.h"
#include "manifest.h"  // rfc3339_utc_now

namespace lwsbk {

namespace {

std::string json_str(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

}  // namespace

void fire_failure_webhook(const std::string& url, const std::string& stage,
                          const std::string& error_msg) {
  if (url.empty()) return;
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    LWSBK_WARN("alert webhook ignored: URL must start with http(s)://");
    return;
  }

  char host[256] = "unknown";
  gethostname(host, sizeof(host) - 1);

  std::string payload = "{\"event\":\"lmdb-lws-backup-failure\",\"stage\":" +
                        json_str(stage) + ",\"error\":" + json_str(error_msg) +
                        ",\"host\":" + json_str(host) +
                        ",\"timestamp\":" + json_str(rfc3339_utc_now()) + "}";

  int pfd[2];
  if (pipe(pfd) != 0) {
    LWSBK_WARN("alert webhook: pipe failed: %s", std::strerror(errno));
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    LWSBK_WARN("alert webhook: fork failed: %s", std::strerror(errno));
    ::close(pfd[0]);
    ::close(pfd[1]);
    return;
  }
  if (pid == 0) {
    // child: payload arrives on stdin so it never appears in argv
    dup2(pfd[0], STDIN_FILENO);
    ::close(pfd[0]);
    ::close(pfd[1]);
    execlp("curl", "curl", "-sS", "-m", "15", "-X", "POST", "-H",
           "Content-Type: application/json", "--data-binary", "@-",
           url.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  ::close(pfd[0]);
  const char* p = payload.data();
  size_t left = payload.size();
  while (left > 0) {
    ssize_t n = ::write(pfd[1], p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  ::close(pfd[1]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    LWSBK_WARN("alert webhook: waitpid failed: %s", std::strerror(errno));
    return;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    LWSBK_INFO("alert webhook delivered (stage: %s)", stage.c_str());
  } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
    LWSBK_WARN("alert webhook: curl binary not found in PATH");
  } else {
    LWSBK_WARN("alert webhook: curl exited with status %d",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  }
}

}  // namespace lwsbk
