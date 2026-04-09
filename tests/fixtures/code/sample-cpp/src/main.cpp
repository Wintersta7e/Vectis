#include "widget.hpp"

#include <cstdio>

int main()
{
    demo::Widget w("sidebar", demo::Rect{320, 480});
    w.resize(400, 600);
    std::printf("widget '%s' is %dx%d\n",
                w.name().c_str(), w.bounds().width, w.bounds().height);
    return 0;
}
