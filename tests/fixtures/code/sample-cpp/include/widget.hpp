#pragma once

#include <string>

namespace demo {

struct Rect {
    int width;
    int height;
};

class Widget {
public:
    Widget(std::string name, Rect bounds);
    ~Widget();

    const std::string& name() const;
    Rect bounds() const;

    void resize(int width, int height);

private:
    std::string m_name;
    Rect m_bounds;
};

} // namespace demo
