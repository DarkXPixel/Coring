#include "coring5/http/Handlers.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace Coring5 {

namespace {

constexpr bool is_tchar(unsigned char c) noexcept {
  if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z')) {
    return true;
  }
  return c != 0 && std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           auto lo = [](char c) {
             return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
           };
           return lo(x) == lo(y);
         });
}

Error err(Errc c) noexcept { return Error{c, 0}; }

} // namespace

bool valid_static_header_name(std::string_view n) noexcept {
  return !n.empty() && n.size() <= 256 &&
         std::all_of(n.begin(), n.end(), [](char c) {
           return is_tchar(static_cast<unsigned char>(c));
         });
}

bool valid_static_header_value(std::string_view v) noexcept {
  return v.size() <= kMaxStaticHeaderBytes &&
         std::all_of(v.begin(), v.end(), [](char ch) {
           const auto c = static_cast<unsigned char>(ch);
           return c == '\t' || (c >= 0x20 && c != 0x7f);
         });
}

bool forbidden_static_header(std::string_view n) noexcept {
  static constexpr std::array<std::string_view, 11> kNames{
      "content-length",
      "transfer-encoding",
      "connection",
      "keep-alive",
      "proxy-connection",
      "te",
      "trailer",
      "upgrade",
      "proxy-authenticate",
      "proxy-authorization",
      "expect"};
  return std::any_of(kNames.begin(), kNames.end(),
                     [&](std::string_view k) { return iequals(n, k); });
}

Result<Respond> Respond::make(uint16_t status,
                              std::vector<StaticHeader> headers,
                              std::string body) {
  if (status < 200 || status > 599) {
    return std::unexpected(err(Errc::InvalidArgument));
  }
  if (!body.empty() && (status == 204 || status == 304)) {
    return std::unexpected(err(Errc::InvalidArgument));
  }
  if (body.size() > kMaxStaticBodyBytes || headers.size() > kMaxStaticHeaders) {
    return std::unexpected(err(Errc::TooLarge));
  }
  for (const StaticHeader &h : headers) {
    if (!valid_static_header_name(h.name) ||
        !valid_static_header_value(h.value) ||
        forbidden_static_header(h.name)) {
      return std::unexpected(err(Errc::InvalidArgument));
    }
  }
  Respond r;
  r.status_ = status;
  r.headers_ = std::move(headers);
  r.body_ = std::move(body);
  return r;
}

Result<ApplyStatus> Respond::apply(Exchange &ex,
                                   ApplyCursor &cur) const noexcept {
  if (!cur.head_done) {
    if (ex.phase() == Phase::Routing && !ex.attach_responder(SideRef{})) {
      return std::unexpected(err(Errc::Canceled));
    }
    ResponseMeta m;
    m.status = status_;
    m.no_content = content_forbidden(status_, ex.request_meta().head_only);
    m.declared_known = !m.no_content;
    m.declared = m.no_content ? 0 : body_.size();
    if (ex.publish_final(m) != HeadStatus::Ok) {
      return std::unexpected(err(Errc::Canceled));
    }
    cur.head_done = true;
    if (m.no_content) {
      return ApplyStatus::Done; // publish_final finished the stream
    }
  }
  while (cur.sent < body_.size()) {
    const auto want = static_cast<uint32_t>(
        std::min<std::size_t>(body_.size() - cur.sent, UINT32_MAX));
    const ClaimResult c = ex.claim(StreamId::Res, want);
    if (c.status == ClaimStatus::Blocked) {
      return ApplyStatus::Blocked;
    }
    if (c.status != ClaimStatus::Granted) {
      return std::unexpected(err(Errc::Canceled));
    }
    const auto *p =
        reinterpret_cast<const std::byte *>(body_.data()) + cur.sent;
    if (ex.push(StreamId::Res, std::span<const std::byte>(p, c.granted)) !=
        WriteStatus::Ok) {
      return std::unexpected(err(Errc::Canceled));
    }
    cur.sent += c.granted;
  }
  if (ex.finish(StreamId::Res) != WriteStatus::Ok) {
    return std::unexpected(err(Errc::Canceled));
  }
  return ApplyStatus::Done;
}

bool Redirect::valid_location(std::string_view loc) noexcept {
  return !loc.empty() && loc.size() <= kMaxLocationBytes &&
         std::all_of(loc.begin(), loc.end(), [](char ch) {
           const auto c = static_cast<unsigned char>(ch);
           return c > 0x20 && c < 0x7f;
         });
}

Result<Redirect> Redirect::make(uint16_t status, std::string_view location) {
  if (!valid_status(status) || !valid_location(location)) {
    return std::unexpected(err(Errc::InvalidArgument));
  }
  std::vector<StaticHeader> h;
  h.push_back(StaticHeader{"Location", std::string(location)});
  auto r = Respond::make(status, std::move(h), {});
  if (!r) {
    return std::unexpected(r.error());
  }
  return Redirect(std::move(*r));
}

} // namespace Coring5
