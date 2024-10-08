/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <cmath>

#include <dispenso/task_set.h>

#if !defined(BENCHMARK_WITHOUT_TBB)
#include "tbb/task_group.h"
#include "tbb/task_scheduler_init.h"
#endif // !BENCHMARK_WITHOUT_TBB

#if !defined(BENCHMARK_WITHOUT_FOLLY)
#include <folly/VirtualExecutor.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Coroutine.h>
#include <folly/coro/Task.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#endif // !BENCHMARK_WITHOUT_FOLLY

#include "thread_benchmark_common.h"

using namespace std::chrono_literals;

static constexpr int kSmallSize = 1000;
static constexpr int kMediumSize = 10000;
static constexpr int kLargeSize = 1000000;

struct alignas(64) Work {
  size_t count = 0;

  void operator+=(size_t o) {
    count += o;
  }
};

Work g_work[1025];
std::atomic<int> g_tCounter{0};
inline int testTid() {
  static DISPENSO_THREAD_LOCAL int t = -1;
  if (t < 0) {
    t = g_tCounter.fetch_add(1, std::memory_order_acq_rel);
  }
  return t;
}

inline Work& work() {
  static DISPENSO_THREAD_LOCAL Work* w = nullptr;

  if (!w) {
    if (testTid() == 0) {
      w = g_work + 1024;
    } else {
      w = g_work + (testTid() & 1023);
    }
  }
  return *w;
}

void BM_dispenso(benchmark::State& state) {
  const int num_threads = state.range(0) - 1;
  const int num_elements = state.range(1);
  dispenso::ThreadPool pool(num_threads);

  for (auto UNUSED_VAR : state) {
    dispenso::TaskSet tasks(pool);
    for (int i = 0; i < num_elements; ++i) {
      tasks.schedule([i]() { work() += i; });
    }
  }
}

#if !defined(BENCHMARK_WITHOUT_FOLLY)
void BM_folly(benchmark::State& state) {
  const int num_threads = state.range(0);
  const int num_elements = state.range(1);
  folly::CPUThreadPoolExecutor follyExec(num_threads);

  for (auto UNUSED_VAR : state) {
    folly::VirtualExecutor tasks(&follyExec);
    for (int i = 0; i < num_elements; ++i) {
      tasks.add([i]() { work() += i; });
    }
  }
}

void BM_folly2(benchmark::State& state) {
  const int num_threads = state.range(0);
  const int num_elements = state.range(1);

  if (num_elements > 10000) {
    state.SkipWithError("We run out of memory here with too many elements");
  }

  folly::CPUThreadPoolExecutor follyExec(num_threads);
  for (auto UNUSED_VAR : state) {
    folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
      std::vector<folly::coro::Task<void>> tasks;
      for (int i = 0; i < num_elements; ++i) {
        tasks.push_back(folly::coro::co_invoke([num_elements]() -> folly::coro::Task<void> {
          co_await folly::coro::co_reschedule_on_current_executor;
          std::vector<folly::coro::Task<void>> tasks2;
          int num = std::sqrt(num_elements);
          for (int j = 0; j < num; ++j) {
            tasks2.push_back(folly::coro::co_invoke([j]() -> folly::coro::Task<void> {
              co_await folly::coro::co_reschedule_on_current_executor;
              work() += j;
            }));
          }
          co_await folly::coro::collectAllRange(std::move(tasks2));
        }));
      }
      co_await folly::coro::collectAllRange(std::move(tasks)).scheduleOn(&follyExec);
    }());
  }
}
#endif // !BENCHMARK_WITHOUT_FOLLY

#if !defined(BENCHMARK_WITHOUT_TBB)
void BM_tbb(benchmark::State& state) {
  const int num_threads = state.range(0);
  const int num_elements = state.range(1);
  tbb::task_scheduler_init initsched(num_threads);

  for (auto UNUSED_VAR : state) {
    tbb::task_group g;
    for (int i = 0; i < num_elements; ++i) {
      g.run([i]() { work() += i; });
    }
    g.wait();
  }
}

void BM_tbb2(benchmark::State& state) {
  const int num_threads = state.range(0);
  const int num_elements = state.range(1);

  for (auto UNUSED_VAR : state) {
    tbb::task_scheduler_init initsched(num_threads);
    tbb::task_group g;
    for (int i = 0; i < num_elements; ++i) {
      g.run([num_elements]() {
        int num = std::sqrt(num_elements);
        tbb::task_group g2;
        for (int j = 0; j < num; ++j) {
          g2.run([j]() { work() += j; });
        }
        g2.wait();
      });
    }
    g.wait();
  }
}

void BM_tbb_mostly_idle(benchmark::State& state) {
  const int num_threads = state.range(0);
  const int num_elements = state.range(1);

  struct Recurse {
    void operator()() const {
      work() += i;
      if (i < num_elements) {
        ++i;
        g->run(*this);
      }
    }

    mutable int i;
    mutable tbb::task_group* g;
    int num_elements;
  };

  startRusage();

  for (auto UNUSED_VAR : state) {
    tbb::task_scheduler_init initsched(num_threads);
    tbb::task_group g;
    Recurse rec;
    rec.i = 0;
    rec.g = &g;
    rec.num_elements = num_elements;
    rec();
    g.wait();
  }
  endRusage(state);
}

void BM_tbb_very_idle(benchmark::State& state) {
  const int num_threads = state.range(0);

  startRusage();

  for (auto UNUSED_VAR : state) {
    tbb::task_scheduler_init initsched(num_threads);
    tbb::task_group g;
    g.run([]() {});
    std::this_thread::sleep_for(100ms);
    g.run([]() {});
    g.wait();
  }
  endRusage(state);
}

#endif // !BENCHMARK_WITHOUT_TBB

void BM_dispenso2(benchmark::State& state) {
  const int num_threads = state.range(0) - 1;
  const int num_elements = state.range(1);

  for (auto UNUSED_VAR : state) {
    dispenso::ThreadPool pool(num_threads);
    for (int i = 0; i < num_elements; ++i) {
      pool.schedule([&pool, num_elements]() {
        int num = std::sqrt(num_elements);
        dispenso::TaskSet tasks(pool);
        for (int j = 0; j < num; ++j) {
          tasks.schedule([j]() { work() += j; });
        }
      });
    }
  }
}

void BM_dispenso_mostly_idle(benchmark::State& state) {
  const int num_threads = std::max<int>(1, state.range(0) - 1);
  const int num_elements = state.range(1);

  struct Recurse {
    void operator()() {
      work() += i;
      if (i < num_elements) {
        ++i;
        pool->schedule(*this);
      }
    }

    int i;
    dispenso::ThreadPool* pool;
    int num_elements;
  };

  startRusage();

  for (auto UNUSED_VAR : state) {
    dispenso::ThreadPool pool(num_threads);
    Recurse rec;
    rec.i = 0;
    rec.pool = &pool;
    rec.num_elements = num_elements;
    rec();
  }

  endRusage(state);
}

void BM_dispenso_very_idle(benchmark::State& state) {
  const int num_threads = state.range(0) - 1;
  startRusage();

  for (auto UNUSED_VAR : state) {
    dispenso::ThreadPool pool(num_threads);
    pool.schedule([]() {});
    std::this_thread::sleep_for(100ms);
    pool.schedule([]() {});
  }

  endRusage(state);
}

static void CustomArguments(benchmark::internal::Benchmark* b) {
  for (int j : {kSmallSize, kMediumSize, kLargeSize}) {
    for (int s : pow2HalfStepThreads()) {
      b->Args({s, j});
    }
  }
}

static void CustomArgumentsVeryIdle(benchmark::internal::Benchmark* b) {
  for (int s : pow2HalfStepThreads()) {
    b->Args({s});
  }
}

#if !defined(BENCHMARK_WITHOUT_TBB)
BENCHMARK(BM_tbb)->Apply(CustomArguments)->Unit(benchmark::kMicrosecond)->UseRealTime();
#endif // !BENCHMARK_WITHOUT_TBB

#if !defined(BENCHMARK_WITHOUT_FOLLY)
BENCHMARK(BM_folly)->Apply(CustomArguments)->Unit(benchmark::kMicrosecond)->UseRealTime();
#endif // !BENCHMARK_WITHOUT_FOLLY
BENCHMARK(BM_dispenso)->Apply(CustomArguments)->Unit(benchmark::kMicrosecond)->UseRealTime();

#if !defined(BENCHMARK_WITHOUT_TBB)
BENCHMARK(BM_tbb2)->Apply(CustomArguments)->Unit(benchmark::kMicrosecond)->UseRealTime();
#endif // !BENCHMARK_WITHOUT_TBB
#if !defined(BENCHMARK_WITHOUT_FOLLY)
BENCHMARK(BM_folly2)->Apply(CustomArguments)->Unit(benchmark::kMicrosecond)->UseRealTime();
#endif // !BENCHMARK_WITHOUT_FOLLY
BENCHMARK(BM_dispenso2)->Apply(CustomArguments)->Unit(benchmark::kMicrosecond)->UseRealTime();

#if !defined(BENCHMARK_WITHOUT_TBB)
BENCHMARK(BM_tbb_mostly_idle)->Apply(CustomArguments)->Unit(benchmark::kMicrosecond)->UseRealTime();
#endif // !BENCHMARK_WITHOUT_TBB
BENCHMARK(BM_dispenso_mostly_idle)
    ->Apply(CustomArguments)
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

#if !defined(BENCHMARK_WITHOUT_TBB)
BENCHMARK(BM_tbb_very_idle)
    ->Apply(CustomArgumentsVeryIdle)
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();
#endif // !BENCHMARK_WITHOUT_TBB
BENCHMARK(BM_dispenso_very_idle)
    ->Apply(CustomArgumentsVeryIdle)
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

BENCHMARK_MAIN();
