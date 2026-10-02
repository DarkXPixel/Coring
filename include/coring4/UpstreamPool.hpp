#pragma once

#include "coring4/SessionStorage.hpp"
#include "coring4/UpstreamConnection.hpp"
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
namespace Coring4 {

// class UpstreamMap {
//   struct UpstreamPool {
//     std::vector<SessionHandle> handles;
//   };

//   struct StringHash {
//     using is_transparent = void;
//     std::size_t operator()(std::string_view sv) const noexcept {
//       return std::hash<std::string_view>{}(sv);
//     }
//   };

// public:
//   UpstreamMap(
//       SessionStorage<UpstreamConnectionVariant> &storage_upstreams) noexcept
//       : storage_upstreams_(storage_upstreams) {}

//   SessionHandle get(std::string_view addr) {
//     auto it = pool_.find(addr);

//     if (it != pool_.end()) {
//       auto &val = it->second;
//       if (val.handles.empty()) {
//         auto [handle, _] = storage_upstreams_.emplace<UpstreamConnection>();
//         return handle;
//       }
//       auto handle = val.handles.back();
//       val.handles.pop_back();
//       return handle;
//     }
//   }

//   std::unordered_map<std::string, UpstreamPool, StringHash, std::equal_to<>>
//       pool_;

// private:
//   SessionStorage<UpstreamConnectionVariant> &storage_upstreams_;
// };
} // namespace Coring4