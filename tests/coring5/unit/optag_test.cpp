#include "coring5/io/OpTag.hpp"
#include <gtest/gtest.h>
#include <set>

using namespace Coring5;

namespace {

// Every OpKind value the layout must carry.
constexpr unsigned kKinds = static_cast<unsigned>(OpKind::Count_);

} // namespace

TEST(OpTag, RoundTripsAllFields) {
  for (uint32_t gen : {1U, 2U, 0x7FFFFFFFU, 0xFFFFFFFFU}) {
    for (uint32_t idx : {0U, 1U, 12345U, kMaxHandleIndex}) {
      for (auto kind :
           {OpKind::Accept, OpKind::Recv, OpKind::Cancel, OpKind::RecvMsg}) {
        for (uint8_t aux : {0, 1, 7}) { // aux is 3 bits
          const auto t = OpTag::make(kind, idx, gen, aux);
          EXPECT_EQ(t.gen(), gen);
          EXPECT_EQ(t.index(), idx);
          EXPECT_EQ(t.kind(), kind);
          EXPECT_EQ(t.aux(), aux);
        }
      }
    }
  }
}

TEST(OpTag, EveryPoolAndKindRoundTripIndependently) {
  // Exhaustive over the two fields this layout changed: no pool value may
  // alter the kind or aux, and no kind may alter the pool or aux.
  for (unsigned p = 0; p < (1U << kOpPoolBits); ++p) {
    for (unsigned k = 0; k < kKinds; ++k) {
      const auto t =
          OpTag::make(static_cast<OpKind>(k), Handle{0xFFFFF, 0xFFFFFFFFU},
                      kAuxMax, static_cast<PoolId>(p));
      EXPECT_EQ(static_cast<unsigned>(t.pool()), p) << "kind " << k;
      EXPECT_EQ(static_cast<unsigned>(t.kind()), k) << "pool " << p;
      EXPECT_EQ(t.aux(), kAuxMax);
      EXPECT_EQ(t.index(), 0xFFFFFU);
      EXPECT_EQ(t.gen(), 0xFFFFFFFFU);
    }
  }
}

TEST(OpTag, PoolFieldIsFourBitsWide) {
  // Pool ids 8..15 did not exist before M4.0; they must survive intact and
  // must not leak into the neighbouring fields.
  for (unsigned p : {7U, 8U, 9U, 15U}) {
    const auto t = OpTag::make(OpKind::Nop, 0, 0, 0, static_cast<PoolId>(p));
    EXPECT_EQ(static_cast<unsigned>(t.pool()), p);
    EXPECT_EQ(t.kind(), OpKind::Nop);
    EXPECT_EQ(t.aux(), 0);
    EXPECT_EQ(t.index(), 0U);
    EXPECT_EQ(t.gen(), 0U);
  }
}

TEST(OpTag, KindFieldFitsEveryDefinedKindAndNothingMore) {
  EXPECT_LE(kKinds, 1U << kOpKindBits);
  // The largest defined kind round-trips; the bits above kind are not touched.
  const auto last = static_cast<OpKind>(kKinds - 1);
  const auto t = OpTag::make(last, 0, 0, 0, static_cast<PoolId>(0));
  EXPECT_EQ(t.kind(), last);
  EXPECT_EQ(t.index(), 0U);
}

TEST(OpTag, FieldsDoNotBleedIntoEachOther) {
  const auto t = OpTag::make(OpKind::Send, 0xFFFFF, 0, 0);
  EXPECT_EQ(t.gen(), 0U);
  EXPECT_EQ(t.aux(), 0);
  EXPECT_EQ(t.kind(), OpKind::Send);
  const auto u = OpTag::make(OpKind::Nop, 0, 0xFFFFFFFF, kAuxMax);
  EXPECT_EQ(u.index(), 0U);
  EXPECT_EQ(u.kind(), OpKind::Nop);
  EXPECT_EQ(u.aux(), kAuxMax);
}

TEST(OpTag, BitLayoutIsExactlyAsDocumented) {
  // Pins the wire layout so a future edit cannot silently shift a field:
  //   gen:32 | index:20 | kind:5 | pool:4 | aux:3   (from the top)
  static_assert(
      kOpAuxBits + kOpPoolBits + kOpKindBits + kOpIndexBits + kOpGenBits == 64);
  const auto t =
      OpTag::make(OpKind::Recv, /*index=*/0x12345, /*gen=*/0xABCDEF01U,
                  /*aux=*/5, PoolId::L4);
  const uint64_t expect =
      (uint64_t{0xABCDEF01U} << 32) | (uint64_t{0x12345} << 12) |
      (uint64_t{static_cast<unsigned>(OpKind::Recv)} << 7) |
      (uint64_t{static_cast<unsigned>(PoolId::L4)} << 3) | uint64_t{5};
  EXPECT_EQ(t.raw, expect);
  // Field boundaries.
  EXPECT_EQ(kOpPoolShift, 3U);
  EXPECT_EQ(kOpKindShift, 7U);
  EXPECT_EQ(kOpIndexShift, 12U);
  EXPECT_EQ(kOpGenShift, 32U);
}

TEST(OpTag, DistinctInputsGiveDistinctTags) {
  std::set<uint64_t> seen;
  for (unsigned p = 0; p < 16; ++p) {
    for (unsigned k = 0; k < kKinds; ++k) {
      for (uint8_t aux : {0, 3, 7}) {
        const auto t = OpTag::make(static_cast<OpKind>(k), 17, 9, aux,
                                   static_cast<PoolId>(p));
        EXPECT_TRUE(seen.insert(t.raw).second)
            << "collision p=" << p << " k=" << k << " aux=" << int{aux};
      }
    }
  }
}

TEST(OpTag, HandleConversion) {
  const Handle h{42, 7};
  const auto t = OpTag::make(OpKind::Recv, h);
  EXPECT_EQ(t.handle(), h);
}

TEST(OpTag, ConnRefConversionKeepsThePool) {
  const ConnRef c{PoolId::Http2, Handle{99, 3}};
  const auto t = OpTag::make(OpKind::Send, c, kAuxPeer);
  EXPECT_EQ(t.conn().pool, PoolId::Http2);
  EXPECT_EQ(t.conn().handle, c.handle);
  EXPECT_EQ(t.aux(), kAuxPeer);
}

TEST(OpTag, DefaultTagIsNopWithInvalidGeneration) {
  const OpTag t{};
  EXPECT_EQ(t.kind(), OpKind::Nop);
  EXPECT_FALSE(t.handle().valid());
}
