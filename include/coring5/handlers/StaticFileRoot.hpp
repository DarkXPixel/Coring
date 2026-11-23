#pragma once

#include "coring5/config/Routes.hpp"
#include "coring5/http/Handlers.hpp"
#include "coring5/io/FdOwner.hpp"
#include <array>
#include <climits>
#include <linux/openat2.h>
#include <map>
#include <sys/stat.h>

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
  static Result<> decode_target(std::string_view target,
                               std::span<char> path) noexcept;
  static open_how open_options() noexcept;
  static constexpr unsigned stat_fields =
      STATX_TYPE | STATX_SIZE | STATX_INO | STATX_MTIME | STATX_CTIME;
  static Result<StaticFileMetadata> metadata(const struct statx &st) noexcept;

  // Opens and pins the configured directory.
  [[nodiscard]] static Result<StaticFileRoot>
  open(std::string_view directory) noexcept;

  // Accepts an origin-form request target and selects a regular file under
  // the pinned root. The optional policy supplies index and try_files.
  [[nodiscard]] Result<OpenedStaticFile>
  resolve(std::string_view target,
          const StaticFileOptions *options = nullptr) const noexcept;

private:
  explicit StaticFileRoot(FdOwner directory) noexcept
      : directory_(std::move(directory)) {}

  FdOwner directory_;
};

// Shared lookup policy for synchronous and io_uring callers. The decoded URI
// and validated options must outlive the lookup. Each emitted path is relative
// to the pinned root, including when a directory needs an index.
class StaticFileLookup {
public:
  StaticFileLookup(std::string_view uri,
                  const StaticFileOptions *options) noexcept
      : uri_(uri),
        index_(options ? std::string_view(options->index)
                       : StaticFileOptions::default_index),
        candidates_(options ? std::span(options->try_files)
                            : std::span<const std::string>{}) {}

  // The returned path is stable until the next probe.
  [[nodiscard]] Result<const char *> next() noexcept;
  [[nodiscard]] Result<std::optional<StaticFileMetadata>>
  inspect(const struct statx &st) noexcept;
  [[nodiscard]] static bool is_miss(int error) noexcept;

private:
  [[nodiscard]] Result<const char *> path(std::string_view base,
                                         bool append_index) noexcept;

  std::array<char, PATH_MAX> path_;
  std::string_view uri_;
  std::string_view index_;
  std::span<const std::string> candidates_;
  std::size_t step_{};
  bool needs_index_{};
};

struct StaticRoots {
  std::map<std::string, StaticFileRoot, std::less<>> directories;
};

} // namespace Coring5
