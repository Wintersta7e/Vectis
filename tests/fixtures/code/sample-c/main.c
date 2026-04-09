#include <stdio.h>
#include <stdlib.h>

struct Point {
    int x;
    int y;
};

struct Line {
    struct Point start;
    struct Point end;
};

int add(int a, int b) {
    return a + b;
}

struct Point make_point(int x, int y) {
    struct Point p = {x, y};
    return p;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    struct Point origin = make_point(0, 0);
    printf("origin=(%d,%d)\n", origin.x, origin.y);
    return 0;
}
