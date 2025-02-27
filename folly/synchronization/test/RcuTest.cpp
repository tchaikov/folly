/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/synchronization/Rcu.h>

#include <thread>
#include <vector>

#include <glog/logging.h>

#include <folly/Benchmark.h>
#include <folly/Random.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>

using namespace folly;

DEFINE_int64(iters, 100000, "Number of iterations");
DEFINE_uint64(threads, 32, "Number of threads");

TEST(RcuTest, Basic) {
  auto foo = new int(2);
  rcu_retire(foo);
}

class des {
  bool* d_;

 public:
  des(bool* d) : d_(d) {}
  ~des() { *d_ = true; }
};

TEST(RcuTest, Guard) {
  bool del = false;
  auto foo = new des(&del);
  { rcu_reader g; }
  rcu_retire(foo);
  rcu_synchronize();
  EXPECT_TRUE(del);
}

TEST(RcuTest, SlowReader) {
  std::thread t;
  {
    rcu_reader g;

    t = std::thread([&]() { rcu_synchronize(); });
    usleep(100); // Wait for synchronize to start
  }
  t.join();
}

rcu_reader tryretire(des* obj) {
  rcu_reader g;
  rcu_retire(obj);
  return g;
}

TEST(RcuTest, CopyGuard) {
  bool del = false;
  auto foo = new des(&del);
  {
    auto res = tryretire(foo);
    EXPECT_FALSE(del);
  }
  rcu_barrier();
  EXPECT_TRUE(del);
}

TEST(RcuTest, Stress) {
  std::vector<std::thread> readers;
  constexpr uint32_t sz = 1000;
  std::atomic<int*> ints[sz];
  for (uint32_t i = 0; i < sz; i++) {
    ints[i].store(new int(0));
  }
  for (unsigned th = 0; th < FLAGS_threads; th++) {
    readers.push_back(std::thread([&]() {
      for (int i = 0; i < FLAGS_iters / 100; i++) {
        rcu_reader g;
        int sum = 0;
        int* ptrs[sz];
        for (uint32_t j = 0; j < sz; j++) {
          ptrs[j] = ints[j].load(std::memory_order_acquire);
        }
        for (uint32_t j = 0; j < sz; j++) {
          sum += *ptrs[j];
        }
        EXPECT_EQ(sum, 0);
      }
    }));
  }
  std::atomic<bool> done{false};
  std::vector<std::thread> updaters;
  for (unsigned th = 0; th < FLAGS_threads; th++) {
    updaters.push_back(std::thread([&]() {
      while (!done.load()) {
        auto newint = new int(0);
        auto oldint = ints[folly::Random::rand32() % sz].exchange(newint);
        if (folly::Random::rand32() % 2 == 0) {
          rcu_retire<int>(oldint, [](int* obj) {
            *obj = folly::Random::rand32();
            delete obj;
          });
        } else {
          rcu_synchronize();
          *oldint = folly::Random::rand32();
          delete oldint;
        }
      }
    }));
  }
  for (auto& t : readers) {
    t.join();
  }
  done = true;

  for (auto& t : updaters) {
    t.join();
  }

  // Cleanup for asan
  rcu_synchronize();
  for (uint32_t i = 0; i < sz; i++) {
    delete ints[i].exchange(nullptr);
  }
}

TEST(RcuTest, Synchronize) {
  std::vector<std::thread> threads;
  for (unsigned th = 0; th < FLAGS_threads; th++) {
    threads.push_back(std::thread([&]() {
      for (int i = 0; i < 10; i++) {
        rcu_synchronize();
      }
    }));
  }
  for (auto& t : threads) {
    t.join();
  }
}

TEST(RcuTest, NewDomainTest) {
  struct UniqueTag;
  rcu_domain<UniqueTag> newdomain(nullptr);
  rcu_synchronize(newdomain);
}

TEST(RcuTest, NewDomainGuardTest) {
  struct UniqueTag;
  rcu_domain<UniqueTag> newdomain(nullptr);
  bool del = false;
  auto foo = new des(&del);
  { rcu_reader_domain<UniqueTag> g(newdomain); }
  rcu_retire(foo, {}, newdomain);
  rcu_synchronize(newdomain);
  EXPECT_TRUE(del);
}

TEST(RcuTest, MovableReader) {
  {
    rcu_reader g;
    rcu_reader f(std::move(g));
  }
  rcu_synchronize();
  {
    rcu_reader g(std::defer_lock);
    rcu_reader f;
    g = std::move(f);
  }
  rcu_synchronize();
}

TEST(RcuTest, SynchronizeInCall) {
  rcu_default_domain().call([]() { rcu_synchronize(); });
  rcu_synchronize();
}

TEST(RcuTest, MoveReaderBetweenThreads) {
  rcu_reader g;
  std::thread t([f = std::move(g)] {});
  t.join();
  rcu_synchronize();
}

TEST(RcuTest, ForkTest) {
  rcu_token<RcuTag> epoch;
  std::thread t([&]() { epoch = rcu_default_domain().lock_shared(); });
  t.join();
  auto pid = fork();
  if (pid) {
    // parent
    rcu_default_domain().unlock_shared(std::move(epoch));
    rcu_synchronize();
    int status = -1;
    auto pid2 = waitpid(pid, &status, 0);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(pid, pid2);
  } else {
    // child
    rcu_synchronize();
    exit(0); // Do not print gtest results
  }
}

TEST(RcuTest, ThreadLocalList) {
  struct TTag;
  folly::detail::ThreadCachedLists<TTag> lists;
  std::vector<std::thread> threads{FLAGS_threads};
  std::atomic<unsigned long> done{FLAGS_threads};
  for (auto& tr : threads) {
    tr = std::thread([&]() {
      for (int i = 0; i < FLAGS_iters; i++) {
        auto node = new folly::detail::ThreadCachedListsBase::Node;
        lists.push(node);
      }
      --done;
    });
  }
  while (done.load() > 0) {
    folly::detail::ThreadCachedLists<TTag>::ListHead list{};
    lists.collect(list);
    list.forEach([](folly::detail::ThreadCachedLists<TTag>::Node* node) {
      delete node;
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  // Run cleanup pass one more time to make ASAN happy
  folly::detail::ThreadCachedLists<TTag>::ListHead list{};
  lists.collect(list);
  list.forEach(
      [](folly::detail::ThreadCachedLists<TTag>::Node* node) { delete node; });
}

TEST(RcuTest, ThreadDeath) {
  bool del = false;
  std::thread t([&] {
    auto foo = new des(&del);
    rcu_retire(foo);
  });
  t.join();
  rcu_synchronize();
  EXPECT_TRUE(del);
}

TEST(RcuTest, RcuObjBase) {
  bool retired = false;
  struct base_test : rcu_obj_base<base_test> {
    bool* ret_;
    base_test(bool* ret) : ret_(ret) {}
    ~base_test() { (*ret_) = true; }
  };

  auto foo = new base_test(&retired);
  foo->retire();
  rcu_synchronize();
  EXPECT_TRUE(retired);
}

TEST(RcuTest, Tsan) {
  int data = 0;
  std::thread t1([&] {
    auto epoch = rcu_default_domain().lock_shared();
    data = 1;
    rcu_default_domain().unlock_shared(std::move(epoch));
    // Delay before exiting so the thread is still alive for TSAN detection.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  });

  std::thread t2([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // This should establish a happens-before relationship between the earlier
    // write (data = 1) and this write below (data = 2).
    rcu_default_domain().synchronize();
    data = 2;
  });

  t1.join();
  t2.join();
  EXPECT_EQ(data, 2);
}
