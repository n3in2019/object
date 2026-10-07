#include "object/object.h"
#include "object/object_factory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <typeindex>
#include <vector>

// second_tu.cpp owns the Widget class; this translation unit never sees its
// definition, so any registration proves auto-registration works across TUs.
namespace second_tu {
std::string_view widget_type_name();
std::string_view widget_tag_name();
}  // namespace second_tu

namespace tf {
namespace tag {
struct Shape {};
struct TwoD {};
}  // namespace tag

class Circle : public obj::Object<Circle, tag::Shape, tag::TwoD> {
 public:
  int radius = 7;
};

namespace nested {
class Square : public obj::Object<Square, tag::Shape> {
 public:
  int side = 4;
};
}  // namespace nested

class Blob : public obj::Object<Blob> {};

class Ellipse : public Circle {};

// Test-controlled construction window; timeouts only bound failed tests.
struct ConstructionGate {
  std::mutex mutex;
  std::condition_variable changed;
  int entered = 0;
  bool released = false;

  void enter() {
    std::unique_lock<std::mutex> lock(mutex);
    ++entered;
    changed.notify_all();
    if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return released; })) {
      throw std::runtime_error("construction gate timed out");
    }
  }

  bool wait_for_entries(int count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, [&] { return entered >= count; });
  }

  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};

class ColdSingleton : public obj::Object<ColdSingleton> {
 public:
  static inline std::atomic<int> constructions{0};
  static inline ConstructionGate *gate = nullptr;
  ColdSingleton() {
    ++constructions;
    // Registry access, ordinary construction and a different singleton must all
    // be safe inside the user constructor.
    (void)obj::ObjectFactory::list_types();
    (void)obj::ObjectFactory::create_shared(nested::Square::_type());
    (void)obj::ObjectFactory::get_single_instance<Blob>();
    gate->enter();
  }
};

class OverriddenSingleton : public obj::Object<OverriddenSingleton> {
 public:
  static inline ConstructionGate *gate = nullptr;
  OverriddenSingleton() { gate->enter(); }
  explicit OverriddenSingleton(int) {}  // replacement bypasses the blocked factory
};

class RetriedSingleton : public obj::Object<RetriedSingleton> {
 public:
  static inline std::atomic<int> attempts{0};
  RetriedSingleton() {
    if (++attempts == 1) throw std::runtime_error("first attempt fails");
  }
};

class RecursiveSingleton : public obj::Object<RecursiveSingleton> {
 public:
  RecursiveSingleton() { (void)obj::ObjectFactory::get_single_instance<RecursiveSingleton>(); }
};

class FailedColdSingleton : public obj::Object<FailedColdSingleton> {
 public:
  static inline ConstructionGate *gate = nullptr;
  static inline std::atomic<int> attempts{0};
  static inline bool fail = true;
  FailedColdSingleton() {
    ++attempts;
    if (fail) {
      gate->enter();
      throw std::runtime_error("controlled construction failure");
    }
  }
};

static_assert(obj::is_tagged_v<Circle, tag::Shape>);
static_assert(obj::is_tagged_v<Circle, tag::TwoD>);
static_assert(!obj::is_tagged_v<nested::Square, tag::TwoD>);
static_assert(!obj::is_tagged_v<Blob, tag::Shape>);
static_assert(obj::is_tagged_v<Ellipse, tag::Shape>);  // markers are inherited
static_assert(Circle::_tags.size() == 2);
static_assert(Blob::_tags.size() == 0);

}  // namespace tf

namespace {

bool contains(const std::vector<std::string_view> &names, std::string_view name) { return std::find(names.begin(), names.end(), name) != names.end(); }

}  // namespace

TEST(Factory, AutoRegistersDerivedClasses) {
  const auto names = obj::ObjectFactory::list_types();
  EXPECT_TRUE(contains(names, tf::Circle::_type()));
  EXPECT_TRUE(contains(names, tf::nested::Square::_type()));
  EXPECT_TRUE(contains(names, tf::Blob::_type()));
  EXPECT_TRUE(contains(names, second_tu::widget_type_name()));
}

TEST(Factory, TypeNameIsQualifiedAndStable) {
  EXPECT_NE(tf::Circle::_type().find("Circle"), std::string_view::npos);
  EXPECT_NE(tf::Circle::_type().find("tf"), std::string_view::npos);
  EXPECT_NE(tf::Circle::_type(), tf::nested::Square::_type());
  EXPECT_EQ(tf::Circle::_type(), obj::tag_name<tf::Circle>());
}

TEST(Factory, CreateReturnsCorrectDynamicType) {
  // __type_index() through the base pointer is virtual dispatch: it must report
  // the most-derived type the factory actually constructed.
  auto circle = obj::ObjectFactory::create(tf::Circle::_type());
  ASSERT_NE(circle, nullptr);
  EXPECT_EQ(circle->__type_index(), std::type_index(typeid(tf::Circle)));

  auto square = obj::ObjectFactory::create_shared(tf::nested::Square::_type());
  ASSERT_NE(square, nullptr);
  EXPECT_EQ(square->__type_index(), std::type_index(typeid(tf::nested::Square)));
}

TEST(Factory, UnknownTypeThrows) {
  EXPECT_THROW(obj::ObjectFactory::create("definitely::not::registered"), std::out_of_range);
  EXPECT_THROW(obj::ObjectFactory::create_shared("definitely::not::registered"), std::out_of_range);
  EXPECT_THROW(obj::ObjectFactory::tags_of("definitely::not::registered"), std::out_of_range);
}

TEST(Factory, DuplicateRegistrationKeepsFirst) {
  obj::TypeInfo probe;
  probe.name = "tf::probe";
  probe.create = []() -> std::unique_ptr<obj::ObjectBase> { return std::make_unique<tf::Blob>(); };
  probe.create_shared = nullptr;  // exercises the wrap-create fallback on create_shared
  probe.type_index = typeid(tf::Blob);

  EXPECT_TRUE(obj::ObjectFactory::register_type(probe.name, probe));
  EXPECT_FALSE(obj::ObjectFactory::register_type(probe.name, probe));
  EXPECT_EQ(obj::ObjectFactory::create("tf::probe")->__type_index(), std::type_index(typeid(tf::Blob)));
  EXPECT_EQ(obj::ObjectFactory::create_shared("tf::probe")->__type_index(), std::type_index(typeid(tf::Blob)));

  obj::TypeInfo null_factory;  // create left null must be rejected at registration
  null_factory.name = "tf::null";
  EXPECT_THROW(obj::ObjectFactory::register_type("tf::null", null_factory), std::invalid_argument);
}

TEST(Tags, TypedAndStringQueriesAgree) {
  const auto typed = obj::ObjectFactory::list_types<tf::tag::Shape>();
  const auto by_string = obj::ObjectFactory::list_types(obj::tag_name<tf::tag::Shape>());
  EXPECT_EQ(typed, by_string);

  ASSERT_EQ(typed.size(), 2u);
  EXPECT_TRUE(contains(typed, tf::Circle::_type()));
  EXPECT_TRUE(contains(typed, tf::nested::Square::_type()));

  const auto two_d = obj::ObjectFactory::list_types<tf::tag::TwoD>();
  ASSERT_EQ(two_d.size(), 1u);
  EXPECT_EQ(two_d[0], tf::Circle::_type());
}

TEST(Tags, MembershipAndMetadata) {
  EXPECT_TRUE(obj::ObjectFactory::has_tag(tf::Circle::_type(), obj::tag_name<tf::tag::Shape>()));
  EXPECT_TRUE(obj::ObjectFactory::has_tag(tf::Circle::_type(), obj::tag_name<tf::tag::TwoD>()));
  EXPECT_FALSE(obj::ObjectFactory::has_tag(tf::nested::Square::_type(), obj::tag_name<tf::tag::TwoD>()));
  EXPECT_FALSE(obj::ObjectFactory::has_tag("definitely::not::registered", obj::tag_name<tf::tag::Shape>()));

  const auto tags = obj::ObjectFactory::tags_of(tf::Circle::_type());  // declaration order preserved
  ASSERT_EQ(tags.size(), 2u);
  EXPECT_EQ(tags[0], obj::tag_name<tf::tag::Shape>());
  EXPECT_EQ(tags[1], obj::tag_name<tf::tag::TwoD>());

  EXPECT_TRUE(obj::ObjectFactory::tags_of(tf::Blob::_type()).empty());
  EXPECT_EQ(tf::Circle::_tags[0], obj::tag_name<tf::tag::Shape>());
  EXPECT_TRUE(obj::ObjectFactory::list_types("no::such::tag").empty());
}

TEST(Tags, CreateAllInstantiatesEveryMatch) {
  auto objects = obj::ObjectFactory::create_all<tf::tag::Shape>();
  ASSERT_EQ(objects.size(), 2u);
  for (const auto &object : objects) {
    ASSERT_NE(object, nullptr);
    const auto index = object->__type_index();
    EXPECT_TRUE(index == std::type_index(typeid(tf::Circle)) || index == std::type_index(typeid(tf::nested::Square)));
  }

  auto shared = obj::ObjectFactory::create_shared_all<tf::tag::TwoD>();
  ASSERT_EQ(shared.size(), 1u);
  EXPECT_EQ(shared[0]->__type_index(), std::type_index(typeid(tf::Circle)));

  EXPECT_TRUE(obj::ObjectFactory::create_all("no::such::tag").empty());
}

TEST(Factory, RegistersAcrossTranslationUnits) {
  const auto name = second_tu::widget_type_name();
  EXPECT_TRUE(contains(obj::ObjectFactory::list_types(), name));
  EXPECT_TRUE(obj::ObjectFactory::has_tag(name, second_tu::widget_tag_name()));

  const auto objects = obj::ObjectFactory::create_all(second_tu::widget_tag_name());
  ASSERT_EQ(objects.size(), 1u);
  EXPECT_NE(objects[0], nullptr);
}

TEST(Singleton, LazyCachedPerName) {
  const auto first = obj::ObjectFactory::get_single_instance(tf::Circle::_type());
  const auto second = obj::ObjectFactory::get_single_instance(tf::Circle::_type());
  EXPECT_EQ(first.get(), second.get());
  EXPECT_EQ(first->__type_index(), std::type_index(typeid(tf::Circle)));

  const auto typed = obj::ObjectFactory::get_single_instance<tf::Circle>();
  EXPECT_EQ(typed.get(), first.get());
  EXPECT_EQ(typed->radius, 7);

  EXPECT_THROW(obj::ObjectFactory::get_single_instance("definitely::not::registered"), std::out_of_range);
}

TEST(Singleton, SettableAndTypeChecked) {
  auto replacement = std::make_shared<tf::Circle>();
  replacement->radius = 99;
  obj::ObjectFactory::set_single_instance(replacement);

  const auto current = obj::ObjectFactory::get_single_instance<tf::Circle>();
  EXPECT_EQ(current.get(), replacement.get());
  EXPECT_EQ(current->radius, 99);

  struct Ghost : obj::ObjectBase {  // registered nowhere
    std::type_index __type_index() const override { return std::type_index(typeid(Ghost)); }
  };
  EXPECT_THROW(obj::ObjectFactory::set_single_instance(std::make_shared<Ghost>()), std::out_of_range);

  obj::ObjectFactory::set_single_instance(std::make_shared<tf::Circle>());  // restore for other tests
}

TEST(Singleton, ConcurrentColdStartConstructsExactlyOnce) {
  constexpr int callers = 16;
  tf::ConstructionGate construction;
  tf::ColdSingleton::gate = &construction;
  std::mutex start_mutex;
  std::condition_variable start_changed;
  int ready = 0;
  bool start = false;
  std::atomic<int> failures{0};
  std::vector<std::shared_ptr<tf::ColdSingleton>> results(callers);
  std::vector<std::thread> threads;
  for (int i = 0; i < callers; ++i) {
    threads.emplace_back([&, i] {
      {
        std::unique_lock<std::mutex> lock(start_mutex);
        ++ready;
        start_changed.notify_all();
        start_changed.wait(lock, [&] { return start; });
      }
      try {
        results[i] = obj::ObjectFactory::get_single_instance<tf::ColdSingleton>();
      } catch (...) {
        ++failures;
      }
    });
  }
  {
    std::unique_lock<std::mutex> lock(start_mutex);
    start_changed.wait(lock, [&] { return ready == callers; });
    start = true;
    start_changed.notify_all();
  }
  EXPECT_TRUE(construction.wait_for_entries(1, std::chrono::seconds(5)));
  // Keep the first constructor blocked while the simultaneous callers compete.
  // A second entry is positive evidence of the old bug, not a sleep-based order.
  EXPECT_FALSE(construction.wait_for_entries(2, std::chrono::milliseconds(100)));
  construction.release();
  for (auto &thread : threads) thread.join();
  tf::ColdSingleton::gate = nullptr;
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(tf::ColdSingleton::constructions.load(), 1);
  ASSERT_NE(results[0], nullptr);
  for (const auto &result : results) EXPECT_EQ(result.get(), results[0].get());
}

TEST(Singleton, SetterWinsDuringColdStart) {
  tf::ConstructionGate construction;
  tf::OverriddenSingleton::gate = &construction;
  std::shared_ptr<tf::OverriddenSingleton> result;
  std::atomic<int> failures{0};
  std::thread creator([&] {
    try {
      result = obj::ObjectFactory::get_single_instance<tf::OverriddenSingleton>();
    } catch (...) {
      ++failures;
    }
  });
  EXPECT_TRUE(construction.wait_for_entries(1, std::chrono::seconds(5)));
  auto waiting = std::async(std::launch::async, [] {
    return obj::ObjectFactory::get_single_instance<tf::OverriddenSingleton>();
  });
  EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  auto replacement = std::make_shared<tf::OverriddenSingleton>(0);
  obj::ObjectFactory::set_single_instance(replacement);
  // An already waiting reader must wake even while the constructor stays blocked.
  const auto waiter_status = waiting.wait_for(std::chrono::seconds(2));
  // The setter and readers need not wait for the old construction to complete.
  EXPECT_EQ(obj::ObjectFactory::get_single_instance<tf::OverriddenSingleton>().get(), replacement.get());
  construction.release();
  creator.join();
  EXPECT_EQ(waiter_status, std::future_status::ready);
  EXPECT_EQ(waiting.get().get(), replacement.get());
  tf::OverriddenSingleton::gate = nullptr;
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(result.get(), replacement.get());
  EXPECT_EQ(obj::ObjectFactory::get_single_instance<tf::OverriddenSingleton>().get(), replacement.get());
}

TEST(Singleton, FailedConstructionCanBeRetried) {
  EXPECT_THROW(obj::ObjectFactory::get_single_instance<tf::RetriedSingleton>(), std::runtime_error);
  auto result = obj::ObjectFactory::get_single_instance<tf::RetriedSingleton>();
  EXPECT_EQ(tf::RetriedSingleton::attempts.load(), 2);
  EXPECT_EQ(obj::ObjectFactory::get_single_instance<tf::RetriedSingleton>().get(), result.get());
}

TEST(Singleton, FailedColdStartWakesWaitersAndAllowsRetry) {
  tf::ConstructionGate construction;
  tf::FailedColdSingleton::gate = &construction;
  auto request = [] {
    try {
      (void)obj::ObjectFactory::get_single_instance<tf::FailedColdSingleton>();
      return false;
    } catch (const std::runtime_error &error) {
      return std::string(error.what()) == "controlled construction failure";
    }
  };
  auto creator = std::async(std::launch::async, request);
  EXPECT_TRUE(construction.wait_for_entries(1, std::chrono::seconds(5)));
  auto waiting = std::async(std::launch::async, request);
  EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  construction.release();
  EXPECT_TRUE(creator.get());
  EXPECT_TRUE(waiting.get());
  EXPECT_EQ(tf::FailedColdSingleton::attempts.load(), 1);
  tf::FailedColdSingleton::fail = false;
  auto result = obj::ObjectFactory::get_single_instance<tf::FailedColdSingleton>();
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(tf::FailedColdSingleton::attempts.load(), 2);
  tf::FailedColdSingleton::gate = nullptr;
}

TEST(Singleton, RecursiveInitializationThrowsWithoutStrandingState) {
  EXPECT_THROW(obj::ObjectFactory::get_single_instance<tf::RecursiveSingleton>(), std::logic_error);
  EXPECT_THROW(obj::ObjectFactory::get_single_instance<tf::RecursiveSingleton>(), std::logic_error);
}

TEST(Factory, ConcurrentAccessIsSafe) {
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&failures] {
      for (int i = 0; i < 200; ++i) {
        try {
          auto object = obj::ObjectFactory::create(tf::Circle::_type());
          auto shared = obj::ObjectFactory::create_shared(tf::nested::Square::_type());
          auto singleton = obj::ObjectFactory::get_single_instance<tf::Circle>();
          const auto tagged = obj::ObjectFactory::list_types<tf::tag::TwoD>();
          if (object == nullptr || shared == nullptr || singleton == nullptr || tagged.size() != 1u) {
            ++failures;
          }
        } catch (...) {
          ++failures;
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(failures.load(), 0);
}
