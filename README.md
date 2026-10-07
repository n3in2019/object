# object

Header-only C++17 object factory with static auto-registration, struct-tag filtering,
and lazy singletons. API shaped after libroboflow's `rf::obj` module; overall design
inspired by [Drogon](https://github.com/drogonframework/drogon)'s `DrClassMap`.

```cpp
#include <object/object.h>
#include <object/object_factory.h>

namespace tag {
struct Shape {};
struct Renderable {};
}  // namespace tag

class Circle : public obj::Object<Circle, tag::Shape, tag::Renderable> {
 public:
  double radius = 1.0;
};

// Create by fully qualified name (Circle::_type() is the portable way to spell it)
std::unique_ptr<obj::ObjectBase> a = obj::ObjectFactory::create(Circle::_type());
std::shared_ptr<obj::ObjectBase> b = obj::ObjectFactory::create_shared(Circle::_type());

// Filter and instantiate by tag — typed or string form
for (std::string_view name : obj::ObjectFactory::list_types<tag::Shape>()) { /* ... */ }
auto renderers = obj::ObjectFactory::create_all<tag::Renderable>();  // one of each

// Lazy, cached singletons
auto circle = obj::ObjectFactory::get_single_instance<Circle>();
```

## Headers

| File | Contents |
| --- | --- |
| `object/object_base.h` | `ObjectBase` — non-copyable polymorphic base, `__type_index()` |
| `object/type_info.h` | `TypeInfo` — registration record (function-pointer factories, tags) |
| `object/object_factory.h` | `ObjectFactory` registry, `tag_name<T>()` |
| `object/object.h` | `Object<Derived, Tags...>`, `TagMarker`, `is_tagged_v` |

## How registration works — and its one caveat

Deriving from `Object<T, Tags...>` is all that's needed:

- The template holds a static flag whose initializer registers the class (name,
  factories, tags) with `ObjectFactory` at static initialization, before `main`.
- Names are fully qualified compile-time strings extracted from `__PRETTY_FUNCTION__`
  (`__FUNCSIG__` on MSVC) — no demangler, no allocations, stable for the program's life.
- **Caveat:** a static member's initializer is only instantiated when odr-used. The
  class odr-uses it from `_type()`, `__type_index()`, and its constructor/destructor, so
  any translation unit that *uses* the class in compiled code registers it before
  `main`. A class that no TU ever touches (pure declarative plugin in an archive) is
  not registered — reference `MyPlugin::_type()` (or construct one) somewhere in its
  defining TU.
- Static archives may drop objects nothing references; link plugins with
  `--whole-archive`, or use object libraries / shared libraries. The `[[gnu::used]]`
  attribute on the flag keeps emitted definitions alive.
- Duplicate registration keeps the first and warns on stderr. Unknown names throw
  `std::out_of_range`; predicates (`has_tag`) return false instead.
- The registry and singleton caches are mutex-guarded; concurrent create/filter/
  singleton use is safe. Constructors run outside the lock, so a constructor may
  itself use the factory.

## Tags

Tags are empty structs passed as template parameters. Each becomes a private
`TagMarker<Tag>` base of the class, which enables a compile-time check:

```cpp
static_assert(obj::is_tagged_v<Circle, tag::Shape>);

obj::tag_name<tag::Renderable>();          // "tag::Renderable"
obj::ObjectFactory::has_tag(name, tag);    // runtime check (string form)
obj::ObjectFactory::tags_of(name);         // tags in declaration order
```

Query APIs exist in typed (`list_types<tag::Shape>()`) and string
(`list_types("tag::Shape")`) forms; the string form matters for config-driven lookups.
Duplicate tag types on one class are a compile error (duplicate base class).

## Singletons

`get_single_instance(name)` creates on first use via the type's shared factory and
caches per name; `get_single_instance<T>()` is the typed form and works under
`-fno-rtti` (identity via `__type_index()` + `static_pointer_cast`, no `dynamic_cast`).
`set_single_instance(instance)` replaces or pre-seeds the cached singleton, keyed off
the instance's `__type_index()`.

Concurrent first access to the same name runs only one factory attempt; other
callers wait without holding the registry mutex. Constructors may call registry
APIs, create ordinary objects, or request other singletons. Recursive singleton
dependencies must be acyclic: requesting a singleton already being constructed on
the same thread throws `std::logic_error`; cycles across threads are unsupported.
If construction throws, waiting callers receive the failure unless a cached value
is available, and a later call may retry. `set_single_instance()` never waits for
an in-flight constructor: it publishes its replacement immediately, wakes waiting
readers, and the older successful factory attempt cannot overwrite it. A factory
attempt that throws still propagates its exception to its initiating caller.

## Notes

- `Object<T>` requires a default-constructible `T` (`static_assert` otherwise). Only
  the class naming `Object<Itself, ...>` is registered — plain subclasses are not.
- `__type_index` uses a double underscore (reserved-for-the-implementation by the
  standard, but fine in practice on GCC/Clang) to mark it as a framework hook.
- For gradual adoption inside libroboflow: `namespace rf::obj = obj;`.
- Tested with GCC 16 and Clang 22 (C++17, `-Wall -Wextra -Wpedantic`). MSVC is
  best-effort via `__FUNCSIG__` parsing; tag/type names may include `struct` keywords.

## Build

CMake interface target (`object::object`); tests via `OBJECT_BUILD_TESTING`:

```sh
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Or directly, without CMake:

```sh
clang++ -std=c++17 -Iinclude tests/test_object_factory.cpp tests/second_tu.cpp \
    -lgtest -lgtest_main -pthread -o test_object && ./test_object
```

## License

MIT — see [LICENSE](LICENSE).
