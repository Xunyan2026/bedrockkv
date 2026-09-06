// BedrockKV — a sharded LRU cache used for value-log reads (stage 3) and
// reusable later for SST block caching.
//
// Why a cache at all: value separation moves bytes out of the LSM into an
// append-only vLog, so a point lookup that misses every memtable pays an
// extra pread for the value. Under a zipfian access distribution a small
// LRU absorbs most of those reads.
//
// Why sharded: one global mutex would serialize Gets across cores; N
// shards hash the key space so typical access touches one independent
// lock. 16 shards × small critical sections kept the stage-2 read path
// (which is fully lock-free) essentially unaffected.
//
// Weight-based capacity: entries differ wildly in size (a 1 KiB value vs
// a future 4 KiB block), so capacity is a caller-supplied weight sum
// (e.g. bytes) rather than an entry count. The weight function must be
// pure and cheap — it runs while the shard mutex is held.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrockkv {

template <typename Key, typename Value, typename Hash>
class ShardedLruCache {
 public:
  using WeightFn = std::function<size_t(const Value&)>;

  ShardedLruCache(size_t shards, size_t max_total_weight, WeightFn weight)
      : shards_(shards), max_total_weight_(max_total_weight), weight_(std::move(weight)) {
    shard_.reserve(shards);
    for (size_t i = 0; i < shards; ++i) {
      shard_.emplace_back(std::make_unique<Shard>());
    }
  }

  ShardedLruCache(const ShardedLruCache&) = delete;
  ShardedLruCache& operator=(const ShardedLruCache&) = delete;

  // Looks `key` up and, on hit, copies the value out and promotes it to
  // most-recently-used. Returns false on miss.
  bool Get(const Key& key, Value* value) {
    Shard& s = *shard_[ShardOf(key)];
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.map.find(key);
    if (it == s.map.end()) {
      return false;
    }
    s.lru.splice(s.lru.begin(), s.lru, it->second);  // move to MRU position
    *value = it->second->second;
    return true;
  }

  // Inserts or overwrites `key`, evicting least-recently-used entries
  // until the shard's weight share fits again.
  void Put(const Key& key, Value value) {
    const size_t w = weight_(value);
    if (w > max_total_weight_) {
      return;  // a single entry larger than the whole cache: don't cache
    }
    Shard& s = *shard_[ShardOf(key)];
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.map.find(key);
    if (it != s.map.end()) {
      s.weight -= weight_(it->second->second);
      it->second->second = std::move(value);
      s.weight += w;
      s.lru.splice(s.lru.begin(), s.lru, it->second);
    } else {
      s.lru.emplace_front(key, std::move(value));
      s.map[key] = s.lru.begin();
      s.weight += w;
    }
    while (s.weight > max_total_weight_ && !s.lru.empty()) {
      const auto victim = std::prev(s.lru.end());
      s.weight -= weight_(victim->second);
      s.map.erase(victim->first);
      s.lru.erase(victim);
    }
  }

  void Clear() {
    for (const auto& s : shard_) {
      std::lock_guard<std::mutex> lk(s->mu);
      s->lru.clear();
      s->map.clear();
      s->weight = 0;
    }
  }

 private:
  struct Shard {
    std::mutex mu;
    std::list<std::pair<Key, Value>> lru;                    // front = MRU
    std::unordered_map<Key, typename std::list<std::pair<Key, Value>>::iterator, Hash> map;
    size_t weight = 0;
  };

  size_t ShardOf(const Key& key) const { return Hash{}(key) % shards_; }

  std::vector<std::unique_ptr<Shard>> shard_;
  size_t shards_;
  size_t max_total_weight_;
  WeightFn weight_;
};

}  // namespace bedrockkv
