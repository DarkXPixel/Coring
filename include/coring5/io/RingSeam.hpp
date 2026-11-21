#pragma once

// Test seam for the io layer. Connection types and IoContext name the ring only
// through these aliases:
//
//   production         IoRing = Ring,      IoBufferRing = BufferRing
//   CORING5_FAKE_RING  IoRing = FakeRing,  IoBufferRing = FakeBufferRing
//
// The fake lives in io/FakeRing.hpp and mirrors the prep_*/Lease API with
// scripted completions, so a lifecycle test can close a connection at every
// state without a kernel (docs/TESTING.md). `io` includes nothing from `http`.

#include "coring5/io/Cqe.hpp"

#ifdef CORING5_FAKE_RING
#include "coring5/io/FakeRing.hpp"
namespace Coring5 {
using IoRing = FakeRing;
using IoBufferRing = FakeBufferRing;
} // namespace Coring5
#else
#include "coring5/io/BufferRing.hpp"
#include "coring5/io/Ring.hpp"
namespace Coring5 {
using IoRing = Ring;
using IoBufferRing = BufferRing;
} // namespace Coring5
#endif
