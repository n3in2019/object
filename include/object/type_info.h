#pragma once

#include <memory>
#include <string_view>
#include <typeindex>
#include <vector>

namespace obj {

struct ObjectBase;

// Registration record. Function-pointer factories keep the record copyable and
// allocation-free; all strings are views into static storage (constexpr type names
// and tag names), valid for the lifetime of the program.
struct TypeInfo {
  using Factory = std::unique_ptr<ObjectBase> (*)();
  using SharedFactory = std::shared_ptr<ObjectBase> (*)();

  std::string_view name;
  Factory create = nullptr;
  SharedFactory create_shared = nullptr;  // may be null; the factory falls back to wrapping create()
  std::vector<std::string_view> tags;
  // Used by set_single_instance() to locate this record; the default matches nothing,
  // so manual registrations that want singleton support must set it.
  std::type_index type_index = std::type_index(typeid(void));
};

}  // namespace obj
