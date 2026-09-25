// Simple test: a std::vector<Item> that grows a lot, forcing many
// reallocations. Each reallocation moves every existing element to a new
// buffer, which would normally invalidate any raw pointer taken earlier.
// A Tracker<Item> obtained on the very first element should still resolve
// to the correct, final address once the vector is done growing.

#include <skiptracer.hpp>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace skip;

struct Item : Trackable {

public:
    int id{0};

public:
    skip_FiveRule(Item);

public:
    virtual ~Item(void) = default;
};

int main() {
    std::vector<Item> items;
    items.reserve(0); // start with zero capacity: guarantees reallocations happen

    items.push_back(Item{});
    items.back().id = 0;

    Tracker<Item> t = items.front().tracker();

    std::printf("initial address : %p (capacity %zu)\n",
        (void*)t.locate<Item>(), items.capacity());

    // Force many reallocations by growing the vector one element at a time.
    constexpr int kCount = 100000;
    for (int i = 1; i < kCount; ++i) {
        items.push_back(Item{});
        items.back().id = i;
    }

    Item* final_ptr = t.locate<Item>();

    std::printf("final address   : %p (capacity %zu, size %zu)\n",
        (void*)final_ptr, items.capacity(), items.size());

    // The tracker must still point at the same logical object: the first
    // element ever pushed, wherever the vector's buffer ended up.
    assert(final_ptr != nullptr);
    assert(final_ptr == &items.front());
    assert(final_ptr->id == 0);

    std::printf("test passed\n");
    return 0;
}