// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __BTGC_H__
#define __BTGC_H__

#ifndef __BTGC_ReportCollection__
#define __BTGC_ReportCollection__
#endif

#ifndef __BTGC_Malloc__
#define __BTGC_Malloc__ malloc
#endif

#ifndef __BTGC_Free__
#define __BTGC_Free__ free
#endif

namespace btgc {

template <typename T>
class Array {
  T *a;
  size_t n, m;

 public:
  explicit Array(size_t initialCapacity = 4)
      : a(new T[initialCapacity]), n(0), m(initialCapacity) {}
  ~Array() { delete[] a; }
  Array(const Array<T> &box) = delete;
  Array<T> &operator=(const Array<T> &box) = delete;
  T &operator[](size_t i) { return a[i]; }
  size_t size() const { return n; }
  void del(size_t i) {
    --n;
    a[i] = a[n];
  }
  T pop() {
    --n;
    return a[n];
  }
  void clear() { n = 0; }
  void add(T t) {
    if (n >= m) {
      m <<= 1;
      T *b = new T[m];
      for (size_t i = 0; i < n; ++i) {
        b[i] = a[i];
      }
      delete[] a;
      a = b;
    }
    a[n] = t;
    ++n;
  }
};

template <class T>
class Ptr;

class BTGC {
  struct Block;
  struct Link {
    Link *next, *prev;  // Keep at start, to match Block.
    Block *from, *to;
    Link();
    Link(const Link &blocklink);
    explicit Link(Block *_from);
    Link(Block *_from, Block *_to);
    ~Link();
    void link(Block *_to);
    void *ptr() const;
    Link &operator=(const Link &blocklink);
  };

  struct Block {
    Link *next, *prev;     // Keep at start, to match Link.
    size_t id;             // Position in BTGC::blocks.
    void (*dtor)(void *);  // Destructor for the pointer.
    bool visited;
    Block(size_t _id, void (*_dtor)(void *))
        : next((Link *)this),
          prev((Link *)this),
          id(_id),
          dtor(_dtor),
          visited(false) {}
    void *ptr() const { return (void *)(this + 1); }
    void finalize() { dtor(ptr()); }
  };

  enum class Mode { initialize, search, clear, finalize, destroy };

  struct Rand {
    // Poor quality but super fast random number generator, based on FNV hash.
    static constexpr uint64_t basis = 0xcbf29ce484222325;
    static constexpr uint64_t prime = 0x100000001b3;
    size_t state = basis;
    size_t operator()(size_t n) {
      state = (state * prime) ^ basis;
      return state % n;
    }
  };

  Array<Block *> blocks;
  Rand rand;
  size_t totalLinks;
  Array<Block *> searchStack;
  Array<Block *> searchList;
  size_t pos;
  Block *searchBlk;
  Link *searchLink;
  Mode mode;
  double effort;

  static BTGC *inst;

  void finalize(Block *blk) {
    // Finalize and call all destructors, but don't actually free the memory.
    // This can never be called except when its entire sub-graph is being
    // deleted. Therefore, we don't need to worry about fixing its backlinks.
    size_t blkid = blk->id;
    blocks.del(blkid);
    blocks[blkid]->id = blkid;
    blk->finalize();
  }

  void del(Block *blk) {
    // Free the memory associated with the block. This assumes that
    // finalize(blk) has already been called.
    __BTGC_Free__((void *)blk);
  }

  void poke(Block *blk) {
    // If the block is visited (ie currently being processed) then the current
    // sub-graph that is being searched is not deletable.
    if (blk->visited && (mode == Mode::initialize || mode == Mode::search)) {
      mode = Mode::clear;
      pos = 0;
    }
  }

  void initializeStep() {
    if (searchStack.size() == 0) {
      if (blocks.size() == 0) {
        return;
      }
      searchBlk = blocks[rand(blocks.size())];
      searchBlk->visited = true;
      searchList.add(searchBlk);
    } else {
      searchBlk = searchStack.pop();
    }
    mode = Mode::search;
    searchLink = searchBlk->next;
  }

  void searchStep() {
    if (searchLink == (Link *)searchBlk) {
      if (searchStack.size() == 0) {
        // Root not reached, delete.
        mode = Mode::finalize;
        pos = 0;
        __BTGC_ReportCollection__(searchList.size());
        return;
      }
      mode = Mode::initialize;
      return;
    }

    Block *from = searchLink->from;
    if (from == nullptr) {
      // Root has been reached, don't delete.
      mode = Mode::clear;
      pos = 0;
      return;
    }
    if (!from->visited) {
      from->visited = true;
      searchList.add(from);
      searchStack.add(from);
    }
    searchLink = searchLink->next;
  }

  void clearStep() {
    searchList[pos]->visited = false;
    ++pos;
    if (pos >= searchList.size()) {
      mode = Mode::initialize;
      searchStack.clear();
      searchList.clear();
    }
  }

  void finalizeStep() {
    finalize(searchList[pos]);
    ++pos;
    if (pos >= searchList.size()) {
      mode = Mode::destroy;
      pos = 0;
    }
  }

  void destroyStep() {
    del(searchList[pos]);
    ++pos;
    if (pos >= searchList.size()) {
      mode = Mode::initialize;
      searchList.clear();
    }
  }

  void step() {
    switch (mode) {
      case Mode::initialize:
        initializeStep();
        break;
      case Mode::search:
        searchStep();
        break;
      case Mode::clear:
        clearStep();
        break;
      case Mode::finalize:
        finalizeStep();
        break;
      case Mode::destroy:
        destroyStep();
        break;
    }
  }

  void *alloc(size_t size, void (*dtor)(void *)) {
    // Garbage collect.
    if (blocks.size() > 0) {
      size_t steps = effort * ((2 * (totalLinks / blocks.size())) + 7);
      for (size_t i = 0; i < steps; ++i) step();
    }

    // Allocate block.
    // ptr -> [Block][User's data]
    void *ptr = __BTGC_Malloc__(size + sizeof(Block));
    Block *blk = new (ptr) Block(blocks.size(), dtor);
    blocks.add(blk);

    return (void *)(blk + 1);
  }

  BTGC(double e) : totalLinks(0), mode(Mode::initialize), effort(e) {}

  ~BTGC() {
    for (size_t i = 0; i < blocks.size(); ++i) {
      blocks[i]->finalize();
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
      del(blocks[i]);
    }
  }

 public:
  template <class T>
  friend class Ptr;

  static void init(double effort = 1) { inst = new BTGC(effort); }
  static void finish() { delete inst; }

  static size_t getNumBlocks() { return inst->blocks.size(); }
  static double getEffort() { return inst->effort; }
  static void setEffort(double e) { inst->effort = e; }
};

template <class T>
class Ptr {
  BTGC::Link link;
  static void dtor(void *p) { ((T *)p)->~T(); }

 public:
  Ptr() : link(nullptr) {}
  Ptr(const Ptr &ptr) : link(nullptr, ptr.link.to) {}
  template <class P>
  explicit Ptr(P *from) : link(((BTGC::Block *)from) - 1) {}
  template <class P>
  Ptr(P *from, T *to) : link(((BTGC::Block *)from) - 1, to) {}

  Ptr &operator=(const Ptr &ptr) {
    link.link(ptr.link.to);
    return *this;
  }

  Ptr &operator=(T *ptr) {
    link.link(ptr ? ((BTGC::Block *)ptr) - 1 : nullptr);
    return *this;
  }

  T *operator*() { return (T *)link.ptr(); }
  const T *operator*() const { return (const T *)link.ptr(); }
  T *operator->() { return (T *)link.ptr(); }
  const T *operator->() const { return (const T *)link.ptr(); }
  bool operator==(std::nullptr_t p) const { return link.to == nullptr; }
  bool operator!=(std::nullptr_t p) const { return link.to != nullptr; }
  bool operator==(const Ptr<T> &p) const { return link.to == p.link.to; }
  bool operator!=(const Ptr<T> &p) const { return link.to != p.link.to; }

  template <typename... Args>
  static Ptr make(Args... args) {
    Ptr ptr;
    ptr = new (BTGC::inst->alloc(sizeof(T), dtor)) T(args...);
    return ptr;
  }
};

BTGC *BTGC::inst = nullptr;

BTGC::Link::Link() {
  ++BTGC::inst->totalLinks;
  from = nullptr;
  to = nullptr;
  next = nullptr;
  prev = nullptr;
}

BTGC::Link::Link(const Link &blocklink) {
  ++BTGC::inst->totalLinks;
  from = nullptr;

  // Add this link to the front of to's list of backlinks.
  to = blocklink.to;
  if (to) {
    BTGC::inst->poke(to);
    next = to->next;
    prev = (Link *)to;
    prev->next = this;
    next->prev = this;
  } else {
    next = nullptr;
    prev = nullptr;
  }
}

BTGC::Link::Link(Block *_from, Block *_to) {
  ++BTGC::inst->totalLinks;
  from = _from;

  // Add this link to to's list of backlinks.
  to = _to;
  if (to) {
    BTGC::inst->poke(to);
    if (from) {
      // Insert at the back.
      next = (Link *)to;
      prev = to->prev;
    } else {
      // Insert at the front.
      next = to->next;
      prev = (Link *)to;
    }
    prev->next = this;
    next->prev = this;
  } else {
    next = nullptr;
    prev = nullptr;
  }
}

BTGC::Link::Link(Block *_from) {
  ++BTGC::inst->totalLinks;
  from = _from;
  to = nullptr;
  next = nullptr;
  prev = nullptr;
}

BTGC::Link::~Link() {
  --BTGC::inst->totalLinks;
  // Delete this link from to's list of backlinks.
  if (to) {
    BTGC::inst->poke(to);
    next->prev = prev;
    prev->next = next;
  }
}

void BTGC::Link::link(Block *_to) {
  // Delete this link from to's list of backlinks.
  if (to) {
    BTGC::inst->poke(to);
    next->prev = prev;
    prev->next = next;
  }

  // Add this link to to_'s list of backlinks.
  to = _to;
  if (to) {
    BTGC::inst->poke(to);
    if (from) {
      // Insert at the back.
      next = (Link *)to;
      prev = to->prev;
    } else {
      // Insert at the front.
      next = to->next;
      prev = (Link *)to;
    }
    prev->next = this;
    next->prev = this;
  } else {
    next = nullptr;
    prev = nullptr;
  }
}

void *BTGC::Link::ptr() const {
  BTGC::inst->poke(to);
  return to->ptr();
}

BTGC::Link &BTGC::Link::operator=(const Link &blocklink) {
  link(blocklink.to);
  return *this;
}

}  // namespace btgc

#endif  // __BTGC_H__
