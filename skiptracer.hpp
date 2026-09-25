/*************************************************************************
SKIP TRACER: a small, header-only C++20 library that hands out safe,
type-aware handles (Tracker) to objects that may move or be destroyed
at any time.

Design summary
---------------
- Trackable is polymorphic and non-template.
- relocate() runs automatically from Trackable's own move constructor and
  move assignment - a derived class never has to call it itself.
- Tracker<T>::locate<U>() uses static_cast when possible (compile-time,
  free), falling back to dynamic_cast only when static_cast can't resolve
  the relation (virtual inheritance, or a crosscast between branches).
- Slots live in a stable pool (SlotPool) and are recycled through an
  intrusive free-list. Validity is tracked with a generation counter
  instead of a reference count: locate() costs a single atomic load, no
  read-modify-write, no allocation, no lock on the hot path.

Thread-safety
--------------
- Slot::ptr is a plain pointer; its visibility across threads is
  established by Slot::generation (release on write, acquire on read).
- SlotPool::acquire()/release() take a mutex - assumed rare compared to
  locate()/tracker(), so kept off the hot path.
- Moving/destroying the same Trackable instance from two threads at once
  still needs external synchronization, like any ordinary C++ object.
- relocate() publishes the new address as soon as Trackable's own move is
  done, which happens *before* a derived class's own members are moved
  (ordinary base-then-members order). A Tracker<T> never dereferences a
  stale address, but reading through it while the *same* object is still
  being moved on another thread is a race on that object's members, same
  as for any untracked object moved and read without synchronization.

Repository: https://github.com/agemo-dev/skiptracer
Version: 1.0.0
License: MIT (see end of file).
*************************************************************************/

#ifndef SKIP_TRACER_HPP_
#define SKIP_TRACER_HPP_

#include <mutex>
#include <vector>
#include <memory>
#include <atomic>
#include <cstdint>
#include <cassert>
#include <concepts>
#include <type_traits>

namespace skip {

class Trackable;

// ============================================================================
// Concepts
// ============================================================================

template<typename T>
concept IsTracker = std::is_base_of_v<class TrackerBase, T>;

template<typename T>
concept IsTrackable = std::derived_from<T, Trackable>;

template<typename U, typename T>
concept IsRelatedTo =  std::same_as<U, T>        ||
                       std::derived_from<T, U>   ||
                       std::derived_from<U, T>   ||
                       std::same_as<U, void>;

// ============================================================================
// Slot / SlotPool
// ============================================================================

// Stays at a stable address for as long as any Tracker<T> may reference it.
struct Slot {

public:
    // Not atomic: visibility is published through `generation` below.
    // While free, this field is repurposed by the pool as a free-list link.
    Trackable* ptr{nullptr};

    // Even while in use, odd while free. Bumped on relocate()/acquire()/release().
    std::atomic<std::uint32_t> generation{0};

public:
    Slot(void) = default;

    Slot(Slot&& other) noexcept
        : ptr(other.ptr), generation(other.generation.load(std::memory_order_relaxed))
    {
        other.ptr = nullptr;
    }

public:
    // Slots are never copied between each other: two objects must never share a Slot.
    Slot(const Slot&) : ptr(nullptr), generation(0) {}
    Slot& operator=(const Slot&) { return *this; }

public:
    ~Slot(void) = default;
};

// Fixed-block pool: allocates Slots in blocks of BLOCK_SIZE, recycles freed
// slots through an intrusive free-list, never moves a Slot once constructed.
class SlotPool {

private:
    static constexpr std::size_t k_none = static_cast<std::size_t>(-1);

private:
    std::vector<std::unique_ptr<Slot[]>> m_blocks;
    std::vector<std::size_t>             m_next_free;
    std::size_t                          m_free_head{k_none};
    std::mutex                           m_mutex;

public:
    static constexpr std::size_t BLOCK_SIZE = 4096;

public:
    static SlotPool& getInstance(void) noexcept {
        static SlotPool pool;
        return pool;
    }

    // Hands out a Slot ready to use (generation even, ptr null).
    Slot* acquire(void) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_free_head == k_none) grow();

        std::size_t idx = m_free_head;
        Slot* slot = &m_blocks[idx / BLOCK_SIZE][idx % BLOCK_SIZE];

        m_free_head = m_next_free[idx];
        slot->ptr = nullptr;
        slot->generation.fetch_add(1, std::memory_order_relaxed); // odd -> even
        return slot;
    }

    // Returns a Slot to the pool. Bumped to odd so any stale Tracker<T>
    // snapshot can never match again, even after the slot is recycled.
    void release(Slot* slot) {
        std::lock_guard<std::mutex> lock(m_mutex);
        slot->ptr = nullptr;
        slot->generation.fetch_add(1, std::memory_order_release); // even -> odd

        std::size_t idx  = indexOf(slot);
        m_next_free[idx] = m_free_head;
        m_free_head      = idx;
    }

private:
    void grow(void) {
        std::size_t base = m_blocks.size() * BLOCK_SIZE;
        m_blocks.push_back(std::make_unique<Slot[]>(BLOCK_SIZE));
        m_next_free.resize(base + BLOCK_SIZE);

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            std::size_t idx = base + i;
            m_next_free[idx] = (i + 1 == BLOCK_SIZE) ? k_none : idx + 1;
            m_blocks.back()[i].generation.store(1, std::memory_order_relaxed); // free = odd
        }
        m_free_head = base;
    }

    std::size_t indexOf(const Slot* slot) const noexcept {
        for (std::size_t b = 0; b < m_blocks.size(); ++b) {
            const Slot* first = m_blocks[b].get();
            if (slot >= first && slot < first + BLOCK_SIZE) {
                return b * BLOCK_SIZE + static_cast<std::size_t>(slot - first);
            }
        }
        assert(false && "skiptracer: Slot not owned by this pool");
        return k_none;
    }
};

// ============================================================================
// UnsafeTracker<T> / Tracker<T>
// ============================================================================

class TrackerBase {};

// UnsafeTracker is a base type .
// It tracks any type never check if is inherite to trackable .
// Is particullary useful in class implemetation which declare a tracker of
// itself type .
//
// with Tracker:
// class Foo : Trackable {
//     Tracker<Foo> tracker; <- ERROR ! Foo is not complet and can't satisfy IsTrackable
// };
//
// with UnsafeTracker:
// class Foo : Trackable {
//     UnsafeTracker<Foo> tracker; <- OK ! UnsafeTracker do not check inheritance
// };
//
// But is deprecate fort basic / current usage besauce `T` is not checked in class implementation
// If type were not trackable, le program will may crash !

template<typename T> class UnsafeTracker : private TrackerBase {

private:
    Slot*         m_slot{nullptr};
    std::uint32_t m_generation{0};

public:
    UnsafeTracker(void)                              = default;
    UnsafeTracker(UnsafeTracker&&)                   = default;
    UnsafeTracker(const UnsafeTracker&)              = default;
    UnsafeTracker& operator=(UnsafeTracker&&)        = default;
    UnsafeTracker& operator=(const UnsafeTracker&)   = default;

public:
    // Implicit on purpose: the actual pointer conversion is deferred to
    // locate<U>(), so no cast happens here.
    UnsafeTracker(Slot* slot, std::uint32_t generation) noexcept
        : m_slot(slot), m_generation(generation) {}

    // Implicit conversion from any related UnsafeTracker<U>, e.g. the
    // UnsafeTracker<Trackable> returned by Trackable::tracker().
    template<typename U> requires IsRelatedTo<U, T>
    UnsafeTracker(const UnsafeTracker<U>& other) noexcept
        : m_slot(other.m_slot), m_generation(other.m_generation) {}

public:
    T& operator*(void) {
        assert(locate<T>() && "skiptracer: dereferencing an expired Tracker");
        return *locate<T>();
    }

    const T& operator*(void) const {
        assert(locate<T>() && "skiptracer: dereferencing an expired Tracker");
        return *locate<T>();
    }

    T*       operator->(void)       { return locate<T>(); }
    const T* operator->(void) const { return locate<T>(); }

    explicit operator bool(void) const { return locate<T>() != nullptr; }

    bool operator==(const UnsafeTracker<T>& other) const noexcept {
        return m_slot == other.m_slot && m_generation == other.m_generation;
    }

    bool operator!=(const UnsafeTracker<T>& other) const noexcept {
        return !(*this == other);
    }

public:
    // static_cast when the relation to Trackable is resolvable at compile
    // time; dynamic_cast otherwise (virtual inheritance, crosscast).
    template<typename U = T> requires IsRelatedTo<U, T>
    U* locate(void) noexcept {
        if (!m_slot) return nullptr;
        if (m_slot->generation.load(std::memory_order_acquire) != m_generation) return nullptr;
        Trackable* base = m_slot->ptr;
        if constexpr (std::same_as<U, void>) {
            return static_cast<void*>(base);
        } else if constexpr (requires (Trackable* b) { static_cast<U*>(b); }) {
            return static_cast<U*>(base);
        } else {
            return dynamic_cast<U*>(base);
        }
    }

    template<typename U = T> requires IsRelatedTo<U, T>
    const U* locate(void) const noexcept {
        if (!m_slot) return nullptr;
        if (m_slot->generation.load(std::memory_order_acquire) != m_generation) return nullptr;
        const Trackable* base = m_slot->ptr;
        if constexpr (std::same_as<U, void>) {
            return static_cast<const void*>(base);
        } else if constexpr (requires (const Trackable* b) { static_cast<const U*>(b); }) {
            return static_cast<const U*>(base);
        } else {
            return dynamic_cast<const U*>(base);
        }
    }

public:
    template<typename> friend class UnsafeTracker; // for the converting constructor below
};

// A safe Tracker for basic usage
template<IsTrackable SafeType> using Tracker = UnsafeTracker<SafeType>;

// ============================================================================
// Trackable
// ============================================================================

// Declares all five special members as defaulted. Use this in any class
// deriving from Trackable that would otherwise declare only some of its
// own special members (e.g. only a virtual destructor) - see the rule of
// five note below.
#define skip_FiveRule(CLASS)                     \
    CLASS(void)                       = default; \
    CLASS(CLASS&&)                    = default; \
    CLASS(const CLASS&)               = default; \
    CLASS& operator=(CLASS&&)         = default; \
    CLASS& operator=(const CLASS&)    = default

// If a derived class declares ANY of its own destructor, copy/move
// constructor or assignment, the compiler stops implicitly generating the
// others (rule of five) - including the move that would call into
// Trackable's move and trigger relocate(). E.g. a derived class declaring
// only `virtual ~Derived() = default;` silently loses its move
// constructor: `Derived b = std::move(a);` becomes a copy, and existing
// trackers keep pointing at `a`. If a derived class declares any special
// member, declare all five explicitly so the move keeps flowing through -
// skip_FiveRule(CLASS) above does this for you in one line.
class Trackable {

private:
    mutable Slot*         m_slot{nullptr};
    mutable std::uint32_t m_generation{0};

public:
    Trackable(void)                        = default;
    Trackable(const Trackable&)            = default;
    Trackable& operator=(const Trackable&) = default;

public:
    Trackable(Trackable&& other) noexcept : m_slot(other.m_slot) {
        other.m_slot = nullptr;
        relocate();
    }

    Trackable& operator=(Trackable&& other) noexcept {
        if (this == &other) return *this;
        releaseSlot();
        m_slot = other.m_slot;
        other.m_slot = nullptr;
        relocate();
        return *this;
    }

protected:
    // Re-publishes `this` as the slot's current address. Does not bump the
    // generation: a move keeps tracking the same logical object.
    void relocate(void) const noexcept {
        if (!m_slot) return;
        m_slot->ptr = const_cast<Trackable*>(this);
        m_slot->generation.fetch_add(0, std::memory_order_release);
    }

public:
    Tracker<Trackable> tracker(void) const noexcept {
        if (!m_slot) {
            m_slot = SlotPool::getInstance().acquire();
            m_slot->ptr = const_cast<Trackable*>(this);
        }
        return Tracker<Trackable>(m_slot, m_slot->generation.load(std::memory_order_relaxed));
    }

private:
    void releaseSlot(void) const noexcept {
        if (!m_slot) return;
        SlotPool::getInstance().release(m_slot);
        m_slot = nullptr;
    }

public:
    virtual ~Trackable(void) {
        releaseSlot();
    }
};


// ============================================================================
// Box
// ============================================================================

// Pointer-like wrapper to make a non-Trackable type (int, std::string, ...)
// trackable. Holds a value of type T for direct manipulation.
template<typename T> class Box : public Trackable {

public:
    // Must never be moved individually out of its Box: doing so bypasses
    // relocate() and breaks address validity, unless T's own move already
    // preserves the address of what trackers actually care about (true for
    // basic types and STL containers, whose own storage is stable/handled
    // correctly on move).
    T value;

public:
    skip_FiveRule(Box);

public:
    // Excludes Box&/const Box& so this never competes with the copy/move
    // constructors above (which the rule of five needs explicit, see the
    // note on Trackable).
    template<typename... Args>
        requires (sizeof...(Args) != 1 ||
                  (!std::same_as<std::decay_t<Args>, Box> && ...))
    Box(Args&&... args) : value(std::forward<Args>(args)...) {}

public:
    // Helper: `value` is public and directly accessible anyway.
    T&       operator*(void)       noexcept { return value; }
    const T& operator*(void) const noexcept { return value; }

public:
    T*       operator->(void)       noexcept { return &value; }
    const T* operator->(void) const noexcept { return &value; }

public:
    virtual ~Box(void) = default;
};

} // namespace skip

#endif // SKIP_TRACER_HPP_

/******************************************************************************
MIT License

Copyright (c) 2026 agemo-dev

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
******************************************************************************/