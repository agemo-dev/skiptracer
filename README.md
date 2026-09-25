# Skip Tracer

A smart pointers library that automatically follows and tracks objects even after they are moved.

![logo](asset/logo.png)

---

## Overview

***Skip Tracer*** is a **header only** library designed to ensure memory safety.
It offers various pointer like classes that let you manipulate memory addresses without risking a dangling pointer.
The whole library is available in the header `skiptracer.hpp` at the root of the repository and lives entirely under
the `skip` namespace.

---

## Box

`Box<T>` is a **wrapper** used to wrap various types. ***Skip Tracer*** only lets you track types that inherit from
`Trackable`, so `Box<T>` acts as a guard rail to make a non trackable type (`int`, `std::string`, etc.) trackable :

```cpp
skip::Box<int> num;
std::cout << num.value << std::endl;

// or by dereferencing
std::cout << *num << std::endl;
```

This type has two overloads, const and mutable, of the `*` (dereference) and `->` operators :

```cpp
T&       operator*(void)       noexcept { return value; }
const T& operator*(void) const noexcept { return value; }

T*       operator->(void)       noexcept { return &value; }
const T* operator->(void) const noexcept { return &value; }
```

It only holds one public member :

```cpp
T value;
```

`Box<T>` inherits from `Trackable`, so it can be tracked like any other trackable class.

---

## Tracker

`Tracker` objects let you follow trackable objects (classes inheriting from `Trackable`, including `Box<T>`).
They point to the real address of the tracked object even after it moves (via `std::move`),
and point to `nullptr` once the object is destroyed. This avoids dangling pointers and segmentation faults.

Their methods are as follows :

```cpp
// Get the current pointer, optionally converted to a related type U
template<typename U = T> requires IsRelatedTo<U, T>
U* locate(void) noexcept;

template<typename U = T> requires IsRelatedTo<U, T>
const U* locate(void) const noexcept;

T& operator*(void);
const T& operator*(void) const;

T*       operator->(void);
const T* operator->(void) const;

explicit operator bool(void) const;
bool operator==(const UnsafeTracker<T>&) const noexcept;
bool operator!=(const UnsafeTracker<T>&) const noexcept;
```

***Skip Tracer*** ships two tracker types :

- `UnsafeTracker<T>`
- `Tracker<T>`

### UnsafeTracker

`UnsafeTracker` is the base tracker type. It lets you track an object without any check on
its actual nature :

```cpp
template<typename T> class UnsafeTracker;
```

As its name suggests, this tracker type is not "safe". Nothing at compile time guarantees that the tracked type
actually inherits from `Trackable`, which can make it fall back to behaving like an **unprotected raw pointer**.
It is therefore not recommended unless you are certain the tracked type is actually **trackable**.

```cpp
skip::UnsafeTracker<std::string> unsafe; // std::string does not inherit from Trackable: this will never work correctly
skip::UnsafeTracker<skip::Box<std::string>> safe; // OK, Box inherits from Trackable
```

This example is a bit misleading since `std::string` has no `tracker()` method anyway, so it could never actually be
tracked, but it illustrates the point : `UnsafeTracker` does not protect you from this kind of mistake at compile time.

It can still be very useful when declaring a class, when the type is still incomplete at the point of writing
(self reference, or reference to a type declared later) :

```cpp
class Foo : public skip::Trackable {

public:
    skip::UnsafeTracker<Foo> other;
};
```

### Tracker

`Tracker<T>` will be the most commonly used tracker type. Unlike `UnsafeTracker<T>`, `Tracker<T>` checks at compile
time that `T` actually inherits from `Trackable`, through the `IsTrackable` concept :

```cpp
template<IsTrackable SafeType> using Tracker = UnsafeTracker<SafeType>;
```

```cpp
class Foo {};

skip::Tracker<Foo> tracker; // ERROR: Foo does not inherit from Trackable
```

```cpp
class Foo : public skip::Trackable {};

skip::Tracker<Foo> tracker; // OK

// or
skip::Tracker<skip::Box<Foo>> tracker; // OK
```

To retrieve the current pointer, use the methods shown above :

```cpp
// check that the object is still alive
if (tracker) {
    auto* ptr = tracker.locate();
    ptr->doSomething();
}

// or more simply
if (tracker) {
    tracker->doSomething();
}
```

It is recommended to use the `*` or `->` operators directly rather than storing the pointer separately, to always be
sure of pointing at the right address, especially in a multithreaded environment : a pointer stored on its own can
become stale as soon as `locate()` returns if the object is moved or destroyed on another thread in the meantime.

`locate` can also convert the pointer to a related type, as long as an inheritance relation exists between the two :

```cpp
class Bar {};
class Foo : public Bar, public skip::Trackable {};

skip::Tracker<Foo> tracker = foo.tracker();
auto* ptr = tracker.locate<Bar>();
```

The conversion uses `static_cast` whenever the relation can be resolved at compile time (zero cost), and only falls
back to `dynamic_cast` when necessary (virtual inheritance, or a crosscast to a "cousin" type in a diamond hierarchy).

---

## Trackable

`Trackable` is the base class every class you want to make trackable must inherit from :

```cpp
class Foo : public skip::Trackable {};
```

`Trackable` is polymorphic and non template : a single base class is enough regardless of how many inheritance
levels exist or where it sits in the base list.

It has one public method :

```cpp
// Get a tracker object pointing at the current instance
Tracker<Trackable> tracker(void) const noexcept;
```

This method is used to obtain a tracker from a trackable object :

```cpp
skip::Tracker<Foo> tracker = foo.tracker();
```

### Things to know before inheriting from Trackable

If a derived class declares any of its own destructor, copy constructor, copy assignment, move constructor, or move
assignment, the compiler stops implicitly generating the others (rule of five). This includes the move constructor
that would normally call into `Trackable`'s own move and trigger the address update.

Concretely, a class that only declares `virtual ~Derived() = default;` silently loses its move constructor :
`Derived b = std::move(a);` then becomes a **copy**, and existing trackers keep pointing at `a`, with no compiler
error to warn you.

If a derived class needs to declare any of its own special members, it is recommended to declare all five explicitly
(`= default` is enough) so the move keeps flowing through to `Trackable`. The `skip_FiveRule(CLASS)` macro does this
for you in one line :

```cpp
#define skip_FiveRule(CLASS)                     \
    CLASS(void)                    = default;    \
    CLASS(CLASS&&)                 = default;    \
    CLASS(const CLASS&)            = default;    \
    CLASS& operator=(CLASS&&)      = default;    \
    CLASS& operator=(const CLASS&) = default
```

```cpp
class Foo : public skip::Trackable {
public:
    skip_FiveRule(Foo);
    virtual ~Foo() = default; // needs its own destructor, e.g. to be a polymorphic base
};
```

It is recommended to always add `skip_FiveRule(CLASS)` to a class inheriting from `Trackable` as soon as it declares
any of its own special members, even if you never intend to move it directly, since a base class further down an
inheritance chain could still be moved through it.

---

## Concept

***Skip Tracer*** offers **three different concepts** :

```cpp
// Check if a type is a tracker type
template<typename T>
concept IsTracker = std::is_base_of_v<class TrackerBase, T>;

// Check if a type inherits from the Trackable class
template<typename T>
concept IsTrackable = std::derived_from<T, Trackable>;

// Check if a pointer is convertible to another type (via static_cast or dynamic_cast)
template<typename U, typename T>
concept IsRelatedTo =  std::same_as<U, T>        ||
                       std::derived_from<T, U>   ||
                       std::derived_from<U, T>   ||
                       std::same_as<U, void>;
```

---

## Thread safety

- Comparing a `Slot`'s generation only ever uses a plain atomic `load` (no read modify write), both on write and on
  read : `locate()` and `tracker()` are safe to call concurrently from multiple threads, with no lock on the hot path.
- Creating and destroying a `Slot` (via `SlotPool::acquire()`/`release()`) is protected by a mutex, assumed to be rare
  compared to `locate()`/`tracker()` calls.
- Moving or destroying the same `Trackable` instance from two threads at once still needs your own external
  synchronization, like for any ordinary C++ object.
- The new address is published as soon as the `Trackable` part of a move is done, which happens **before** a derived
  class's own members are moved (ordinary base then members order in C++). A `Tracker<T>` will never dereference a
  stale address, but reading through it while the same object is still being moved on another thread remains a race
  on that object's members, exactly as it would for an untracked object moved and read without synchronization.

---

## More

***Skip Tracer*** is currently at version **1.0**.
The library is functional but may contain bugs. If you find one, please contact me
at [my e-mail](mailto:my.agemo.dev@gmail.com).

---

## Version

Version | Release date | Patch note |
|---|---|---|
| 1.0.0 | sept. 25 2026 | Added all base features |

---

## Licence

**MIT** - free to use, modify and distribute.