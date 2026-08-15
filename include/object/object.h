#pragma once

#include "object/object_base.h"
#include "object/object_factory.h"

#include <array>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

// Keeps the registration flag of every Object<T> specialization alive even when the
// linker never references it, so auto-registration survives archive/static-lib use.
#if !defined(OBJ_USED)
#if defined(__GNUC__) || defined(__clang__)
#define OBJ_USED [[gnu::used]]
#else
#define OBJ_USED
#endif
#endif

namespace obj {

// Empty marker base injected per tag. Object<Derived, Tags...> inherits one
// TagMarker<Tag> per tag, which is what is_tagged_v inspects at compile time.
template <typename Tag>
struct TagMarker {};

// True when T's Object<> bases include TagMarker<Tag>. T must be a complete type;
// tag markers are inherited, so subclasses of a tagged class are tagged as well.
template <typename T, typename Tag>
inline constexpr bool is_tagged_v = std::is_base_of_v<TagMarker<Tag>, T>;

namespace detail {

template <typename Derived, typename... Tags>
bool auto_register_once() {
  static_assert(std::is_default_constructible_v<Derived>, "Object<T> requires T to be default constructible");
  const std::string_view name = qualified_name_impl<Derived>();
  TypeInfo info;
  info.name = name;
  info.create = []() -> std::unique_ptr<ObjectBase> { return std::make_unique<Derived>(); };
  info.create_shared = []() -> std::shared_ptr<ObjectBase> { return std::make_shared<Derived>(); };
  info.tags = std::vector<std::string_view>{qualified_name_impl<Tags>()...};
  info.type_index = typeid(Derived);
  ObjectFactory::register_type(name, info);
  return true;
}

}  // namespace detail

// CRTP base for auto-registered classes. Merely deriving registers the class (under
// its fully qualified name) together with its tags at static initialization time.
// Tags are empty structs; duplicate tag types in one class are a compile error.
// Note: the class registered is Derived itself — subclasses that don't derive their
// own Object<> specialization are not registered separately.
template <typename Derived, typename... Tags>
struct Object : ObjectBase, private TagMarker<Tags>... {
 public:
  // (void)registered_ in every member below is an odr-use: instantiating any of these
  // bodies in a translation unit forces that unit to also emit the initializer of
  // registered_, whose dynamic initialization (running before main) performs the
  // auto-registration. Neither defining the class nor the used attribute alone forces
  // initializer instantiation on clang; odr-use does, on every compiler.
  static std::string_view _type() {
    (void)registered_;
    return detail::qualified_name_impl<Derived>();
  }

  std::type_index __type_index() const override {
    (void)registered_;
    return std::type_index(typeid(Derived));
  }

  static constexpr std::array<std::string_view, sizeof...(Tags)> _tags{detail::qualified_name_impl<Tags>()...};

 protected:
  Object() { (void)registered_; }

  ~Object() override { (void)registered_; }

 private:
  OBJ_USED static inline const bool registered_ = detail::auto_register_once<Derived, Tags...>();
};

}  // namespace obj
