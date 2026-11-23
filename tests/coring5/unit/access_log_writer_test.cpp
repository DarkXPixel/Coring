#include "coring5/obs/AccessLogWriter.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <unistd.h>

namespace Coring5 {
namespace {
int fd_count() {
  DIR *dir = opendir("/proc/self/fd");
  if (!dir) {
    return -1;
  }
  int count = 0;
  while (readdir(dir)) {
    ++count;
  }
  closedir(dir);
  return count;
}

class AccessLogTest : public testing::Test {
protected:
  void SetUp() override {
    initial_fds = fd_count();
    ASSERT_GT(initial_fds, 0);
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(close(fd), 0);
  }

  void TearDown() override {
    EXPECT_EQ(unlink(path), 0);
    EXPECT_EQ(fd_count(), initial_fds);
  }

  char path[40] = "/tmp/coring5-access-XXXXXX";
  int initial_fds{};
  std::unique_ptr<AccessLogRing[]> rings{new AccessLogRing[2]};
};

TEST_F(AccessLogTest, FullRingDropsAndStopFlushesEveryAcceptedRecordInOrder) {
  AccessRecord record{.duration_us = 2,
                      .listener = 3,
                      .bytes_in = 4,
                      .bytes_out = 5,
                      .status = 200,
                      .method_len = 3,
                      .target_len = 4,
                      .method = {'G', 'E', 'T'},
                      .target = {'/', '\n', '%', '\0'}};
  for (unsigned i = 0; i < 4096; ++i) {
    record.unix_ms = i;
    ASSERT_TRUE(rings[0].push(record));
  }
  EXPECT_FALSE(rings[0].push(record));
  record.listener = 9;
  ASSERT_TRUE(rings[1].push(record));

  AccessLogWriter writer;
  ASSERT_TRUE(writer.start(path, {rings.get(), 2}));
  ASSERT_TRUE(writer.stop());
  EXPECT_TRUE(writer.stop());
  EXPECT_EQ(rings[0].size_approx(), 0U);
  EXPECT_EQ(rings[1].size_approx(), 0U);
  std::ifstream output(path);
  std::string line;
  unsigned first = 0, second = 0;
  while (std::getline(output, line)) {
    if (line == "4095\t2\t9\t4\t5\t200\tGET\t/%0A%25%00") {
      EXPECT_LT(first, 4096U); // one busy ring cannot monopolize the writer
      ++second;
    } else {
      ASSERT_EQ(line,
                std::to_string(first) + "\t2\t3\t4\t5\t200\tGET\t/%0A%25%00");
      ++first;
    }
  }
  EXPECT_EQ(first, 4096U);
  EXPECT_EQ(second, 1U);
}

TEST_F(AccessLogTest, WritesWhileRunningAndDestructorFlushesFinalPush) {
  std::ofstream(path) << "existing\n";
  const int watch = inotify_init1(IN_CLOEXEC);
  ASSERT_GE(watch, 0);
  ASSERT_GE(inotify_add_watch(watch, path, IN_MODIFY), 0);
  {
    AccessLogWriter writer;
    ASSERT_TRUE(writer.start(path, {rings.get(), 1}));
    AccessRecord record{.method_len = 255, .target_len = 255};
    record.method.fill('M');
    record.target.fill('T');
    ASSERT_TRUE(rings[0].push(record));
    pollfd changed{watch, POLLIN, 0};
    ASSERT_EQ(poll(&changed, 1, 5000), 1);
    ASSERT_TRUE(rings[0].push({}));
  }
  ASSERT_EQ(close(watch), 0);
  std::ifstream output(path);
  std::string line;
  ASSERT_TRUE(std::getline(output, line));
  EXPECT_EQ(line, "existing");
  ASSERT_TRUE(std::getline(output, line));
  EXPECT_EQ(line, "0\t0\t0\t0\t0\t0\t" + std::string(8, 'M') + "\t" +
                      std::string(96, 'T'));
  ASSERT_TRUE(std::getline(output, line));
  EXPECT_EQ(line, "0\t0\t0\t0\t0\t0\t\t");
  EXPECT_FALSE(std::getline(output, line));
}

TEST_F(AccessLogTest, RotationRetainsWholeRecordsAndBoundsArchiveCount) {
  constexpr std::string_view suffix = "\t0\t0\t0\t0\t0\t\t";
  const auto first = std::string(path) + ".1";
  const auto second = std::string(path) + ".2";
  AccessLogWriter writer;
  // Each record is longer than the limit and occupies its own file.
  ASSERT_TRUE(writer.start(path, {rings.get(), 1}, {1, 2}));
  for (unsigned i = 1; i <= 4; ++i) {
    EXPECT_TRUE(rings[0].push({.unix_ms = i}));
  }
  ASSERT_TRUE(writer.stop());
  for (const auto &[name, index] :
       std::array{std::pair{std::string(path), 4}, std::pair{first, 3},
                  std::pair{second, 2}}) {
    std::ifstream file(name);
    std::string line;
    EXPECT_TRUE(std::getline(file, line));
    EXPECT_EQ(line, std::to_string(index) + std::string(suffix));
    EXPECT_FALSE(std::getline(file, line));
  }
  EXPECT_EQ(unlink(first.c_str()), 0);
  EXPECT_EQ(unlink(second.c_str()), 0);
  EXPECT_NE(access((std::string(path) + ".3").c_str(), F_OK), 0);
}

TEST_F(AccessLogTest, ShutdownReportsWriteFailure) {
  const int fd =
      memfd_create("access-log-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE), 0);
  const auto sealed_path = "/proc/self/fd/" + std::to_string(fd);
  AccessLogWriter writer;
  ASSERT_TRUE(writer.start(sealed_path.c_str(), {rings.get(), 1}));
  ASSERT_EQ(close(fd), 0);
  ASSERT_TRUE(rings[0].push({}));
  for (int i = 0; i < 2; ++i) {
    auto result = writer.stop();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().sys, EPERM);
  }
}

TEST_F(AccessLogTest, FailedStartLeavesRingsAvailableForRetry) {
  AccessLogWriter writer;
  ASSERT_TRUE(rings[0].push({}));
  EXPECT_FALSE(writer.start("/dev/null", {rings.get(), 1}));
  EXPECT_EQ(rings[0].size_approx(), 1U);
  ASSERT_TRUE(writer.start(path, {rings.get(), 1}));
  EXPECT_FALSE(writer.start(path, {rings.get(), 1}));
  ASSERT_TRUE(writer.stop());
  EXPECT_EQ(rings[0].size_approx(), 0U);
}

} // namespace
} // namespace Coring5
