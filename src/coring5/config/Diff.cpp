#include "coring5/config/Diff.hpp"

namespace Coring5 {

ConfigDiff diff_snapshots(const ConfigSnapshot &old_s,
                          const ConfigSnapshot &new_s) {
  ConfigDiff d;
  d.limits_changed = !(old_s.limits == new_s.limits);
  d.upstreams_changed = !(old_s.upstreams == new_s.upstreams);
  d.routes_changed = old_s.routes != new_s.routes;
  for (const auto &n : new_s.listeners) {
    const auto *old = old_s.find(n.id);
    if (old && old->route != n.route) {
      d.routes_changed = true;
    }
    (old != nullptr ? d.kept : d.added).push_back(n.id);
  }
  for (const auto &o : old_s.listeners) {
    if (new_s.find(o.id) == nullptr) {
      d.removed.push_back(o.id);
    }
  }
  return d;
}

} // namespace Coring5
