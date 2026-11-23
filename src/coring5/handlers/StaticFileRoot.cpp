#include "coring5/handlers/StaticFileRoot.hpp"

#include <fcntl.h>
#include <limits.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <algorithm>
#include <array>
#include <cerrno>

namespace Coring5 {

namespace {

int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool safe_segment(std::string_view segment) noexcept {
    return !segment.empty() && segment != "." && segment != "..";
}

std::unexpected<Error> lookup_error(int error) noexcept {
    switch (error) {
        case ELOOP:
        case EXDEV:
            return unexpected_error(Errc::ProtocolViolation, error);
        case ENAMETOOLONG:
            return unexpected_error(Errc::TooLarge, error);
        case ENOENT:
        case ENOTDIR:
        case EISDIR:
        case EACCES:
            return unexpected_error(Errc::InvalidArgument, error);
        case EMFILE:
        case ENFILE:
            return unexpected_error(Errc::Overloaded, error);
        case ENOSYS:
            return unexpected_error(Errc::Unsupported, error);
        default:
            return unexpected_error(Errc::Internal, error);
    }
}

}  // namespace

Result<> StaticFileRoot::decode_target(std::string_view target, std::span<char> path) noexcept {
    path = path.first(std::min<std::size_t>(path.size(), PATH_MAX));
    if (path.empty()) return unexpected_error(Errc::TooLarge);
    if (!target.starts_with('/') || target.find('#') != std::string_view::npos) {
        return unexpected_error(Errc::ProtocolViolation);
    }
    target.remove_prefix(1);
    auto src = target.substr(0, target.find('?'));
    char* out = path.data();
    const char* segment = out;
    while (!src.empty()) {
        auto c = static_cast<unsigned char>(src.front());
        src.remove_prefix(1);
        if (c == '%') {
            if (src.size() < 2) {
                return unexpected_error(Errc::ProtocolViolation);
            }
            const int hi = hex_digit(src[0]);
            const int lo = hex_digit(src[1]);
            if (hi < 0 || lo < 0) {
                return unexpected_error(Errc::ProtocolViolation);
            }
            c = static_cast<unsigned char>(hi * 16 + lo);
            src.remove_prefix(2);
            if (c == '/' || c == '%') {
                return unexpected_error(Errc::ProtocolViolation);
            }
        }
        if (c < 0x20 || c == 0x7f || c == '\\') {
            return unexpected_error(Errc::ProtocolViolation);
        }
        if (out == path.data() + path.size() - 1) {
            return unexpected_error(Errc::TooLarge, ENAMETOOLONG);
        }
        if (c == '/') {
            if (!safe_segment({segment, out})) {
                return unexpected_error(Errc::ProtocolViolation);
            }
            segment = out + 1;
        }
        *out++ = static_cast<char>(c);
    }
    // A terminal slash denotes a directory. Empty interior segments were
    // already rejected above, and "/" decodes to the root-relative empty path.
    if (segment != out && !safe_segment({segment, out})) {
        return unexpected_error(Errc::ProtocolViolation);
    }
    *out = '\0';
    return {};
}

Result<StaticFileRoot> StaticFileRoot::open(std::string_view directory) noexcept {
    if (directory.empty() || directory.find('\0') != std::string_view::npos) {
        return unexpected_error(Errc::InvalidArgument);
    }
    std::array<char, PATH_MAX> path;
    if (directory.size() >= path.size()) {
        return unexpected_error(Errc::TooLarge, ENAMETOOLONG);
    }
    *std::ranges::copy(directory, path.data()).out = '\0';
    FdOwner fd(::open(path.data(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!fd) {
        return lookup_error(errno);
    }
    return StaticFileRoot(std::move(fd));
}

Result<OpenedStaticFile> StaticFileRoot::resolve(
    std::string_view target, const StaticFileOptions* options) const noexcept {
    if (!directory_) {
        return unexpected_error(Errc::InvalidArgument, EBADF);
    }
    if (options && !options->validation_error().empty()) {
        return unexpected_error(Errc::InvalidArgument, EINVAL);
    }
    std::array<char, PATH_MAX> uri;
    if (const auto decoded = decode_target(target, uri); !decoded) {
        return std::unexpected(decoded.error());
    }
    StaticFileLookup lookup(uri.data(), options);
    const auto how = open_options();
    for (;;) {
        const auto path = lookup.next();
        if (!path) {
            return std::unexpected(path.error());
        }
        // NONBLOCK keeps an unexpected FIFO from stalling lookup before statx.
        FdOwner fd(static_cast<int>(
            ::syscall(SYS_openat2, directory_.get(), *path, &how, sizeof(how))));
        if (!fd) {
            const int error = errno;
            if (StaticFileLookup::is_miss(error)) {
                continue;
            }
            return lookup_error(error);
        }
        struct statx st{};
        if (::statx(fd.get(), "", AT_EMPTY_PATH, stat_fields, &st) < 0) {
            return lookup_error(errno);
        }
        const auto info = lookup.inspect(st);
        if (!info) {
            return std::unexpected(info.error());
        }
        if (*info) {
            return OpenedStaticFile{std::move(fd), **info};
        }
    }
}

open_how StaticFileRoot::open_options() noexcept {
    return open_how{
        .flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY,
        .mode = 0,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV,
    };
}

Result<StaticFileMetadata> StaticFileRoot::metadata(const struct statx& st) noexcept {
    if ((st.stx_mask & stat_fields) != stat_fields || !S_ISREG(st.stx_mode)) {
        return unexpected_error(Errc::InvalidArgument);
    }
    return StaticFileMetadata{
            .size = st.stx_size,
            .inode = st.stx_ino,
            .device_major = st.stx_dev_major,
            .device_minor = st.stx_dev_minor,
            .modified = {st.stx_mtime.tv_sec, st.stx_mtime.tv_nsec},
            .changed = {st.stx_ctime.tv_sec, st.stx_ctime.tv_nsec},
    };
}

bool StaticFileLookup::is_miss(int error) noexcept {
    return error == ENOENT || error == ENOTDIR;
}

Result<const char*> StaticFileLookup::next() noexcept {
    if (candidates_.empty()) {
        if (step_ == 0) {
            ++step_;
            return path(uri_.empty() ? "." : uri_, false);
        }
        if (needs_index_ && step_ == 1) {
            ++step_;
            return path(uri_, true);
        }
        return lookup_error(needs_index_ ? EISDIR : ENOENT);
    }

    while (step_ < std::min(candidates_.size(), StaticFileOptions::max_candidates)) {
        const std::string_view candidate = candidates_[step_++];
        if (candidate == "$uri") {
            if (uri_.empty() || uri_.ends_with('/')) {
                continue;
            }
            return path(uri_, false);
        }
        if (candidate == "$uri/") {
            return path(uri_, true);
        }
        return path(candidate.substr(1), candidate.ends_with('/'));
    }
    return lookup_error(ENOENT);
}

Result<const char*> StaticFileLookup::path(std::string_view base, bool append_index) noexcept {
    const bool separator = append_index && !base.empty() && !base.ends_with('/');
    const auto length = base.size() + (append_index ? index_.size() : 0) + separator;
    if (length >= path_.size()) {
        return lookup_error(ENAMETOOLONG);
    }
    auto out = std::ranges::copy(base, path_.data()).out;
    if (separator) {
        *out++ = '/';
    }
    if (append_index) {
        out = std::ranges::copy(index_, out).out;
    }
    *out = '\0';
    return path_.data();
}

Result<std::optional<StaticFileMetadata>> StaticFileLookup::inspect(
    const struct statx& st) noexcept {
    if ((st.stx_mask & STATX_TYPE) != 0 && !S_ISREG(st.stx_mode)) {
        // An index is a file probe. A directory named like the index cannot
        // trigger another index lookup.
        needs_index_ |= candidates_.empty() && step_ == 1 && S_ISDIR(st.stx_mode);
        return std::nullopt;
    }
    return StaticFileRoot::metadata(st).transform([](const auto& info) {
        return std::optional{info};
    });
}

}  // namespace Coring5
