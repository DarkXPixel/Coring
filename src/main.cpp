#include "coring2/WorkerThreadLoop.hpp"
#include "coring2/uring/UringEventLoop.hpp"
#include "utility/CliParser.hpp"
#include <liburing.h>
#include <print>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>

int main(int argc, char *argv[]) {
  auto config = Coring::Utility::CliParser::parse_args(argc, argv);
  if (!config) {
    switch (config.error()) {
    case Coring::Utility::CliParseError::InvalidArgument:
      std::println("Invalid argument");
      break;
    default:
      break;
    }
    return -1;
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  Coring2::WorkerThreadLoop<Coring2::UringLoop> t;
  t.start(8888);

  return 0;
}