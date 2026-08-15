#pragma once

#include <typeindex>

namespace obj {

// Polymorphic base for all factory-created objects. Non-copyable and non-movable so
// instances keep a stable identity, which is what singletons and type-index lookups rely on.
//
// __type_index() deliberately duplicates what typeid(*p) gives on a polymorphic type:
// it keeps reporting the dynamic type under -fno-rtti, where typeid on an expression
// degrades to the static type and dynamic_cast is unavailable. It is also a hook a
// derived class can override to report a different identity than its own typeid.
struct ObjectBase {
  virtual ~ObjectBase() = default;

  ObjectBase(const ObjectBase &) = delete;
  ObjectBase &operator=(const ObjectBase &) = delete;
  ObjectBase(ObjectBase &&) = delete;
  ObjectBase &operator=(ObjectBase &&) = delete;

  virtual std::type_index __type_index() const = 0;

 protected:
  ObjectBase() = default;
};

}  // namespace obj
