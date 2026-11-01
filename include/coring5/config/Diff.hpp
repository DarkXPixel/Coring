#pragma once

#include "coring5/config/Snapshot.hpp"
#include <vector>

namespace Coring5 {

// Pure function over two snapshots (identity = ListenerId). Listeners whose
// name or address changed get a NEW id, so they appear as removed + added.
struct ConfigDiff {
  std::vector<ListenerId> added;
  std::vector<ListenerId> removed;
  std::vector<ListenerId> kept;
  bool limits_changed{false};

  [[nodiscard]] bool empty() const noexcept {
    return added.empty() && removed.empty() && !limits_changed;
  }
};

[[nodiscard]] ConfigDiff diff_snapshots(const ConfigSnapshot &old_s,
                                        const ConfigSnapshot &new_s);

} // namespace Coring5
