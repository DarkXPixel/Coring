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

bool valid_header_name(std::string_view n) noexcept {
  return !n.empty() && std::all_of(n.begin(), n.end(), [](char c) {
    return is_tchar(static_cast<unsigned char>(c));
  });
}

bool valid_header_value(std::string_view v) noexcept {
  return std::all_of(v.begin(), v.end(), [](char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return c == '\t' || (c >= 0x20 && c != 0x7f);
  });
}

bool framing_or_hop_by_hop(std::string_view n) noexcept {
  static constexpr std::array<std::string_view, 10> kNames{
      "content-length",
      "transfer-encoding",
      "connection",
      "keep-alive",
      "proxy-connection",
      "te",
      "trailer",
      "upgrade",
      "proxy-authenticate",
      "proxy-authorization"};
  return std::any_of(kNames.begin(), kNames.end(),
                     [&](std::string_view k) { return iequals(n, k); });
}

std::string_view next_connection_option(std::string_view &list) noexcept {
  const auto comma = list.find(',');
  const auto option = list.substr(0, comma);
  list = comma == list.npos ? std::string_view{} : list.substr(comma + 1);
  const auto first = option.find_first_not_of(" \t");
  if (first == option.npos) {
    return {};
  }
  return option.substr(first, option.find_last_not_of(" \t") - first + 1);
}

bool nominated(std::span<const ProxyHeader> headers,
               std::string_view name) noexcept {
  for (const auto &header : headers) {
    if (iequals(header.name, "connection")) {
      auto list = header.value;
      while (!list.empty()) {
        if (iequals(next_connection_option(list), name)) {
          return true;
        }
      }
    }
  }
  return false;
}

Result<std::size_t> filter_headers(std::span<const ProxyHeader> input,
                                   std::span<ProxyHeader> output,
                                   bool request) noexcept {
  bool has_connection_options = false;
  for (const auto &header : input) {
    if (!valid_header_name(header.name) || !valid_header_value(header.value)) {
      return unexpected_error(Errc::ProtocolViolation);
    }
    if (iequals(header.name, "connection")) {
      auto list = header.value;
      while (!list.empty()) {
        const auto option = next_connection_option(list);
        if (!option.empty() && !valid_header_name(option)) {
          return unexpected_error(Errc::ProtocolViolation);
        }
        has_connection_options |= !option.empty();
      }
    }
  }
  std::size_t used = 0;
  constexpr std::string_view forwarded_prefix = "x-forwarded-";
  for (const auto &header : input) {
    const auto name = header.name;
    if (framing_or_hop_by_hop(name) ||
        iequals(name, "proxy-authentication-info") ||
        (request && (iequals(name, "forwarded") ||
                     iequals(name.substr(0, forwarded_prefix.size()),
                             forwarded_prefix))) ||
        (has_connection_options && nominated(input, name))) {
      continue;
    }
    if (used == output.size()) {
      return unexpected_error(Errc::TooLarge);
    }
    output[used++] = header;
  }
  return used;
}

} // namespace

bool valid_static_header_name(std::string_view n) noexcept {
  return n.size() <= 256 && valid_header_name(n);
}

bool valid_static_header_value(std::string_view v) noexcept {
  return v.size() <= kMaxStaticHeaderBytes && valid_header_value(v);
}

bool forbidden_static_header(std::string_view n) noexcept {
  return framing_or_hop_by_hop(n) || iequals(n, "expect");
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
  return apply(ex, cur, status_, body_);
}

Result<ApplyStatus>
Respond::apply(Exchange &ex, ApplyCursor &cur, uint16_t status,
               std::string_view body,
               std::span<const ResponseField> fields) noexcept {
  if (!cur.head_done) {
    if (ex.phase() == Phase::Routing && !ex.attach_responder(SideRef{})) {
      return std::unexpected(err(Errc::Canceled));
    }
    ResponseMeta m;
    m.status = status;
    m.no_content = content_forbidden(status, ex.request_meta().head_only);
    m.declared_known = !content_forbidden(status, false);
    m.declared = body.size();
    if (ex.publish_final(m, fields) != HeadStatus::Ok) {
      return std::unexpected(err(Errc::Canceled));
    }
    cur.head_done = true;
    if (m.no_content) {
      return ApplyStatus::Done; // publish_final finished the stream
    }
  }
  while (cur.sent < body.size()) {
    const auto want = static_cast<uint32_t>(
        std::min<uint64_t>(body.size() - cur.sent, UINT32_MAX));
    const ClaimResult c = ex.claim(StreamId::Res, want);
    if (c.status == ClaimStatus::Blocked) {
      return ApplyStatus::Blocked;
    }
    if (c.status != ClaimStatus::Granted) {
      return std::unexpected(err(Errc::Canceled));
    }
    const auto *p =
        reinterpret_cast<const std::byte *>(body.data()) + cur.sent;
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

Result<std::span<const ProxyHeader>>
ReverseProxy::request_headers(std::span<const ProxyHeader> input,
                              ProxyForwarding forwarding,
                              std::span<ProxyHeader> output) noexcept {
  if (forwarding.client_address.empty() ||
      forwarding.client_address.find_first_of(" ,\t") !=
          std::string_view::npos ||
      !valid_header_value(forwarding.client_address) ||
      !valid_header_value(forwarding.original_host)) {
    return unexpected_error(Errc::InvalidArgument);
  }
  const auto filtered = filter_headers(input, output, true);
  if (!filtered) {
    return std::unexpected(filtered.error());
  }
  auto used = *filtered;
  const bool has_host = !forwarding.original_host.empty();
  if (output.size() - used < (has_host ? 3u : 2u)) {
    return unexpected_error(Errc::TooLarge);
  }
  output[used++] = {"X-Forwarded-For", forwarding.client_address};
  if (has_host) {
    output[used++] = {"X-Forwarded-Host", forwarding.original_host};
  }
  output[used++] = {"X-Forwarded-Proto", forwarding.tls ? "https" : "http"};
  return output.first(used);
}

Result<std::span<const ProxyHeader>>
ReverseProxy::on_response_head(Exchange &ex, ApplyCursor &cur,
                               ResponseMeta meta,
                               std::span<const ProxyHeader> input,
                               std::span<ProxyHeader> output, bool tunnel) noexcept {
  if (cur.head_done || ex.phase() != Phase::Active ||
      ex.responder_state() != Exchange::ResponderState::Attached) {
    return unexpected_error(Errc::Canceled);
  }
  if (ex.stream(StreamId::Res).state() == StreamState::Aborted) {
    ex.detach(Side::Responder, Errc::Canceled);
    return unexpected_error(Errc::Canceled);
  }
  const auto filtered = filter_headers(input, output, false);
  const bool status_valid = tunnel ? meta.status == 101
                                   : meta.status >= 200 && meta.status <= 599;
  if (!filtered || !status_valid) {
    const Errc why = filtered ? Errc::ProtocolViolation : filtered.error().code;
    (void)on_failure(ex, why, ProxyRetry::Disallow);
    return unexpected_error(why);
  }
  meta.synthesized = false;
  const auto fields = output.first(*filtered);
  const auto head = tunnel ? ex.publish_tunnel(fields)
                           : ex.publish_final(meta, fields);
  if (head != HeadStatus::Ok) {
    return unexpected_error(Errc::Canceled);
  }
  cur.head_done = true;
  if (!tunnel && meta.declared_known && meta.declared == 0 &&
      ex.stream(StreamId::Res).state() == StreamState::Open) {
    (void)ex.finish(StreamId::Res);
  }
  request_staged_ = 0;
  if (!tunnel && !ex.stream(StreamId::Req).end_seen()) {
    (void)ex.cancel_request_body();
  }
  return fields;
}

} // namespace Coring5
