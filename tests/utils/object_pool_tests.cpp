////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 by EMC Corporation, All Rights Reserved
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"
#include "utils/object_pool.hpp"
#include "utils/thread_utils.hpp"

NS_BEGIN(tests)

struct test_slow_sobject {
  DECLARE_SHARED_PTR(test_slow_sobject);
  int id;
  test_slow_sobject (int i): id(i) {
    ++TOTAL_COUNT;
  }
  static std::atomic<size_t> TOTAL_COUNT; // # number of objects created
  static ptr make(int i) {
    irs::sleep_ms(2000);
    return ptr(new test_slow_sobject(i));
  }
};

std::atomic<size_t> test_slow_sobject::TOTAL_COUNT{};

struct test_sobject {
  DECLARE_SHARED_PTR(test_sobject);
  int id;
  test_sobject(int i): id(i) { }
  static ptr make(int i) { return ptr(new test_sobject(i)); }
};

struct test_uobject {
  DECLARE_UNIQUE_PTR(test_uobject);
  int id;
  test_uobject(int i): id(i) {}
  static ptr make(int i) { return ptr(new test_uobject(i)); }
};

NS_END

using namespace tests;

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

TEST(bounded_object_pool_tests, check_total_number_of_instances) {
  const size_t MAX_COUNT = 2;
  iresearch::bounded_object_pool<test_slow_sobject> pool(MAX_COUNT);

  std::mutex mutex;
  std::condition_variable ready_cv;
  bool ready{false};

  std::atomic<size_t> id{};
  test_slow_sobject::TOTAL_COUNT = 0;

  auto job = [&mutex, &ready_cv, &pool, &ready, &id](){
    // wait for all threads to be ready
    {
      SCOPED_LOCK_NAMED(mutex, lock);

      while (!ready) {
        ready_cv.wait(lock);
      }
    }

    pool.emplace(id++);
  };

  auto job_shared = [&mutex, &ready_cv, &pool, &ready, &id](){
    // wait for all threads to be ready
    {
      SCOPED_LOCK_NAMED(mutex, lock);

      while (!ready) {
        ready_cv.wait(lock);
      }
    }

    pool.emplace(id++).release();
  };

  const size_t THREADS_COUNT = 32;
  std::vector<std::thread> threads;

  for (size_t i = 0; i < THREADS_COUNT/2; ++i) {
    threads.emplace_back(job);
    threads.emplace_back(job_shared);
  }

  // ready
  ready = true;
  ready_cv.notify_all();

  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_LE(test_slow_sobject::TOTAL_COUNT.load(), MAX_COUNT);
}

TEST(bounded_object_pool_tests, test_sobject_pool) {
  // block on full pool
  {
    std::condition_variable cond;
    std::mutex mutex;
    iresearch::bounded_object_pool<test_sobject> pool(1);
    auto obj = pool.emplace(1).release();

    {
      SCOPED_LOCK_NAMED(mutex, lock);
      std::atomic<bool> emplace(false);
      std::thread thread([&cond, &mutex, &pool, &emplace]()->void{ auto obj = pool.emplace(2); emplace = true; SCOPED_LOCK(mutex); cond.notify_all(); });

      auto result = cond.wait_for(lock, std::chrono::milliseconds(1000)); // assume thread blocks in 1000ms

      // MSVC 2015/2017 optimized code seems to sporadically notify condition variables without explicit request
      MSVC2015_OPTIMIZED_ONLY(while(!emplace && result == std::cv_status::no_timeout) result = cond.wait_for(lock, std::chrono::milliseconds(1000)));
      MSVC2017_ONLY(while(!emplace && result == std::cv_status::no_timeout) result = cond.wait_for(lock, std::chrono::milliseconds(1000)));

      ASSERT_EQ(std::cv_status::timeout, result);
      // ^^^ expecting timeout because pool should block indefinitely
      obj.reset();
      lock.unlock();
      thread.join();
    }
  }

  // test object reuse
  {
    iresearch::bounded_object_pool<test_sobject> pool(1);
    auto obj = pool.emplace(1);
    auto* obj_ptr = obj.get();

    ASSERT_EQ(1, obj->id);
    obj.reset();
    auto obj_shared = pool.emplace(2).release();
    ASSERT_EQ(1, obj_shared->id);
    ASSERT_EQ(obj_ptr, obj_shared.get());
  }

  // test shared visitation
  {
    iresearch::bounded_object_pool<test_sobject> pool(1);
    auto obj = pool.emplace(1);
    std::condition_variable cond;
    std::mutex mutex;
    SCOPED_LOCK_NAMED(mutex, lock);
    std::thread thread([&cond, &mutex, &pool]()->void {
      auto visitor = [](test_sobject& obj)->bool { return true; };
      pool.visit(visitor, true);
      SCOPED_LOCK(mutex);
      cond.notify_all();
    });
    auto result = cond.wait_for(lock, std::chrono::milliseconds(1000)); // assume thread finishes in 1000ms

    obj.reset();

    if (lock) {
      lock.unlock();
    }

    thread.join();
    ASSERT_EQ(std::cv_status::no_timeout, result); // check only after joining with thread to avoid early exit
  }

  // test exclusive visitation
  {
    iresearch::bounded_object_pool<test_sobject> pool(1);
    auto obj = pool.emplace(1);
    std::condition_variable cond;
    std::mutex mutex;
    SCOPED_LOCK_NAMED(mutex, lock);
    std::atomic<bool> visit(false);
    std::thread thread([&cond, &mutex, &pool, &visit]()->void {
      auto visitor = [](test_sobject& obj)->bool { return true; };
      pool.visit(visitor, false);
      visit = true;
      SCOPED_LOCK(mutex);
      cond.notify_all();
    });
    auto result = cond.wait_for(lock, std::chrono::milliseconds(1000)); // assume thread finishes in 1000ms

    // MSVC 2015/2017 optimized code seems to sporadically notify condition variables without explicit request
    MSVC2015_OPTIMIZED_ONLY(while(!visit && result == std::cv_status::no_timeout) result = cond.wait_for(lock, std::chrono::milliseconds(1000)));
    MSVC2017_ONLY(while(!visit && result == std::cv_status::no_timeout) result = cond.wait_for(lock, std::chrono::milliseconds(1000)));

    obj.reset();

    if (lock) {
      lock.unlock();
    }

    thread.join();
    ASSERT_EQ(std::cv_status::timeout, result); // check only after joining with thread to avoid early exit
    // ^^^ expecting timeout because pool should block indefinitely
  }
}

//TEST(object_pool_tests, bounded_uobject_pool) {
//  // block on full pool
//  {
//    std::condition_variable cond;
//    std::mutex mutex;
//    iresearch::bounded_object_pool<test_uobject> pool(1);
//    auto obj = pool.emplace(1);
//
//    {
//      SCOPED_LOCK_NAMED(mutex, lock);
//      std::atomic<bool> emplace(false);
//      std::thread thread([&cond, &mutex, &pool, &emplace]()->void{ auto obj = pool.emplace(2); emplace = true; SCOPED_LOCK(mutex); cond.notify_all(); });
//
//      auto result = cond.wait_for(lock, std::chrono::milliseconds(1000)); // assume thread blocks in 1000ms
//
//      // MSVC 2015/2017 optimized code seems to sporadically notify condition variables without explicit request
//      MSVC2015_OPTIMIZED_ONLY(while(!emplace && result == std::cv_status::no_timeout) result = cond.wait_for(lock, std::chrono::milliseconds(1000)));
//      MSVC2017_ONLY(while(!emplace && result == std::cv_status::no_timeout) result = cond.wait_for(lock, std::chrono::milliseconds(1000)));
//
//      ASSERT_EQ(std::cv_status::timeout, result);
//      // ^^^ expecting timeout because pool should block indefinitely
//      obj.reset();
//      obj.reset();
//      lock.unlock();
//      thread.join();
//    }
//  }
//
//  // test object reuse
//  {
//    iresearch::bounded_object_pool<test_uobject> pool(1);
//    auto obj = pool.emplace(1);
//    auto* obj_ptr = obj.get();
//
//    ASSERT_EQ(1, obj->id);
//    obj.reset();
//    obj = pool.emplace(2);
//    ASSERT_EQ(1, obj->id);
//    ASSERT_EQ(obj_ptr, obj.get());
//  }
//
//  // test shared visitation
//  {
//    iresearch::bounded_object_pool<test_uobject> pool(1);
//    auto obj = pool.emplace(1);
//    std::condition_variable cond;
//    std::mutex mutex;
//    SCOPED_LOCK_NAMED(mutex, lock);
//    std::thread thread([&cond, &mutex, &pool]()->void {
//      auto visitor = [](test_uobject& obj)->bool { return true; };
//      pool.visit(visitor, true);
//      SCOPED_LOCK(mutex);
//      cond.notify_all();
//    });
//    auto result = cond.wait_for(lock, std::chrono::milliseconds(1000)); // assume thread finishes in 1000ms
//
//    obj.reset();
//
//    if (lock) {
//      lock.unlock();
//    }
//
//    thread.join();
//    ASSERT_EQ(std::cv_status::no_timeout, result); // check only after joining with thread to avoid early exit
//  }
//
//  // test exclusive visitation
//  {
//    iresearch::bounded_object_pool<test_uobject> pool(1);
//    auto obj = pool.emplace(1);
//    std::condition_variable cond;
//    std::mutex mutex;
//    SCOPED_LOCK_NAMED(mutex, lock);
//    std::atomic<bool> visit(false);
//    std::thread thread([&cond, &mutex, &pool, &visit]()->void {
//      auto visitor = [](test_uobject& obj)->bool { return true; };
//      pool.visit(visitor, false);
//      visit = true;
//      SCOPED_LOCK(mutex);
//      cond.notify_all();
//    });
//    auto result = cond.wait_for(lock, std::chrono::milliseconds(1000)); // assume thread finishes in 1000ms
//
//    // MSVC 2015/2017 optimized code seems to sporadically notify condition variables without explicit request
//    MSVC2015_OPTIMIZED_ONLY(while(!visit && result == std::cv_status::no_timeout) result = cond.wait_for(lock, std::chrono::milliseconds(1000)));
//    MSVC2017_ONLY(while(!visit && result == std::cv_status::no_timeout) result = cond.wait_for(lock, std::chrono::milliseconds(1000)));
//
//    obj.reset();
//
//    if (lock) {
//      lock.unlock();
//    }
//
//    thread.join();
//    ASSERT_EQ(std::cv_status::timeout, result);
//    // ^^^ expecting timeout because pool should block indefinitely
//  }
//}

TEST(unbounded_object_pool_tests, construct) {
  iresearch::unbounded_object_pool<test_sobject> pool(42);
  ASSERT_EQ(42, pool.size());
}

TEST(unbounded_object_pool_tests, test_sobject_pool) {
  // create new untracked object on full pool
  {
    std::condition_variable cond;
    std::mutex mutex;
    iresearch::unbounded_object_pool<test_sobject> pool(1);
    auto obj = pool.emplace(1).release();

    {
      SCOPED_LOCK_NAMED(mutex, lock);
      std::thread thread([&cond, &mutex, &pool]()->void{ auto obj = pool.emplace(2); SCOPED_LOCK(mutex); cond.notify_all(); });
      ASSERT_EQ(std::cv_status::no_timeout, cond.wait_for(lock, std::chrono::milliseconds(1000))); // assume threads start within 1000msec
      lock.unlock();
      thread.join();
    }
  }

  // test empty pool
  {
    iresearch::unbounded_object_pool<test_sobject> pool;
    ASSERT_EQ(0, pool.size());
    auto obj = pool.emplace(1);
    ASSERT_TRUE(obj);

    ASSERT_EQ(1, obj->id);
    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    auto obj_shared = pool.emplace(2).release();
    ASSERT_TRUE(bool(obj_shared));
    ASSERT_EQ(2, obj_shared->id);
  }

  // test object reuse
  {
    iresearch::unbounded_object_pool<test_sobject> pool(1);
    auto obj = pool.emplace(1);
    ASSERT_TRUE(obj);
    auto* obj_ptr = obj.get();

    ASSERT_EQ(1, obj->id);
    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    auto obj_shared = pool.emplace(2).release();
    ASSERT_TRUE(bool(obj_shared));
    ASSERT_EQ(1, obj_shared->id);
    ASSERT_EQ(obj_ptr, obj_shared.get());
  }

  // ensure untracked object is not placed back in the pool
  {
    iresearch::unbounded_object_pool<test_sobject> pool(1);
    auto obj0 = pool.emplace(1);
    ASSERT_TRUE(obj0);
    auto obj1 = pool.emplace(2).release();
    ASSERT_TRUE(bool(obj1));
    auto* obj0_ptr = obj0.get();

    ASSERT_EQ(1, obj0->id);
    ASSERT_EQ(2, obj1->id);
    ASSERT_NE(obj0_ptr, obj1.get());
    obj0.reset(); // will be placed back in pool first
    ASSERT_FALSE(obj0);
    ASSERT_EQ(nullptr, obj0.get());
    obj1.reset(); // will push obj1 out of the pool
    ASSERT_FALSE(obj1);
    ASSERT_EQ(nullptr, obj1.get());

    auto obj2 = pool.emplace(3).release();
    ASSERT_TRUE(bool(obj2));
    auto obj3 = pool.emplace(4);
    ASSERT_TRUE(obj3);
    ASSERT_EQ(1, obj2->id);
    ASSERT_EQ(4, obj3->id);
    ASSERT_EQ(obj0_ptr, obj2.get());
    ASSERT_NE(obj0_ptr, obj3.get());
    // obj3 may have been allocated in the same addr as obj1, so can't safely validate
  }

  // test pool clear
  {
    iresearch::unbounded_object_pool<test_sobject> pool(1);
    auto obj = pool.emplace(1);
    ASSERT_TRUE(obj);
    auto* obj_ptr = obj.get();

    ASSERT_EQ(1, obj->id);
    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2);
    ASSERT_TRUE(obj);
    ASSERT_EQ(1, obj->id);
    ASSERT_EQ(obj_ptr, obj.get());

    pool.clear(); // will clear objects inside the pool only
    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2);
    ASSERT_TRUE(obj);
    ASSERT_EQ(1, obj->id);
    ASSERT_EQ(obj_ptr, obj.get()); // same object

    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    pool.clear(); // will clear objects inside the pool only
    obj = pool.emplace(3); // 'obj' should not be reused
    ASSERT_TRUE(obj);
    ASSERT_EQ(3, obj->id);
  }
}

TEST(unbounded_object_pool_tests, test_uobject_pool) {
  // create new untracked object on full pool
  {
    std::condition_variable cond;
    std::mutex mutex;
    iresearch::unbounded_object_pool<test_uobject> pool(1);
    auto obj = pool.emplace(1).release();

    {
      SCOPED_LOCK_NAMED(mutex, lock);
      std::thread thread([&cond, &mutex, &pool]()->void{ auto obj = pool.emplace(2); SCOPED_LOCK(mutex); cond.notify_all(); });
      ASSERT_EQ(std::cv_status::no_timeout, cond.wait_for(lock, std::chrono::milliseconds(1000))); // assume threads start within 1000msec
      lock.unlock();
      thread.join();
    }
  }

  // test object reuse
  {
    iresearch::unbounded_object_pool<test_uobject> pool(1);
    auto obj = pool.emplace(1);
    ASSERT_TRUE(obj);
    auto* obj_ptr = obj.get();

    ASSERT_EQ(1, obj->id);
    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    auto obj_shared = pool.emplace(2).release();
    ASSERT_TRUE(bool(obj_shared));
    ASSERT_EQ(1, obj_shared->id);
    ASSERT_EQ(obj_ptr, obj_shared.get());
  }

  // ensure untracked object is not placed back in the pool
  {
    iresearch::unbounded_object_pool<test_uobject> pool(1);
    auto obj0 = pool.emplace(1);
    ASSERT_TRUE(obj0);
    auto obj1 = pool.emplace(2).release();
    ASSERT_TRUE(bool(obj1));
    auto* obj0_ptr = obj0.get();

    ASSERT_EQ(1, obj0->id);
    ASSERT_EQ(2, obj1->id);
    ASSERT_NE(obj0_ptr, obj1.get());
    obj0.reset(); // will be placed back in pool first
    ASSERT_FALSE(obj0);
    ASSERT_EQ(nullptr, obj0.get());
    obj1.reset(); // will push obj1 out of the pool
    ASSERT_FALSE(obj1);
    ASSERT_EQ(nullptr, obj1.get());

    auto obj2 = pool.emplace(3).release();
    ASSERT_TRUE(bool(obj2));
    auto obj3 = pool.emplace(4);
    ASSERT_TRUE(obj3);
    ASSERT_EQ(1, obj2->id);
    ASSERT_EQ(4, obj3->id);
    ASSERT_EQ(obj0_ptr, obj2.get());
    ASSERT_NE(obj0_ptr, obj3.get());
    // obj3 may have been allocated in the same addr as obj1, so can't safely validate
  }

  // test pool clear
  {
    iresearch::unbounded_object_pool<test_uobject> pool(1);
    auto obj = pool.emplace(1);
    ASSERT_TRUE(obj);
    auto* obj_ptr = obj.get();

    ASSERT_EQ(1, obj->id);
    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2);
    ASSERT_TRUE(obj);
    ASSERT_EQ(1, obj->id);
    ASSERT_EQ(obj_ptr, obj.get());

    pool.clear(); // will clear objects inside the pool only
    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2);
    ASSERT_TRUE(obj);
    ASSERT_EQ(1, obj->id);
    ASSERT_EQ(obj_ptr, obj.get()); // same object

    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    pool.clear(); // will clear objects inside the pool only
    obj = pool.emplace(3); // 'obj' should not be reused
    ASSERT_TRUE(obj);
    ASSERT_EQ(3, obj->id);
  }
}

TEST(unbounded_object_pool_tests, control_object_move) {
  irs::unbounded_object_pool<test_sobject> pool(2);
  ASSERT_EQ(2, pool.size());

  // move constructor
  {
    auto moved = pool.emplace(1);
    ASSERT_TRUE(moved);
    ASSERT_NE(nullptr, moved.get());
    ASSERT_EQ(1, moved->id);

    auto obj = std::move(moved);
    ASSERT_FALSE(moved);
    ASSERT_EQ(nullptr, moved.get());
    ASSERT_TRUE(obj);
    ASSERT_EQ(1, obj->id);
  }

  // move assignment
  {
    auto moved = pool.emplace(1);
    ASSERT_TRUE(moved);
    ASSERT_NE(nullptr, moved.get());
    ASSERT_EQ(1, moved->id);
    auto* moved_ptr = moved.get();

    auto obj = pool.emplace(2);
    ASSERT_TRUE(obj);
    ASSERT_EQ(2, obj->id);

    obj = std::move(moved);
    ASSERT_TRUE(obj);
    ASSERT_FALSE(moved);
    ASSERT_EQ(nullptr, moved.get());
    ASSERT_EQ(obj.get(), moved_ptr);
    ASSERT_EQ(1, obj->id);
  }
}

TEST(unbounded_object_pool_volatile_tests, construct) {
  iresearch::unbounded_object_pool_volatile<test_sobject> pool(42);
  ASSERT_EQ(42, pool.size());
  ASSERT_EQ(0, pool.generation_size());
}

TEST(unbounded_object_pool_volatile_tests, move) {
  irs::unbounded_object_pool_volatile<test_sobject> moved(2);
  ASSERT_EQ(2, moved.size());
  ASSERT_EQ(0, moved.generation_size());

  auto obj0 = moved.emplace(1);
  ASSERT_EQ(1, moved.generation_size());
  ASSERT_TRUE(obj0);
  ASSERT_NE(nullptr, obj0.get());
  ASSERT_EQ(1, obj0->id);

  irs::unbounded_object_pool_volatile<test_sobject> pool(std::move(moved));
  ASSERT_EQ(2, moved.generation_size());
  ASSERT_EQ(2, pool.generation_size());

  auto obj1 = pool.emplace(2);
  ASSERT_EQ(3, pool.generation_size()); // +1 for moved
  ASSERT_EQ(3, moved.generation_size());
  ASSERT_TRUE(obj1);
  ASSERT_NE(nullptr, obj1.get());
  ASSERT_EQ(2, obj1->id);

  // insert via moved pool
  auto obj2 = moved.emplace(3);
  ASSERT_EQ(4, pool.generation_size());
  ASSERT_EQ(4, moved.generation_size());
  ASSERT_TRUE(obj2);
  ASSERT_NE(nullptr, obj2.get());
  ASSERT_EQ(3, obj2->id);
}

TEST(unbounded_object_pool_volatile_tests, control_object_move) {
  irs::unbounded_object_pool_volatile<test_sobject> pool(2);
  ASSERT_EQ(2, pool.size());
  ASSERT_EQ(0, pool.generation_size());

  // move constructor
  {
    auto moved = pool.emplace(1);
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_TRUE(moved);
    ASSERT_NE(nullptr, moved.get());
    ASSERT_EQ(1, moved->id);

    auto obj = std::move(moved);
    ASSERT_FALSE(moved);
    ASSERT_EQ(nullptr, moved.get());
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_TRUE(obj);
    ASSERT_EQ(1, obj->id);
  }

  // move assignment
  {
    auto moved = pool.emplace(1);
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_TRUE(moved);
    ASSERT_NE(nullptr, moved.get());
    ASSERT_EQ(1, moved->id);
    auto* moved_ptr = moved.get();

    auto obj = pool.emplace(2);
    ASSERT_EQ(2, pool.generation_size());
    ASSERT_TRUE(obj);
    ASSERT_EQ(2, obj->id);

    obj = std::move(moved);
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_TRUE(obj);
    ASSERT_FALSE(moved);
    ASSERT_EQ(nullptr, moved.get());
    ASSERT_EQ(obj.get(), moved_ptr);
    ASSERT_EQ(1, obj->id);
  }

  ASSERT_EQ(0, pool.generation_size());
}

TEST(unbounded_object_pool_volatile_tests, test_sobject_pool) {
  // create new untracked object on full pool
  {
    std::condition_variable cond;
    std::mutex mutex;
    iresearch::unbounded_object_pool_volatile<test_sobject> pool(1);
    ASSERT_EQ(0, pool.generation_size());
    auto obj = pool.emplace(1).release();
    ASSERT_EQ(1, pool.generation_size());

    {
      SCOPED_LOCK_NAMED(mutex, lock);
      std::thread thread([&cond, &mutex, &pool]()->void{ auto obj = pool.emplace(2); SCOPED_LOCK(mutex); cond.notify_all(); });
      ASSERT_EQ(std::cv_status::no_timeout, cond.wait_for(lock, std::chrono::milliseconds(1000))); // assume threads start within 1000msec
      lock.unlock();
      thread.join();
    }

    ASSERT_EQ(1, pool.generation_size());
  }

  // test empty pool
  {
    iresearch::unbounded_object_pool_volatile<test_sobject> pool;
    ASSERT_EQ(0, pool.size());
    auto obj = pool.emplace(1);
    ASSERT_TRUE(obj);

    ASSERT_EQ(1, obj->id);
    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    auto obj_shared = pool.emplace(2).release();
    ASSERT_TRUE(bool(obj_shared));
    ASSERT_EQ(2, obj_shared->id);
  }

  // test object reuse
  {
    iresearch::unbounded_object_pool_volatile<test_sobject> pool(1);
    ASSERT_EQ(0, pool.generation_size());
    auto obj = pool.emplace(1);
    ASSERT_EQ(1, pool.generation_size());
    auto* obj_ptr = obj.get();

    ASSERT_EQ(1, obj->id);
    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    ASSERT_EQ(0, pool.generation_size());
    auto obj_shared = pool.emplace(2).release();
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_EQ(1, obj_shared->id);
    ASSERT_EQ(obj_ptr, obj_shared.get());
  }

  // ensure untracked object is not placed back in the pool
  {
    iresearch::unbounded_object_pool_volatile<test_sobject> pool(1);
    ASSERT_EQ(0, pool.generation_size());
    auto obj0 = pool.emplace(1);
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_TRUE(obj0);
    auto obj1 = pool.emplace(2).release();
    ASSERT_EQ(2, pool.generation_size());
    ASSERT_TRUE(bool(obj1));
    auto* obj0_ptr = obj0.get();

    ASSERT_EQ(1, obj0->id);
    ASSERT_EQ(2, obj1->id);
    ASSERT_NE(obj0_ptr, obj1.get());
    obj0.reset(); // will be placed back in pool first
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_FALSE(obj0);
    ASSERT_EQ(nullptr, obj0.get());
    obj1.reset(); // will push obj1 out of the pool
    ASSERT_EQ(0, pool.generation_size());
    ASSERT_FALSE(obj1);
    ASSERT_EQ(nullptr, obj1.get());

    auto obj2 = pool.emplace(3).release();
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_TRUE(bool(obj2));
    auto obj3 = pool.emplace(4);
    ASSERT_EQ(2, pool.generation_size());
    ASSERT_TRUE(obj3);
    ASSERT_EQ(1, obj2->id);
    ASSERT_EQ(4, obj3->id);
    ASSERT_EQ(obj0_ptr, obj2.get());
    ASSERT_NE(obj0_ptr, obj3.get());
    // obj3 may have been allocated in the same addr as obj1, so can't safely validate
  }

  // test pool clear
  {
    iresearch::unbounded_object_pool_volatile<test_sobject> pool(1);
    ASSERT_EQ(0, pool.generation_size());
    auto obj_noreuse = pool.emplace(-1);
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_TRUE(obj_noreuse);
    auto obj = pool.emplace(1);
    ASSERT_EQ(2, pool.generation_size());
    ASSERT_TRUE(obj);
    auto* obj_ptr = obj.get();

    ASSERT_EQ(1, obj->id);
    obj.reset();
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2);
    ASSERT_EQ(2, pool.generation_size());
    ASSERT_TRUE(obj);
    ASSERT_EQ(1, obj->id);
    ASSERT_EQ(obj_ptr, obj.get());

    pool.clear(); // clear existing in a pool
    ASSERT_EQ(2, pool.generation_size());
    obj.reset();
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2); // may return same memory address as obj_ptr, but constructor would have been called
    ASSERT_EQ(2, pool.generation_size());
    ASSERT_TRUE(obj);
    ASSERT_EQ(1, obj->id);
    ASSERT_EQ(obj_ptr, obj.get());

    pool.clear(true); // clear existing in a pool and prevent external object from returning back
    ASSERT_EQ(0, pool.generation_size());
    obj.reset();
    ASSERT_EQ(0, pool.generation_size());
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2); // may return same memory address as obj_ptr, but constructor would have been called
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_TRUE(obj);
    ASSERT_EQ(2, obj->id);

    obj_noreuse.reset(); // reset value from previuos generation
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_FALSE(obj_noreuse);
    ASSERT_EQ(nullptr, obj_noreuse.get());
    obj = pool.emplace(3); // 'obj_noreuse' should not be reused
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_EQ(3, obj->id);
  }
}

TEST(unbounded_object_pool_volatile_tests, return_object_after_pool_destroyed) {
  auto pool = irs::memory::make_unique<irs::unbounded_object_pool_volatile<test_sobject>>(1);
  ASSERT_EQ(0, pool->generation_size());
  ASSERT_NE(nullptr, pool);

  auto obj = pool->emplace(42);
  ASSERT_EQ(1, pool->generation_size());
  ASSERT_TRUE(obj);
  ASSERT_EQ(42, obj->id);
  auto objShared = pool->emplace(442).release();
  ASSERT_EQ(2, pool->generation_size());
  ASSERT_NE(nullptr, objShared);
  ASSERT_EQ(442, objShared->id);

  // destroy pool
  pool.reset();
  ASSERT_EQ(nullptr, pool);

  // ensure objects are still there
  ASSERT_EQ(42, obj->id);
  ASSERT_EQ(442, objShared->id);
}

TEST(unbounded_object_pool_volatile_tests, test_uobject_pool) {
  // create new untracked object on full pool
  {
    std::condition_variable cond;
    std::mutex mutex;
    iresearch::unbounded_object_pool_volatile<test_uobject> pool(1);
    auto obj = pool.emplace(1);

    {
      SCOPED_LOCK_NAMED(mutex, lock);
      std::thread thread([&cond, &mutex, &pool]()->void{ auto obj = pool.emplace(2); SCOPED_LOCK(mutex); cond.notify_all(); });
      ASSERT_EQ(std::cv_status::no_timeout, cond.wait_for(lock, std::chrono::milliseconds(1000))); // assume threads start within 1000msec
      lock.unlock();
      thread.join();
    }
  }

  // test object reuse
  {
    iresearch::unbounded_object_pool_volatile<test_uobject> pool(1);
    auto obj = pool.emplace(1);
    auto* obj_ptr = obj.get();

    ASSERT_EQ(1, obj->id);
    obj.reset();
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2);
    ASSERT_EQ(1, obj->id);
    ASSERT_EQ(obj_ptr, obj.get());
  }

  // ensure untracked object is not placed back in the pool
  {
    iresearch::unbounded_object_pool_volatile<test_uobject> pool(1);
    auto obj0 = pool.emplace(1);
    auto obj1 = pool.emplace(2);
    auto* obj0_ptr = obj0.get();
    auto* obj1_ptr = obj1.get();

    ASSERT_EQ(1, obj0->id);
    ASSERT_EQ(2, obj1->id);
    ASSERT_NE(obj0_ptr, obj1.get());
    obj1.reset(); // will be placed back in pool first
    ASSERT_FALSE(obj1);
    ASSERT_EQ(nullptr, obj1.get());
    obj0.reset(); // will push obj1 out of the pool
    ASSERT_FALSE(obj0);
    ASSERT_EQ(nullptr, obj0.get());

    auto obj2 = pool.emplace(3);
    auto obj3 = pool.emplace(4);
    ASSERT_EQ(2, obj2->id);
    ASSERT_EQ(4, obj3->id);
    ASSERT_EQ(obj1_ptr, obj2.get());
    ASSERT_NE(obj1_ptr, obj3.get());
    // obj3 may have been allocated in the same addr as obj1, so can't safely validate
  }

  // test pool clear
  {
    iresearch::unbounded_object_pool_volatile<test_uobject> pool(1);
    ASSERT_EQ(0, pool.generation_size());
    auto obj_noreuse = pool.emplace(-1);
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_TRUE(obj_noreuse);
    auto obj = pool.emplace(1);
    ASSERT_EQ(2, pool.generation_size());
    ASSERT_TRUE(obj);
    auto* obj_ptr = obj.get();

    ASSERT_EQ(1, obj->id);
    obj.reset();
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2);
    ASSERT_EQ(2, pool.generation_size());
    ASSERT_TRUE(obj);
    ASSERT_EQ(1, obj->id);
    ASSERT_EQ(obj_ptr, obj.get());

    pool.clear(); // clear existing in a pool
    ASSERT_EQ(2, pool.generation_size());
    obj.reset();
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2); // may return same memory address as obj_ptr, but constructor would have been called
    ASSERT_EQ(2, pool.generation_size());
    ASSERT_TRUE(obj);
    ASSERT_EQ(1, obj->id);
    ASSERT_EQ(obj_ptr, obj.get());

    pool.clear(true); // clear existing in a pool and prevent external object from returning back
    ASSERT_EQ(0, pool.generation_size());
    obj.reset();
    ASSERT_EQ(0, pool.generation_size());
    ASSERT_FALSE(obj);
    ASSERT_EQ(nullptr, obj.get());
    obj = pool.emplace(2); // may return same memory address as obj_ptr, but constructor would have been called
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_TRUE(obj);
    ASSERT_EQ(2, obj->id);

    obj_noreuse.reset(); // reset value from previuos generation
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_FALSE(obj_noreuse);
    ASSERT_EQ(nullptr, obj_noreuse.get());
    obj = pool.emplace(3); // 'obj_noreuse' should not be reused
    ASSERT_EQ(1, pool.generation_size());
    ASSERT_EQ(3, obj->id);
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
