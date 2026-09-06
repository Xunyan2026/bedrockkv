// BedrockKV — Iterator: the uniform traversal interface over every
// sorted source (MemTable, SSTable, merges of both).
//
// Contract (leveldb-style):
//   * keys/values are INTERNAL keys (user_key ++ tag u64 LE) ordered by
//     InternalKeyComparator — user key ascending, tag (seq) descending;
//   * an iterator is positioned or invalid; key()/value() are only
//     callable while Valid();
//   * the underlying data must outlive the iterator (all our sources are
//     immutable or refcounted, so iterators never see concurrent edits);
//   * iterators see a frozen view — no snapshot semantics needed at this
//     stage because nothing mutates in place.
#pragma once

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "bedrockkv/block.h"
#include "bedrockkv/status.h"

namespace bedrockkv {

class Iterator {
 public:
  virtual ~Iterator() = default;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  // Positions on the first entry with internal key >= `target`.
  virtual void Seek(std::string_view target) = 0;
  // Precondition: Valid().
  virtual void Next() = 0;
  virtual std::string_view key() const = 0;    // internal key
  virtual std::string_view value() const = 0;  // payload (empty for tombstones)
};

// k-way merge over ordered children. Uses a linear scan for the minimum
// instead of a heap: k is small (L0 files + 2 levels ≈ dozens at most),
// so O(k) per step beats heap bookkeeping and stays trivially correct.
class MergingIterator : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return current_ != nullptr; }

  void SeekToFirst() override {
    for (auto& c : children_) {
      c->SeekToFirst();
    }
    FindSmallest();
  }

  void Seek(std::string_view target) override {
    for (auto& c : children_) {
      c->Seek(target);
    }
    FindSmallest();
  }

  void Next() override {
    if (current_ != nullptr) {
      current_->Next();
    }
    FindSmallest();
  }

  std::string_view key() const override { return current_->key(); }
  std::string_view value() const override { return current_->value(); }

 private:
  void FindSmallest() {
    current_ = nullptr;
    for (const auto& c : children_) {
      if (c->Valid() &&
          (current_ == nullptr || less_(c->key(), current_->key()))) {
        current_ = c.get();
      }
    }
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_ = nullptr;
  InternalKeyComparator less_;
};

}  // namespace bedrockkv
