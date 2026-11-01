# Architecture decision records

Short records of *why* a decision was made, so it is not re-litigated without
new information. Format: context → decision → consequences. Status is
`accepted` unless noted; supersede rather than edit.

| ADR | Decision |
|---|---|
| [0001](0001-thread-per-core-share-nothing.md) | Thread-per-core, share-nothing workers; control thread does all fallible work |
| [0002](0002-generation-handles-and-optag.md) | 32-bit generation handles as a tripwire; `OpTag` layout |
| [0003](0003-inflight-accounting-and-close.md) | In-flight counting at prep time; close only at `inflight == 0` |
| [0004](0004-immutable-snapshots-and-reload.md) | Immutable config snapshots, infallible worker apply, retire on control thread |
| [0005](0005-sans-io-protocols.md) | Sans-IO protocol state machines; use nghttp2/ngtcp2/OpenSSL |
| [0006](0006-provided-buffers-and-leases.md) | One slab per worker, kernel-selected buffers, RAII leases |
| [0007](0007-layering-and-build.md) | One library per module, downward-only links, checked includes |
| [0008](0008-min-kernel-6-1.md) | Kernel floor 6.1; probe the rest |
