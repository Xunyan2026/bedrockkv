// BedrockKV — SkipList: an ordered, insert-only, concurrency-ready skiplist.
//
// This is the heart of the future MemTable. Design notes:
//
//  * Ordered structure: keys are kept sorted, so range scans walk level 0.
//  * Template on Comparator: callers may store encoded composite keys.
//    The MemTable (stage 1) stores `user_key + tag` and orders by user key
//    ascending, tag descending. Two keys are "equal" iff neither is less
//    than the other under the comparator — NOT operator==.
//  * Insert-only: nodes are never removed while the list is alive (the
//    MemTable expresses deletion as tombstone entries). This is what makes
//    lock-free readers safe: a node, once linked, stays valid.
//  * Concurrency contract (leveldb-style, db/skiplist.h):
//      - at most ONE writer thread calls Insert() at any time — guaranteed
//        by the caller, not by this class;
//      - any number of readers may call Contains()/iterate concurrently
//        with the writer, without taking any lock;
//      - correctness relies on release/acquire memory ordering: a reader
//        can only discover a node through a release store (the writer
//        linking it into some level), and by that time every pointer of
//        the node that the reader can follow is already published.
//  * Memory: each node owns its key and an array of `height` atomic next
//    pointers. (leveldb packs nodes into an arena for fewer allocations —
//    a revisit-later optimization, tracked in docs/design notes.)
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace bedrockkv {

template <typename Comparator = std::less<std::string>>
class SkipListT {
 private:
  struct Node {
    const std::string key;
    const int height;
    // next[level] is the successor on that level. One atomic per level.
    const std::unique_ptr<std::atomic<Node*>[]> next;

    Node(std::string k, int h)
        : key(std::move(k)),
          height(h),
          next(std::make_unique<std::atomic<Node*>[]>(h)) {
      for (int i = 0; i < h; ++i) {
        next[i].store(nullptr, std::memory_order_relaxed);
      }
    }

    // Readers use acquire / writer uses release: whatever the writer
    // published into a node BEFORE linking it must be visible to readers
    // AFTER they load the link. acquire + release = the happens-before
    // edge that makes lock-free reading safe.
    Node* Next(int level) const {
      return next[level].load(std::memory_order_acquire);
    }
    void SetNext(int level, Node* x) {
      next[level].store(x, std::memory_order_release);
    }

    // Writer-only, relaxed variants. Used while a node is being wired up:
    // no reader can reach the node yet (nothing points at it).
    Node* RelaxedNext(int level) const {
      return next[level].load(std::memory_order_relaxed);
    }
    void RelaxedSetNext(int level, Node* x) {
      next[level].store(x, std::memory_order_relaxed);
    }
  };

 public:
  // Max tower height. With p = 1/4 the expected fraction of nodes at
  // level i is (1/4)^i, so 12 levels comfortably covers 4^12 ≈ 16M keys.
  static constexpr int kMaxLevel = 12;

  SkipListT() : head_(new Node("", kMaxLevel)) {
    nodes_.emplace_back(head_);  // sentinel dies with the list too
  }
  // Out-of-line is unnecessary now that Node is fully defined above, but
  // defaulted here to keep RAII ownership (nodes_) obvious.
  ~SkipListT() = default;

  SkipListT(const SkipListT&) = delete;
  SkipListT& operator=(const SkipListT&) = delete;

  // Inserts `key` in sorted position. Returns false and changes nothing
  // if an equivalent key is already present. Single-writer only.
  bool Insert(const std::string& key);

  // Lock-free point lookup: true iff an equivalent key is in the list.
  bool Contains(const std::string& key) const;

  // Approximate entry count. Relaxed atomic: under a concurrent Insert it
  // may briefly lag — fine for its purpose (triggering a MemTable flush).
  size_t ApproximateSize() const { return size_.load(std::memory_order_relaxed); }

  // Forward iterator over keys in ascending order (per the comparator).
  // Each thread must use its own iterator; an iterator is position-stable
  // and may safely run concurrently with Insert.
  class Iterator {
   public:
    explicit Iterator(const SkipListT* list) : list_(list), node_(nullptr) {}

    bool Valid() const { return node_ != nullptr; }
    // Precondition: Valid(). The reference stays valid while the list lives.
    const std::string& key() const { return node_->key; }
    void Next() { node_ = node_->Next(0); }              // Precondition: Valid()
    void SeekToFirst() { node_ = list_->head_->Next(0); }
    // Positions on the first key >= target (invalid if none).
    void Seek(const std::string& target) {
      node_ = list_->FindGreaterOrEqual(target, nullptr);
    }

   private:
    const SkipListT* list_;
    Node* node_;
  };

 private:
  static constexpr int kBranch = 4;  // level-up probability = 1/kBranch

  // Coin-flip tower height: geometric distribution with p = 1/4, capped
  // at kMaxLevel. thread_local RNG: no locking, per-thread streams.
  int RandomLevel() {
    thread_local std::mt19937 rng(std::random_device{}());
    thread_local std::uniform_int_distribution<int> coin(0, kBranch - 1);
    int level = 1;
    while (level < kMaxLevel && coin(rng) == 0) {
      ++level;
    }
    return level;
  }

  bool Equal(const std::string& a, const std::string& b) const {
    return !less_(a, b) && !less_(b, a);
  }

  // Returns the first node with key >= `key` under the comparator
  // (nullptr if none), and fills prev[0..kMaxLevel-1] with, per level,
  // the last node whose key is < `key` (the insertion predecessor).
  Node* FindGreaterOrEqual(const std::string& key, Node* prev[]) const {
    Node* x = head_;
    int level = kMaxLevel - 1;
    for (;;) {
      Node* next = x->Next(level);
      if (next != nullptr && less_(next->key, key)) {
        x = next;  // `next` is still < key: step forward on this level
      } else {
        // `next` is the first node >= key on this level: record the
        // predecessor and descend.
        if (prev != nullptr) {
          prev[level] = x;
        }
        if (level == 0) {
          return next;
        }
        --level;
      }
    }
  }

  // Sentinel: height kMaxLevel, empty key (never compared — we only ever
  // compare head_'s successors).
  Node* head_;
  // RAII ownership of every node: nodes are insert-linked and never
  // unlinked, so a flat owning vector is all the bookkeeping needed.
  std::vector<std::unique_ptr<Node>> nodes_;
  std::atomic<size_t> size_{0};
  Comparator less_;
};

template <typename Comparator>
bool SkipListT<Comparator>::Insert(const std::string& key) {
  Node* prev[kMaxLevel];
  Node* existing = FindGreaterOrEqual(key, prev);
  if (existing != nullptr && Equal(existing->key, key)) {
    return false;  // duplicate: leave the list untouched
  }

  const int height = RandomLevel();
  auto* node = new Node(key, height);
  nodes_.emplace_back(node);

  // Wire the new node bottom-up (leveldb's ordering, and it matters):
  //
  //   1. node.next[level] <- prev[level].next[level]  (relaxed, writer-only:
  //      nobody can see `node` yet — nothing links to it);
  //   2. prev[level].next[level] <- node  (RELEASE: the moment any reader
  //      observes `node` through this link, step 1 for this level — and all
  //      lower levels, done in earlier iterations — is guaranteed visible).
  //
  // A reader can only reach `node` on level L after step 2 ran for level L,
  // and by then node.next[0..L] are all published. It can never follow a
  // half-initialized pointer.
  for (int level = 0; level < height; ++level) {
    node->RelaxedSetNext(level, prev[level]->RelaxedNext(level));
    prev[level]->SetNext(level, node);
  }

  size_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

template <typename Comparator>
bool SkipListT<Comparator>::Contains(const std::string& key) const {
  const Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && Equal(x->key, key);
}

// The plain string-ordered skiplist used by tests and simple callers.
using SkipList = SkipListT<>;

}  // namespace bedrockkv
