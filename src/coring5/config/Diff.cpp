#include "coring5/config/Diff.hpp"

namespace Coring5 {

ConfigDiff diff_snapshots(const ConfigSnapshot &old_s,
                          const ConfigSnapshot &new_s) {
  ConfigDiff d;
  d.limits_changed = !(old_s.limits == new_s.limits);
  d.upstreams_changed = !(old_s.upstreams == new_s.upstreams);
  for (const auto &n : new_s.listeners) {
    (old_s.find(n.id) != nullptr ? d.kept : d.added).push_back(n.id);
  }
  for (const auto &o : old_s.listeners) {
    if (new_s.find(o.id) == nullptr) {
      d.removed.push_back(o.id);
    }
  }
  return d;
}

} // namespace Coring5
