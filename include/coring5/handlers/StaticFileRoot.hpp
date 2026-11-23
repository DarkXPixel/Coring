#pragma once

#include <linux/openat2.h>
#include <sys/stat.h>
#include <map>
#include "coring5/http/Handlers.hpp"
#include "coring5/io/FdOwner.hpp"

namespace Coring5 {

struct OpenedStaticFile {
    FdOwner fd;
    // Cached for this open only. A new lookup reopens and re-stats the file.
    StaticFileMetadata metadata;
};

// Linux filesystem adapter. These calls may block; callers must arrange lookup
// off the worker datapath. Body reads belong to the caller's I/O driver.
class StaticFileRoot {
public:
    [[nodiscard]] int fd() const noexcept { return directory_.get(); }
    static Result<> decode_target(std::string_view target, std::span<char> path) noexcept;
    static open_how open_options() noexcept;
    static constexpr unsigned stat_fields = STATX_TYPE | STATX_SIZE | STATX_INO | STATX_MTIME | STATX_CTIME;
    static Result<StaticFileMetadata> metadata(const struct statx& st) noexcept;

    // Opens and pins the configured directory.
    [[nodiscard]] static Result<StaticFileRoot> open(std::string_view directory) noexcept;

    // Accepts an origin-form request target. Strips the query and decodes the
    // path once. Empty and dot path segments, symlinks, mount crossings and
    // non-regular files are rejected. Errors preserve errno for status mapping.
    [[nodiscard]] Result<OpenedStaticFile> resolve(std::string_view target) const noexcept;

private:
    explicit StaticFileRoot(FdOwner directory) noexcept
        : directory_(std::move(directory)) {}

    FdOwner directory_;
};

struct StaticRoots {
    std::map<std::string, StaticFileRoot, std::less<>> directories;
};

}  // namespace Coring5
