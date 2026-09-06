#include "bedrockkv/fs_util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace bedrockkv::fs {
namespace {

std::string ErrnoText() { return std::string(std::strerror(errno)); }

// Parent directory of `path` ("" if no slash — then use ".").
std::string ParentDir(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

}  // namespace

Status SyncDir(const std::string& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return Status::IOError("cannot open directory " + dir + ": " + ErrnoText());
  }
  const int rc = ::fsync(fd);
  const int saved_errno = errno;
  ::close(fd);
  if (rc != 0) {
    return Status::IOError("cannot fsync directory " + dir + ": " +
                           std::string(std::strerror(saved_errno)));
  }
  return Status::Ok();
}

Status WriteFileDurable(const std::string& path, std::string_view data) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return Status::IOError("cannot create " + path + ": " + ErrnoText());
  }
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int saved = errno;
      ::close(fd);
      return Status::IOError("write to " + path + " failed: " +
                             std::string(std::strerror(saved)));
    }
    written += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) {
    const int saved = errno;
    ::close(fd);
    return Status::IOError("fsync " + path + " failed: " +
                           std::string(std::strerror(saved)));
  }
  ::close(fd);
  return SyncDir(ParentDir(path));
}

Status ReadFileToString(const std::string& path, std::string* out) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::IOError("cannot open " + path + ": " + ErrnoText());
  }
  out->clear();
  char buf[65536];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int saved = errno;
      ::close(fd);
      return Status::IOError("read " + path + " failed: " +
                             std::string(std::strerror(saved)));
    }
    if (n == 0) {
      break;
    }
    out->append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return Status::Ok();
}

}  // namespace bedrockkv::fs
