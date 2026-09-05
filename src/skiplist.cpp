#include "bedrockkv/skiplist.h"

#include <utility>

namespace bedrockkv {

// ---- Node ----------------------------------------------------------------

struct SkipList::Node {
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

  // Readers use acquire: whatever the writer published into this node
  // BEFORE linking it (release store on the predecessor) must be visible
  // to us AFTER we load the link (acquire). acquire + release = the
  // happens-before edge that makes lock-free reading safe.
  Node* Next(int level) const {
    return next[level].load(std::memory_order_acquire);
  }
  void SetNext(int level, Node* x) {
    next[level].store(x, std::memory_order_release);
  }

  // Writer-only, relaxed variants. Used while a node is being wired up:
  // no reader can reach the node yet (nothing points at it), so ordering
  // is irrelevant here — only the writer touches these fields.
  Node* RelaxedNext(int level) const {
    return next[level].load(std::memory_order_relaxed);
  }
  void RelaxedSetNext(int level, Node* x) {
    next[level].store(x, std::memory_order_relaxed);
  }
};

// ---- Lifecycle ------------------------------------------------------------

SkipList::SkipList() : head_(new Node("", kMaxLevel)) {
  nodes_.emplace_back(head_);  // sentinel dies with the list too
}

SkipList::~SkipList() = default;

int SkipList::RandomLevel() {
  thread_local std::mt19937 rng(std::random_device{}());
  thread_local std::uniform_int_distribution<int> coin(0, kBranch - 1);

  int level = 1;
  // Each level-up wins with probability 1/4 — the "coin flip".
  while (level < kMaxLevel && coin(rng) == 0) {
    ++level;
  }
  return level;
}

// ---- Search ---------------------------------------------------------------

SkipList::Node* SkipList::FindGreaterOrEqual(const std::string& key,
                                             Node* prev[]) const {
  Node* x = head_;
  int level = kMaxLevel - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next != nullptr && next->key < key) {
      x = next;  // `next` is still < key: step forward on this level
    } else {
      // `next` is the first node >= key on this level: record predecessor
      // and descend.
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

bool SkipList::Contains(const std::string& key) const {
  const Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && x->key == key;
}

// ---- Insert (single writer) ----------------------------------------------

bool SkipList::Insert(const std::string& key) {
  Node* prev[SkipList::kMaxLevel];
  Node* existing = FindGreaterOrEqual(key, prev);
  if (existing != nullptr && existing->key == key) {
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

// ---- Iterator -------------------------------------------------------------

SkipList::Iterator::Iterator(const SkipList* list)
    : list_(list), node_(nullptr) {}

bool SkipList::Iterator::Valid() const { return node_ != nullptr; }

const std::string& SkipList::Iterator::key() const { return node_->key; }

void SkipList::Iterator::Next() { node_ = node_->Next(0); }

void SkipList::Iterator::SeekToFirst() { node_ = list_->head_->Next(0); }

void SkipList::Iterator::Seek(const std::string& target) {
  node_ = list_->FindGreaterOrEqual(target, nullptr);
}

}  // namespace bedrockkv
