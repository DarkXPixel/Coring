#pragma once

// TLS adapter (M5). OpenSSL with MEMORY BIOs: the library never touches the
// socket; ciphertext in/out goes through our own io_uring recv/send.
//
// STATUS: interface only.
//
//   rx ciphertext --feed_ciphertext()--> [SSL] --read_plaintext()--> app
//   app --write_plaintext()--> [SSL] --take_ciphertext()--> tx (send SQE)
//
// Notes for the implementation:
//  * SNI/ALPN selection reads the immutable ConfigSnapshot's cert catalogue
//    (ids, not pointers: invariant I8). SSL_CTX objects are built on the
//    control thread and shared read-only (SSL_CTX is thread-safe for
//    SSL_new()).
//  * The handshake is CPU-heavy; watch the loop-stall histogram. A helper
//    thread is a deliberate, measured exception to "no cross-thread" (see
//    docs/ARCHITECTURE.md).
//  * Session tickets: shared rotating key set (control thread rotates).
//  * Optional kTLS (SSL_OP_ENABLE_KTLS) enables splice/sendfile on TLS.

#include "coring5/base/Result.hpp"
#include <cstddef>
#include <span>
#include <string_view>

namespace Coring5 {

enum class TlsState : uint8_t { Handshaking, Established, Closed, Failed };

// TODO(M5): class TlsSession {
//   TlsState feed_ciphertext(std::span<const std::byte>);
//   Result<std::size_t> read_plaintext(std::span<std::byte> out);
//   Result<std::size_t> write_plaintext(std::span<const std::byte> in);
//   std::span<const std::byte> pending_ciphertext();
//   void consume_ciphertext(std::size_t n);
//   std::string_view alpn() const;
//   std::string_view sni() const;
// };

} // namespace Coring5
