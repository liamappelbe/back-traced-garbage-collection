# Back-Traced Garbage Collector

An experimental garbage collector for C++. It is designed for use cases where
consistent real time performance is critical, such as games or UIs. This is not
an officially supported Google product.

Advantages:

 - A simple API, in a single header of a few hunderd lines.
 - No threading. All GC work happens on the main thread.
 - Very short, very consistent GC pauses. A tiny amount of GC work is done each
   time a new object is allocated and there are essentially no pauses longer
   than a few microseconds, even when managing heaps in the tens of GB.
 - Consistent smooth collection in almost all cases. Garbage is collected in
   small chunks, rather than all at once, which decreases the peak heap size.
 - A single tuning parameter that directly controls how much work the GC does
   per allocation, and how much garbage it collects.

Limitations:

 - Arrays aren't fully supported yet. You can store an array of objects managed
   by the GC, but the GC can't manage the array itself.
 - Multi-threading isn't supported yet. There's one global GC.
 - Significant memory overhead, due to GC book-keeping.
 - Pointer writes are about 3x slower than raw pointer writes, again due to GC
   book-keeping. This is still about 3x *faster* than a std::shared_ptr though.
 - If you try hard enough, it's possible to construct pathological cases where
   the GC won't collect as much garbage. This is unlikely to occur in practice
   though.

The name "back-traced" refers to the way the GC searches the object graph, in
the reverse direction of traditional mark and sweep. This allows the GC to run
extremely incrementally, performing orders of magnitude more GC events with
orders of magnitude shorter pause times than other GCs.

## Usage
The GC works by wrapping raw pointers in a smart pointer: `Ptr<T>`. Any object
managed by the GC must only be referred to through `Ptr`s, otherwise the object
could be collected when you're still using it. Never store a raw pointer to a
managed object.

There are 2 kinds of `Ptr`, roots and non-roots:

 - If a `Ptr` is inside an object managed by the GC, it is a non-root.
 - If a `Ptr` is a local or global variable, it is a root.

This distinction is the most important thing to keep in mind when using the GC.

The job of the GC is to find all the objects reachable from the root `Ptr`s,
directly or indirectly, and delete everything else. So it's critically important
that `Ptr`s inside a managed object are not accidentally made root `Ptr`s,
otherwise the pointed to object will never be garbage collected.

From the GC's perspective, the difference between roots and non-roots is that:

 - Non-root `Ptr`s are constructed knowing the object they're sitting inside. In
   the constructor of any object managed by the GC, initialize all internal
   `Ptr`s with the `this` of the enclosing object.
 - Root `Ptr`s have `nullptr` as their enclosing object. Just construct these
   with the default constructior, or by assigning another `Ptr`.

```c++
class Child {
};

class MyObject {
  Ptr<Child> child;
  MyObject(Ptr<Child> c)
      : child(this) {  // Initialize non-root Ptr with this of enclosing object.
    child = c;  // Set the target of the Ptr separately.
  }
};

int main() {
  BTGC::init();

  Ptr<MyObject> o = Ptr<MyObject>::make(Ptr<Child>::make());

  BTGC::finish();
}
```

In the above example, `o` and `c` are root `Ptr`s, and `child` is a non-root. So
the key thing to note is that `child` is initialized with the `this` pointer of
the enclosing object. That's how the GC knows it's a non-root `Ptr`.
