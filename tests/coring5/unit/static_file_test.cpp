#include <gtest/gtest.h>
#include <sys/stat.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "coring5/handlers/StaticFileRoot.hpp"
#include "coring5/http/ExchangePool.hpp"
#include "coring5/http/Handlers.hpp"

using namespace Coring5;

namespace {

using FileStatus = StaticFileStep::Status;
constexpr StaticFileMetadata kFile{.size = 10, .inode = 42};

class StaticFileRootTest : public ::testing::Test {
protected:
    void SetUp() override {
        char name[] = "/tmp/coring5-static-file-XXXXXX";
        ASSERT_NE(::mkdtemp(name), nullptr);
        temp_ = name;
        root_ = temp_ / "root";
        ASSERT_TRUE(std::filesystem::create_directories(root_ / "dir"));
        write(root_ / "dir" / "some file.txt", "inside");
        write(temp_ / "secret", "outside");
    }

    void TearDown() override {
        if (!temp_.empty()) {
            std::filesystem::remove_all(temp_);
        }
    }

    static void write(const std::filesystem::path& path, std::string_view data) {
        std::ofstream file(path);
        file << data;
        file.close();
        ASSERT_TRUE(file);
    }

    std::filesystem::path temp_;
    std::filesystem::path root_;
};

TEST_F(StaticFileRootTest, ResolvesEncodedPathAndIgnoresQuery) {
    const auto root = StaticFileRoot::open(root_.native());
    ASSERT_TRUE(root);
    const auto file = root->resolve("/dir/some%20file.txt?q=/../secret");
    ASSERT_TRUE(file);
    EXPECT_EQ(file->metadata.size, 6u);
    std::array<char, 6> data;
    ASSERT_EQ(::pread(file->fd.get(), data.data(), data.size(), 0), 6);
    EXPECT_EQ((std::string_view{data.data(), data.size()}), "inside");
}

TEST_F(StaticFileRootTest, RejectsPathsThatCouldEscapeTheRoot) {
    const auto root = StaticFileRoot::open(root_.native());
    ASSERT_TRUE(root);
    for (const std::string_view target : {"../secret",
                                          "/../secret",
                                          "/dir/../../secret",
                                          "/%2e%2E/secret",
                                          "/dir/.%2e/secret",
                                          "/dir/./some%20file.txt",
                                          "//secret",
                                          "/dir//some%20file.txt",
                                          "/..%2fsecret",
                                          "/dir%5c..%5csecret",
                                          "/dir\\..\\secret",
                                          "/%252e%252e/secret",
                                          "/secret%00",
                                          "/bad%",
                                          "/bad%GG",
                                          "/dir/some%20file.txt#fragment",
                                          "/"}) {
        SCOPED_TRACE(target);
        const auto file = root->resolve(target);
        ASSERT_FALSE(file);
        EXPECT_EQ(file.error().code, Errc::ProtocolViolation);
    }
    const auto with_nul = root->resolve(std::string_view{"/dir\0tail", 9});
    ASSERT_FALSE(with_nul);
    EXPECT_EQ(with_nul.error().code, Errc::ProtocolViolation);
}

TEST_F(StaticFileRootTest, RejectsSymlinksAndNonRegularFiles) {
    std::filesystem::create_symlink(temp_ / "secret", root_ / "outside");
    std::filesystem::create_directory_symlink(temp_, root_ / "outside_dir");
    std::filesystem::create_symlink("dir/some file.txt", root_ / "inside_link");
    ASSERT_EQ(::mkfifo((root_ / "pipe").c_str(), 0600), 0);
    const auto root = StaticFileRoot::open(root_.native());
    ASSERT_TRUE(root);
    for (const std::string_view target :
         {"/outside", "/outside_dir/secret", "/inside_link", "/pipe", "/dir"}) {
        SCOPED_TRACE(target);
        EXPECT_FALSE(root->resolve(target));
    }
    EXPECT_FALSE(StaticFileRoot::open((temp_ / "secret").native()));
    EXPECT_FALSE(root->resolve("/missing"));
}

TEST_F(StaticFileRootTest, KeepsRootPinnedAndRefreshesMetadataOnEachOpen) {
    const auto root = StaticFileRoot::open(root_.native());
    ASSERT_TRUE(root);
    const auto original = root->resolve("/dir/some%20file.txt");
    ASSERT_TRUE(original);

    const auto moved_root = temp_ / "moved";
    std::filesystem::rename(root_, moved_root);
    ASSERT_TRUE(std::filesystem::create_directories(root_ / "dir"));
    write(root_ / "dir" / "some file.txt", "different root");
    const auto pinned = root->resolve("/dir/some%20file.txt");
    ASSERT_TRUE(pinned);
    EXPECT_EQ(original->metadata.inode, pinned->metadata.inode);

    const auto replacement = moved_root / "new";
    write(replacement, "change");
    std::filesystem::rename(replacement, moved_root / "dir" / "some file.txt");
    const auto updated = root->resolve("/dir/some%20file.txt");
    ASSERT_TRUE(updated);
    EXPECT_EQ(original->metadata.size, updated->metadata.size);
    const auto old_response = StaticFile::prepare(original->metadata, false);
    const auto new_response = StaticFile::prepare(updated->metadata, false);
    EXPECT_NE(old_response.etag(), new_response.etag());
    const auto conditional =
        StaticFile::prepare(updated->metadata, false, {.if_none_match = old_response.etag()});
    EXPECT_EQ(conditional.status(), 200u);
}

TEST(StaticFileSelection, WeakETagAndConditionalPrecedence) {
    const auto response = StaticFile::prepare(kFile, false);
    ASSERT_TRUE(response.etag().starts_with("W/\""));
    EXPECT_EQ(response.etag().back(), '"');
    std::string list = "\"other,tag\", ";
    list += response.etag().substr(2);
    for (const std::string_view value :
         {response.etag(), std::string_view{list}, std::string_view{" * "}}) {
        const auto match =
            StaticFile::prepare(kFile, false, {.if_none_match = value, .range = "bytes=0-1"});
        EXPECT_EQ(match.status(), 304u);
        EXPECT_EQ(match.etag(), response.etag());
        EXPECT_TRUE(match.content_range().empty());
    }
    EXPECT_EQ(StaticFile::prepare(kFile, false, {.if_none_match = "\"different\""}).status(), 200u);
    auto changed = kFile;
    ++changed.modified.nanoseconds;
    EXPECT_NE(StaticFile::prepare(changed, false).etag(), response.etag());
}

TEST(StaticFileSelection, ModifiedSinceUsesHttpDatesAndEntityTagPrecedence) {
    auto file = kFile;
    file.modified = {784111777, 123};
    constexpr std::string_view date = "Sun, 06 Nov 1994 08:49:37 GMT";
    EXPECT_EQ(StaticFile::prepare(file, false).last_modified(), date);
    for (const std::string_view value :
         {date, std::string_view{"Sunday, 06-Nov-94 08:49:37 GMT"},
          std::string_view{"Sun Nov  6 08:49:37 1994"}}) {
        EXPECT_EQ(StaticFile::prepare(file, true, {.if_modified_since = value}).status(), 304U);
    }
    for (const std::string_view value :
         {"Sun, 06 Nov 1994 08:49:36 GMT", "Sun, 31 Nov 1994 08:49:37 GMT",
          "Sun, 06 Nov 1994 08:49:37 GMT, Sun, 06 Nov 1994 08:49:37 GMT"}) {
        EXPECT_EQ(StaticFile::prepare(file, false, {.if_modified_since = value}).status(), 200U);
    }
    EXPECT_EQ(StaticFile::prepare(file, false, {.if_none_match = "",
                                              .if_modified_since = date}).status(), 200U);
}

TEST(StaticFileSelection, BoundsSingleRangesIncludingOversizedNumbers) {
    const struct {
        std::string_view range;
        std::string_view content_range;
    } cases[] = {{"bytes=0-0", "bytes 0-0/10"},
                 {"bytes=2-5", "bytes 2-5/10"},
                 {"bytes=8-", "bytes 8-9/10"},
                 {"BYTES=-3", "bytes 7-9/10"},
                 {"bytes=8-999999999999999999999", "bytes 8-9/10"},
                 {"bytes=-999999999999999999999", "bytes 0-9/10"}};
    for (const auto& c : cases) {
        SCOPED_TRACE(c.range);
        const auto response = StaticFile::prepare(kFile, false, {.range = c.range});
        EXPECT_EQ(response.status(), 206u);
        EXPECT_EQ(response.content_range(), c.content_range);
    }
    constexpr StaticFileMetadata huge{.size = UINT64_MAX};
    const auto response = StaticFile::prepare(huge, false, {.range = "bytes=-1"});
    EXPECT_EQ(response.content_range(),
              "bytes 18446744073709551614-18446744073709551614/18446744073709551615");
}

TEST(StaticFileSelection, RejectsInvalidAndUnsatisfiableSingleRanges) {
    for (const std::string_view range : {"",
                                         "bytes=",
                                         "bytes=10-",
                                         "bytes=7-3",
                                         "bytes=-0",
                                         "bytes=0-+3",
                                         "bytes=1 -2",
                                         "bytes=1-2-3",
                                         "bytes=999999999999999999999-"}) {
        SCOPED_TRACE(range);
        const auto response = StaticFile::prepare(kFile, false, {.range = range});
        EXPECT_EQ(response.status(), 416u);
        EXPECT_EQ(response.content_range(), "bytes */10");
    }
    constexpr StaticFileMetadata empty{};
    const auto response = StaticFile::prepare(empty, false, {.range = "bytes=0-"});
    EXPECT_EQ(response.status(), 416u);
    EXPECT_EQ(response.content_range(), "bytes */0");
}

TEST(StaticFileSelection, IgnoresRangesForHeadUnsupportedUnitsAndIfRange) {
    for (const auto& conditions :
         {StaticFileConditions{.range = "items=0-1"},
          StaticFileConditions{.range = "bytes=0-1,3-4"},
          StaticFileConditions{.range = "bytes=0-1", .has_if_range = true}}) {
        const auto response = StaticFile::prepare(kFile, false, conditions);
        EXPECT_EQ(response.status(), 200u);
        EXPECT_TRUE(response.content_range().empty());
    }
    const auto head = StaticFile::prepare(kFile, true, {.range = "bytes=0-1"});
    EXPECT_EQ(head.status(), 200u);
    EXPECT_TRUE(head.content_range().empty());
}

class StaticFileApply : public ::testing::Test {
protected:
    void SetUp() override {
        auto chunks = ChunkPool::create(16, 8);
        ASSERT_TRUE(chunks);
        chunks_ = std::move(*chunks);
    }

    void TearDown() override {
        if (auto* ex = pool_.get(handle_)) {
            ex->detach(Side::Ingress, Errc::Canceled);
            ex->detach(Side::Responder, Errc::Canceled);
        }
        while (pool_.has_ready()) {
            (void)pool_.drain(1, [](Handle, Exchange&) {});
        }
        EXPECT_EQ(pool_.live(), 0u);
        EXPECT_EQ(chunks_.free_count(), chunks_.capacity());
    }

    Exchange& open(bool head_only = false) {
        handle_ = pool_.open({}, {.head_only = head_only});
        return *pool_.get(handle_);
    }

    ChunkPool chunks_;
    ExchangePool pool_{1, chunks_, {4, 1}};
    Handle handle_{};
};

TEST_F(StaticFileApply, ClaimsBeforeReadAndResumesAfterBackpressure) {
    auto& ex = open();
    const auto response = StaticFile::prepare(kFile, false, {.range = "bytes=2-8"});
    ApplyCursor cursor;
    const auto first = response.apply(ex, cursor, 8);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->status, FileStatus::Read);
    EXPECT_EQ(first->offset, 2u);
    EXPECT_EQ(first->size, 4u);
    EXPECT_EQ(ex.response_meta().status, 206u);
    EXPECT_TRUE(ex.response_meta().declared_known);
    EXPECT_EQ(ex.response_meta().declared, 7u);
    EXPECT_EQ(ex.stream(StreamId::Res).claimed(), 4u);

    constexpr std::string_view contents = "0123456789";
    const auto bytes = std::as_bytes(std::span(contents));
    ASSERT_TRUE(response.on_read(ex, cursor, bytes.subspan(2, 3)));  // short read
    EXPECT_EQ(ex.stream(StreamId::Res).claimed(), 0u);
    const auto blocked = response.apply(ex, cursor, 8);
    ASSERT_TRUE(blocked);
    EXPECT_EQ(blocked->status, FileStatus::Blocked);
    const auto still_blocked = response.apply(ex, cursor, 8);
    ASSERT_TRUE(still_blocked);
    EXPECT_EQ(still_blocked->status, FileStatus::Blocked);

    const auto front = ex.front(StreamId::Res);
    EXPECT_TRUE(std::ranges::equal(front, bytes.subspan(2, 3)));
    ex.consume(StreamId::Res, 3);
    EXPECT_NE(ex.pending_events(Side::Responder) & ExEvent::kResWritable, 0);
    const auto second = response.apply(ex, cursor, 8);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, FileStatus::Read);
    EXPECT_EQ(second->offset, 5u);
    EXPECT_EQ(second->size, 4u);
    ASSERT_TRUE(response.on_read(ex, cursor, bytes.subspan(5, 4)));
    const auto done = response.apply(ex, cursor, 0);
    ASSERT_TRUE(done);
    EXPECT_EQ(done->status, FileStatus::Done);
    EXPECT_TRUE(std::ranges::equal(ex.front(StreamId::Res), bytes.subspan(5, 4)));
    ex.consume(StreamId::Res, 4);
    EXPECT_TRUE(ex.stream(StreamId::Res).end_seen());
}

TEST_F(StaticFileApply, HeadPublishesLengthWithoutClaimingCredit) {
    auto& ex = open(true);
    const auto response = StaticFile::prepare(kFile, true, {.range = "bytes=0-1"});
    ApplyCursor cursor;
    const auto done = response.apply(ex, cursor, 0);
    ASSERT_TRUE(done);
    EXPECT_EQ(done->status, FileStatus::Done);
    EXPECT_TRUE(ex.response_meta().no_content);
    EXPECT_TRUE(ex.response_meta().declared_known);
    EXPECT_EQ(ex.response_meta().declared, 10u);
    EXPECT_TRUE(ex.stream(StreamId::Res).end_seen());
    EXPECT_EQ(ex.stream(StreamId::Res).pushed(), 0u);
}

TEST_F(StaticFileApply, NotModifiedAndUnsatisfiableFinishWithoutReads) {
    const std::array responses{
        StaticFile::prepare(kFile, false, {.if_none_match = "*"}),
        StaticFile::prepare(kFile, false, {.range = "bytes=10-"}),
        StaticFile::prepare(StaticFileMetadata{}, false),
    };
    for (const auto& response : responses) {
        auto& ex = open();
        ApplyCursor cursor;
        const auto done = response.apply(ex, cursor, 0);
        ASSERT_TRUE(done);
        EXPECT_EQ(done->status, FileStatus::Done);
        EXPECT_EQ(ex.response_meta().status, response.status());
        EXPECT_EQ(ex.response_meta().no_content, response.status() == 304);
        EXPECT_EQ(ex.response_meta().declared_known, response.status() != 304);
        EXPECT_EQ(ex.response_meta().declared, 0u);
        EXPECT_TRUE(ex.stream(StreamId::Res).end_seen());
        EXPECT_EQ(ex.stream(StreamId::Res).pushed(), 0u);
        ex.detach(Side::Ingress, Errc::Canceled);
        ex.detach(Side::Responder, Errc::Canceled);
        (void)pool_.drain(1, [](Handle, Exchange&) {});
        ASSERT_EQ(pool_.live(), 0u);
    }
}

TEST_F(StaticFileApply, PrematureEofAbortsAndDropsQueuedDataAndCredit) {
    auto& ex = open();
    const auto response = StaticFile::prepare(kFile, false);
    ApplyCursor cursor;
    auto step = response.apply(ex, cursor, 2);
    ASSERT_TRUE(step);
    ASSERT_EQ(step->status, FileStatus::Read);
    const std::array<std::byte, 2> bytes{};
    ASSERT_TRUE(response.on_read(ex, cursor, std::span<const std::byte>{bytes}));
    step = response.apply(ex, cursor, 2);
    ASSERT_TRUE(step);
    ASSERT_EQ(step->status, FileStatus::Read);
    const auto eof = response.on_read(ex, cursor, std::span<const std::byte>{});
    ASSERT_FALSE(eof);
    EXPECT_EQ(eof.error().code, Errc::Eof);
    EXPECT_EQ(ex.stream(StreamId::Res).state(), StreamState::Aborted);
    EXPECT_EQ(ex.stream(StreamId::Res).claimed(), 0u);
    EXPECT_EQ(ex.stream(StreamId::Res).queued(), 0u);
}

TEST_F(StaticFileApply, LateCompletionAfterIngressDetachIsCanceled) {
    auto& ex = open();
    const auto response = StaticFile::prepare(kFile, false);
    ApplyCursor cursor;
    const auto step = response.apply(ex, cursor, 2);
    ASSERT_TRUE(step);
    ASSERT_EQ(step->status, FileStatus::Read);
    ex.detach(Side::Ingress, Errc::Canceled);
    const std::array<std::byte, 2> bytes{};
    const auto completion = response.on_read(ex, cursor, std::span<const std::byte>{bytes});
    ASSERT_FALSE(completion);
    EXPECT_EQ(completion.error().code, Errc::Canceled);
    EXPECT_EQ(ex.stream(StreamId::Res).claimed(), 0u);
}

TEST_F(StaticFileApply, ExhaustedChunkPoolAbortsAndReturnsCredit) {
    chunks_ = ChunkPool{};
    auto& ex = open();
    const auto response = StaticFile::prepare(kFile, false);
    ApplyCursor cursor;
    const auto step = response.apply(ex, cursor, 2);
    ASSERT_TRUE(step);
    ASSERT_EQ(step->status, FileStatus::Read);

    const std::array<std::byte, 2> bytes{};
    const auto completion = response.on_read(ex, cursor, std::span<const std::byte>{bytes});
    ASSERT_FALSE(completion);
    EXPECT_EQ(completion.error().code, Errc::Overloaded);
    EXPECT_EQ(ex.stream(StreamId::Res).state(), StreamState::Aborted);
    EXPECT_EQ(ex.stream(StreamId::Res).claimed(), 0u);
    EXPECT_EQ(ex.stream(StreamId::Res).queued(), 0u);
}

}  // namespace
