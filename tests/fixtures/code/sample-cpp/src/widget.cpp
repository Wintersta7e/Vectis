#include "widget.hpp"

#include <utility>

namespace demo {

Widget::Widget(std::string name, Rect bounds) : m_name(std::move(name)), m_bounds(bounds) {}

Widget::~Widget() = default;

const std::string& Widget::name() const
{
    return m_name;
}

Rect Widget::bounds() const
{
    return m_bounds;
}

void Widget::resize(int width, int height)
{
    m_bounds.width = width;
    m_bounds.height = height;
}

} // namespace demo
