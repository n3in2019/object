#include "object/object.h"

#include <string_view>

namespace second_tu {
namespace tag {
struct Remote {};
}  // namespace tag

// Registered from this translation unit only; test_object_factory.cpp never
// includes this definition.
struct Widget : obj::Object<Widget, tag::Remote> {};

std::string_view widget_type_name() { return Widget::_type(); }

std::string_view widget_tag_name() { return obj::tag_name<tag::Remote>(); }

}  // namespace second_tu
