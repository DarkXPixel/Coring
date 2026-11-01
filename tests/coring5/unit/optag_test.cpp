#include "coring5/io/OpTag.hpp"
#include <gtest/gtest.h>

using namespace Coring5;

TEST(OpTag, RoundTripsAllFields) {
  for (uint32_t gen : {1U, 2U, 0x7FFFFFFFU, 0xFFFFFFFFU}) {
    for (uint32_t idx : {0U, 1U, 12345U, kMaxHandleIndex}) {
      for (auto kind :
           {OpKind::Accept, OpKind::Recv, OpKind::Cancel, OpKind::RecvMsg}) {
        for (uint8_t aux : {0, 1, 63}) {
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

TEST(OpTag, FieldsDoNotBleedIntoEachOther) {
  const auto t = OpTag::make(OpKind::Send, 0xFFFFF, 0, 0);
  EXPECT_EQ(t.gen(), 0U);
  EXPECT_EQ(t.aux(), 0);
  EXPECT_EQ(t.kind(), OpKind::Send);
  const auto u = OpTag::make(OpKind::Nop, 0, 0xFFFFFFFF, 63);
  EXPECT_EQ(u.index(), 0U);
  EXPECT_EQ(u.kind(), OpKind::Nop);
}

TEST(OpTag, HandleConversion) {
  const Handle h{42, 7};
  const auto t = OpTag::make(OpKind::Recv, h);
  EXPECT_EQ(t.handle(), h);
}

TEST(OpTag, DefaultTagIsNopWithInvalidGeneration) {
  const OpTag t{};
  EXPECT_EQ(t.kind(), OpKind::Nop);
  EXPECT_FALSE(t.handle().valid());
}
