// BedrockKV — SkipList: an ordered, insert-only, concurrency-ready skiplist.
//
// This is the heart of the future MemTable. Design notes:
//
//  * Ordered structure: keys are kept sorted, so range scans walk level 0.
//  * Insert-only: nodes are never removed while the list is alive (the
//    MemTable expresses deletion as tombstone entries, stage 1). This is
//    what makes lock-free readers safe: a node, once linked, stays valid.
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
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace bedrockkv {

class SkipList {
 private:
  struct Node;  // defined in skiplist.cpp; readers only pass Node* around

 public:
  // Max tower height. With p = 1/4 the expected fraction of nodes at
  // level i is (1/4)^i, so 12 levels comfortably covers 4^12 ≈ 16M keys.
  static constexpr int kMaxLevel = 12;

  SkipList();
  // Out-of-line: Node is incomplete here, and the node-owning vector needs
  // a complete type to destroy its elements.
  ~SkipList();

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Inserts `key` in sorted position. Returns false and changes nothing
  // if the key is already present. Single-writer only (see contract above).
  bool Insert(const std::string& key);

  // Lock-free point lookup: true iff `key` is currently in the list.
  bool Contains(const std::string& key) const;

  // Approximate entry count. Relaxed atomic: under a concurrent Insert it
  // may briefly lag — fine for its purpose (triggering a MemTable flush).
  size_t ApproximateSize() const { return size_.load(std::memory_order_relaxed); }

  // Forward iterator over keys in ascending order. Each thread must use
  // its own iterator; an iterator is position-stable and may safely run
  // concurrently with Insert (it simply walks whatever it can see).
  class Iterator {
   public:
    explicit Iterator(const SkipList* list);

    bool Valid() const;
    // Precondition: Valid(). The reference stays valid while the list lives.
    const std::string& key() const;
    void Next();                     // Precondition: Valid()
    void SeekToFirst();
    // Positions on the first key >= target (or makes it invalid if none).
    void Seek(const std::string& target);

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  static constexpr int kBranch = 4;  // level-up probability = 1/kBranch

  // Coin-flip tower height for a new node: geometric distribution with
  // p = 1/4, capped at kMaxLevel. thread_local RNG: no locking needed and
  // each writer thread (in tests) keeps an independent stream.
  int RandomLevel();

  // Returns the first node with key >= `key` (nullptr if none), and fills
  // prev[0..kMaxLevel-1] with, per level, the last node whose key is < `key`
  // (i.e. the insertion predecessor on that level).
  Node* FindGreaterOrEqual(const std::string& key, Node* prev[]) const;

  // Sentinel: height kMaxLevel, empty key (never compared — we only ever
  // compare head_'s successors). Owns nothing but its own tower.
  Node* head_;

  // The list keeps ownership of every node so destruction is RAII-clean:
  // nodes are inserted-linked and never unlinked, so no other bookkeeping
  // is possible or needed.
  std::vector<std::unique_ptr<Node>> nodes_;

  std::atomic<size_t> size_{0};
};

}  // namespace bedrockkv
