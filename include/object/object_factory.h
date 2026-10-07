#pragma once

#include "object/object_base.h"
#include "object/type_info.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace obj {
namespace detail {

// Extracts the fully qualified type name from the compiler-specific spelling of the
// enclosing function signature. Works entirely at compile time: no demangler, no
// allocation, and the result lives in static storage for the whole program.
inline constexpr std::string_view extract_type_name(std::string_view pretty_function) {
#if defined(__clang__)
  const auto left = pretty_function.find("T = ");
  if (left == std::string_view::npos) return {};
  const auto start = left + 4;
  const auto right = pretty_function.find(']', start);
  if (right == std::string_view::npos) return {};
  return pretty_function.substr(start, right - start);
#elif defined(__GNUC__)
  const auto left = pretty_function.find("T = ");
  if (left == std::string_view::npos) return {};
  const auto start = left + 4;
  auto right = pretty_function.find(';', start);
  if (right == std::string_view::npos) {
    right = pretty_function.find(']', start);
  }
  if (right == std::string_view::npos) return {};
  return pretty_function.substr(start, right - start);
#elif defined(_MSC_VER)
  constexpr std::string_view needle = "qualified_name_impl<";
  const auto left = pretty_function.find(needle);
  if (left == std::string_view::npos) return {};
  const auto start = left + needle.size();
  const auto right = pretty_function.find('>', start);
  if (right == std::string_view::npos) return {};
  return pretty_function.substr(start, right - start);
#else
  return {};
#endif
}

template <typename T>
constexpr std::string_view qualified_name_impl() {
#if defined(_MSC_VER)
  return extract_type_name(__FUNCSIG__);
#else
  return extract_type_name(__PRETTY_FUNCTION__);
#endif
}

}  // namespace detail

// Compile-time fully qualified name of any type. For an Object-derived class this is
// identical to what Object<T>::_type() reports and what the factory is keyed on.
template <typename T>
constexpr std::string_view tag_name() {
  return detail::qualified_name_impl<T>();
}

// Central object factory: name-keyed registry of TypeInfo records plus lazy
// singletons. Deriving from Object<T, Tags...> registers a class automatically at
// static initialization; register_type() is available for manual registrations.
// Thread safe: registration normally happens during static initialization, but every
// entry point guards the maps, so runtime registration and concurrent lookups are
// also safe. Unknown names throw std::out_of_range; predicates return false instead.
class ObjectFactory {
 public:
  static bool register_type(std::string_view name, TypeInfo info) {
    if (info.create == nullptr) {
      throw std::invalid_argument("register_type: null factory for " + std::string(name));
    }
    auto &self = instance();
    std::lock_guard<std::mutex> lock(self.mutex_);
    const bool inserted = self.types_.emplace(std::string(name), std::move(info)).second;
    if (!inserted) {
      std::cerr << "[obj] duplicate type registration ignored: " << name << std::endl;
    }
    return inserted;
  }

  static std::unique_ptr<ObjectBase> create(std::string_view name) {
    const TypeInfo::Factory factory = instance().factory_of(name);
    return factory();
  }

  static std::shared_ptr<ObjectBase> create_shared(std::string_view name) {
    const TypeInfo info = instance().info_of(name);
    if (info.create_shared != nullptr) {
      return info.create_shared();
    }
    return std::shared_ptr<ObjectBase>(info.create());
  }

  static std::vector<std::string_view> list_types() {
    auto &self = instance();
    std::lock_guard<std::mutex> lock(self.mutex_);
    std::vector<std::string_view> names;
    names.reserve(self.types_.size());
    for (const auto &entry : self.types_) {
      names.push_back(entry.first);
    }
    return names;
  }

  static std::vector<std::string_view> list_types(std::string_view tag) {
    auto &self = instance();
    std::lock_guard<std::mutex> lock(self.mutex_);
    std::vector<std::string_view> names;
    for (const auto &entry : self.types_) {
      if (tagged(entry.second, tag)) {
        names.push_back(entry.first);
      }
    }
    return names;
  }

  template <typename Tag>
  static std::vector<std::string_view> list_types() {
    return list_types(tag_name<Tag>());
  }

  static bool has_tag(std::string_view type, std::string_view tag) {
    auto &self = instance();
    std::lock_guard<std::mutex> lock(self.mutex_);
    const auto it = self.types_.find(std::string(type));
    return it != self.types_.end() && tagged(it->second, tag);
  }

  static std::vector<std::string_view> tags_of(std::string_view type) {
    auto &self = instance();
    std::lock_guard<std::mutex> lock(self.mutex_);
    const auto it = self.types_.find(std::string(type));
    if (it == self.types_.end()) {
      throw std::out_of_range("unknown type: " + std::string(type));
    }
    return it->second.tags;
  }

  static std::vector<std::unique_ptr<ObjectBase>> create_all(std::string_view tag) {
    const auto names = list_types(tag);
    std::vector<std::unique_ptr<ObjectBase>> objects;
    objects.reserve(names.size());
    for (const auto name : names) {
      objects.push_back(create(name));
    }
    return objects;
  }

  template <typename Tag>
  static std::vector<std::unique_ptr<ObjectBase>> create_all() {
    return create_all(tag_name<Tag>());
  }

  static std::vector<std::shared_ptr<ObjectBase>> create_shared_all(std::string_view tag) {
    const auto names = list_types(tag);
    std::vector<std::shared_ptr<ObjectBase>> objects;
    objects.reserve(names.size());
    for (const auto name : names) {
      objects.push_back(create_shared(name));
    }
    return objects;
  }

  template <typename Tag>
  static std::vector<std::shared_ptr<ObjectBase>> create_shared_all() {
    return create_shared_all(tag_name<Tag>());
  }

  // Lazy singleton: created via the type's shared factory on first use, then cached
  // per name. set_single_instance() replaces or pre-seeds the cached instance,
  // keying off the instance's __type_index().
  static std::shared_ptr<ObjectBase> get_single_instance(std::string_view name) {
    auto &self = instance();
    const std::string key(name);
    std::shared_ptr<SingletonInitialization> initialization;
    {
      std::unique_lock<std::mutex> lock(self.mutex_);
      const auto it = self.singletons_.find(key);
      if (it != self.singletons_.end()) {
        return it->second;
      }
      const auto pending = self.singleton_initializations_.find(key);
      if (pending != self.singleton_initializations_.end()) {
        initialization = pending->second;
        if (initialization->owner == std::this_thread::get_id()) {
          throw std::logic_error("recursive singleton initialization: " + key);
        }
        initialization->ready.wait(lock, [&] {
          return initialization->done || self.singletons_.find(key) != self.singletons_.end();
        });
        const auto cached = self.singletons_.find(key);
        if (cached != self.singletons_.end()) {
          return cached->second;
        }
        std::rethrow_exception(initialization->error);
      }
      initialization = std::make_shared<SingletonInitialization>();
      self.singleton_initializations_.emplace(key, initialization);
    }
    // No registry lock (or per-name lock) is held while invoking user code.
    // Keep the constructed object alive until after unlocking, even if a setter wins.
    std::shared_ptr<ObjectBase> singleton;
    try {
      singleton = create_shared(key);
      std::lock_guard<std::mutex> lock(self.mutex_);
      const auto cached = self.singletons_.emplace(key, singleton).first;
      initialization->done = true;
      self.singleton_initializations_.erase(key);
      initialization->ready.notify_all();
      return cached->second;
    } catch (...) {
      std::lock_guard<std::mutex> lock(self.mutex_);
      initialization->error = std::current_exception();
      initialization->done = true;
      self.singleton_initializations_.erase(key);
      initialization->ready.notify_all();
      throw;
    }
  }

  // Typed variant; works under -fno-rtti (identity via __type_index, no dynamic_cast).
  template <typename T>
  static std::shared_ptr<T> get_single_instance() {
    static_assert(std::is_base_of_v<ObjectBase, T>, "T must derive from obj::ObjectBase");
    auto singleton = get_single_instance(T::_type());
    if (singleton == nullptr) {
      throw std::logic_error("factory produced a null instance for " + std::string(T::_type()));
    }
    if (singleton->__type_index() != std::type_index(typeid(T))) {
      throw std::logic_error("singleton type mismatch for " + std::string(T::_type()));
    }
    return std::static_pointer_cast<T>(singleton);
  }

  static void set_single_instance(std::shared_ptr<ObjectBase> singleton) {
    if (singleton == nullptr) {
      throw std::invalid_argument("set_single_instance: null instance");
    }
    auto &self = instance();
    const std::type_index index = singleton->__type_index();
    std::lock_guard<std::mutex> lock(self.mutex_);
    for (const auto &entry : self.types_) {
      if (entry.second.type_index == index) {
        self.singletons_[entry.first] = std::move(singleton);
        const auto pending = self.singleton_initializations_.find(entry.first);
        if (pending != self.singleton_initializations_.end()) {
          pending->second->ready.notify_all();
        }
        return;
      }
    }
    throw std::out_of_range("set_single_instance: unregistered type: " + std::string(index.name()));
  }

 private:
  // Fields are protected by mutex_; shared ownership keeps the condition variable
  // alive for waiters after the completed attempt is removed from the map.
  struct SingletonInitialization {
    std::condition_variable ready;
    std::thread::id owner = std::this_thread::get_id();
    bool done = false;
    std::exception_ptr error;
  };

  ObjectFactory() = default;
  ~ObjectFactory() = default;

  static ObjectFactory &instance() {
    static ObjectFactory factory;
    return factory;
  }

  // Factories are static function pointers, so they stay valid once copied out;
  // invoking them outside the lock keeps user constructors free to touch the factory.
  TypeInfo info_of(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = types_.find(std::string(name));
    if (it == types_.end()) {
      throw std::out_of_range("unknown type: " + std::string(name));
    }
    return it->second;
  }

  TypeInfo::Factory factory_of(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = types_.find(std::string(name));
    if (it == types_.end()) {
      throw std::out_of_range("unknown type: " + std::string(name));
    }
    return it->second.create;
  }

  static bool tagged(const TypeInfo &info, std::string_view tag) {
    return std::find(info.tags.begin(), info.tags.end(), tag) != info.tags.end();
  }

  std::mutex mutex_;
  std::unordered_map<std::string, TypeInfo> types_;
  std::unordered_map<std::string, std::shared_ptr<ObjectBase>> singletons_;
  std::unordered_map<std::string, std::shared_ptr<SingletonInitialization>> singleton_initializations_;
};

}  // namespace obj
