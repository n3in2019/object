#include "object/object.h"
#include "object/object_factory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
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
