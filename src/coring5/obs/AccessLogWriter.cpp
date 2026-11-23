#include "coring5/obs/AccessLogWriter.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <new>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace Coring5 {
namespace {

constexpr std::size_t kBatch = 32;
// Six integers (at most 20 decimal digits and a tab each), two escaped
// strings, a tab and a newline.
constexpr std::size_t kLine =
    6 * 21 + 3 * (AccessRecord{}.method.size() + AccessRecord{}.target.size()) +
    2;

char *append_field(char *out, std::span<const char> field,
                   uint8_t len) noexcept {
  constexpr char hex[] = "0123456789ABCDEF";
  for (char c : field.first(std::min<std::size_t>(len, field.size()))) {
    auto byte = static_cast<unsigned char>(c);
    if (byte < 32 || byte > 126 || c == '%') {
      *out++ = '%';
      *out++ = hex[byte >> 4];
      *out++ = hex[byte & 15];
    } else {
      *out++ = c;
    }
  }
  return out;
}

char *append_record(char *out, const AccessRecord &r) noexcept {
  for (uint64_t value :
       std::array<uint64_t, 6>{r.unix_ms, r.duration_us, r.listener, r.bytes_in,
                               r.bytes_out, r.status}) {
    out = std::to_chars(out, out + 20, value).ptr;
    *out++ = '\t';
  }
  out = append_field(out, r.method, r.method_len);
  *out++ = '\t';
  out = append_field(out, r.target, r.target_len);
  *out++ = '\n';
  return out;
}

int write_batch(int fd, std::span<const char> bytes) noexcept {
  while (!bytes.empty()) {
    const auto n = ::write(fd, bytes.data(), bytes.size());
    if (n > 0) {
      bytes = bytes.subspan(static_cast<std::size_t>(n));
    } else if (n == 0 || errno != EINTR) {
      return n == 0 ? EIO : errno;
    }
  }
  return 0;
}

constexpr int kOpenFlags =
    O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NONBLOCK;

int rotate(int &fd, const std::string &path, unsigned files) noexcept {
  if (::close(std::exchange(fd, -1)) < 0) {
    return errno;
  }

  std::array<char, PATH_MAX> first;
  std::array<char, PATH_MAX> second;
  auto archive = [&](char *name, unsigned index) {
    std::memcpy(name, path.data(), path.size());
    name[path.size()] = '.';
    *std::to_chars(name + path.size() + 1, name + PATH_MAX - 1, index).ptr =
        '\0';
  };
  char *older = first.data();
  char *newer = second.data();
  archive(older, files);
  if (::unlink(older) < 0 && errno != ENOENT) {
    return errno;
  }
  for (unsigned i = files; i > 1; --i) {
    archive(newer, i - 1);
    if (::rename(newer, older) < 0 && errno != ENOENT) {
      return errno;
    }
    std::swap(older, newer);
  }
  if (::rename(path.c_str(), older) < 0) {
    return errno;
  }
  fd = ::open(path.c_str(), kOpenFlags | O_EXCL, 0640);
  return fd < 0 ? errno : 0;
}

} // namespace

AccessLogWriter::~AccessLogWriter() { (void)stop(); }

Result<> AccessLogWriter::start(const char *path,
                               std::span<AccessLogRing> rings,
                               AccessLogRotation rotation) {
  if (thread_.joinable() || !path || !*path || rings.empty() ||
      (rotation.bytes == 0) != (rotation.files == 0)) {
    return unexpected_error(Errc::InvalidArgument);
  }
  if (rotation.files != 0) {
    const std::string_view name(path);
    const auto basename = name.substr(name.find_last_of('/') + 1);
    const std::size_t suffix =
        rotation.files >= 100 ? 4 : (rotation.files >= 10 ? 3 : 2);
    if (basename.empty() || basename.size() + suffix > NAME_MAX ||
        name.size() + suffix >= PATH_MAX) {
      return unexpected_error(Errc::InvalidArgument, ENAMETOOLONG);
    }
  }
  const int fd =
      ::open(path, kOpenFlags | (rotation.files ? O_NOFOLLOW : 0), 0640);
  if (fd < 0) {
    return unexpected_error(Errc::Internal, errno);
  }
  struct stat st{};
  if (::fstat(fd, &st) < 0) {
    const int error = errno;
    ::close(fd);
    return unexpected_error(Errc::Internal, error);
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    return unexpected_error(Errc::InvalidArgument, EINVAL);
  }
  stopping_.store(false);
  error_.store(0, std::memory_order_relaxed);
  try {
    if (rotation.files != 0) {
      path_ = path;
    }
    thread_ = std::thread(&AccessLogWriter::run, this, fd,
                          static_cast<uint64_t>(st.st_size), rings, rotation);
  } catch (const std::bad_alloc &) {
    ::close(fd);
    return unexpected_error(Errc::Internal, ENOMEM);
  } catch (const std::system_error &e) {
    ::close(fd);
    return unexpected_error(Errc::Internal, e.code().value());
  }
  return {};
}

void AccessLogWriter::run(int fd, uint64_t size, std::span<AccessLogRing> rings,
                          AccessLogRotation rotation) noexcept {
  std::array<char, kBatch * kLine> buffer;
  int error = 0;
  for (;;) {
    // Observe shutdown BEFORE scanning: an empty pass started before the last
    // enqueue cannot certify that all producers have finished.
    const bool final_drain = stopping_.load();
    bool had_records = false;
    for (auto &ring : rings) {
      char *end = buffer.data();
      for (std::size_t i = 0; i < kBatch; ++i) {
        auto record = ring.pop();
        if (!record) {
          break;
        }
        char *next = append_record(end, *record);
        if (rotation.files != 0) {
          // Size includes the records buffered for this file so far.
          const auto line_size = static_cast<std::size_t>(next - end);
          if (size != 0 &&
              line_size > rotation.bytes - std::min(size, rotation.bytes)) {
            error = write_batch(fd, {buffer.data(), end});
            if (error == 0) {
              error = rotate(fd, path_, rotation.files);
            }
            if (error != 0) {
              break;
            }
            std::memmove(buffer.data(), end, line_size);
            next = buffer.data() + line_size;
            size = 0;
          }
          size += line_size;
        }
        end = next;
      }
      if (error != 0) {
        break;
      }
      if (end != buffer.data()) {
        had_records = true;
        error = write_batch(fd, {buffer.data(), end});
        if (error != 0) {
          break;
        }
      }
    }
    if (error != 0 || (final_drain && !had_records)) {
      break;
    }
    if (!had_records) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  if (fd >= 0 && ::close(fd) < 0 && error == 0) {
    error = errno;
  }
  error_.store(error, std::memory_order_relaxed);
}

Result<> AccessLogWriter::stop() {
  if (thread_.joinable()) {
    stopping_.store(true);
    thread_.join();
  }
  if (const int err = error(); err != 0) {
    return unexpected_error(Errc::Internal, err);
  }
  return {};
}

} // namespace Coring5
