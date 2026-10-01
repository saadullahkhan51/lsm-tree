#include "util/cache.h"

#include <cassert>
#include <mutex>
#include <string>
#include <unordered_map>

#include "util/hash.h"

namespace lsm {

namespace {

// An entry. refs counts the cache's own reference (while in_cache) plus one
// per outstanding Handle. Each cached entry sits on exactly one of two
// circular lists:
//   in_use_: pinned by at least one client (refs >= 2); never evicted.
//   lru_:    referenced only by the cache (refs == 1); evicted oldest-first.
struct LRUHandle {
  void* value;
  ShardedLRUCache::Deleter deleter;
  LRUHandle* next;
  LRUHandle* prev;
  size_t charge;
  uint32_t refs;
  bool in_cache;
  std::string key;
};

void ListRemove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}

// Makes e the newest entry of the list.
void ListAppend(LRUHandle* list, LRUHandle* e) {
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}

}  // namespace

class ShardedLRUCache::Shard {
 public:
  Shard() {
    lru_.next = lru_.prev = &lru_;
    in_use_.next = in_use_.prev = &in_use_;
  }

  ~Shard() {
    assert(in_use_.next == &in_use_);  // a caller is still holding a Handle
    for (LRUHandle* e = lru_.next; e != &lru_;) {
      LRUHandle* next = e->next;
      assert(e->in_cache && e->refs == 1);
      e->in_cache = false;
      Unref(e);
      e = next;
    }
  }

  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  LRUHandle* Insert(Slice key, void* value, size_t charge, Deleter deleter) {
    std::lock_guard lock(mutex_);

    auto* e = new LRUHandle{value, deleter, nullptr, nullptr, charge, 1, false, std::string(key)};
    if (capacity_ > 0) {
      e->refs++;  // the cache's reference
      e->in_cache = true;
      ListAppend(&in_use_, e);
      usage_ += charge;

      LRUHandle* old = nullptr;
      if (auto it = table_.find(key); it != table_.end()) {
        old = it->second;
        table_.erase(it);
      }
      table_.emplace(Slice(e->key), e);
      FinishErase(old);
    } else {
      // Caching is off; the entry lives only as long as the returned Handle.
      e->next = e->prev = e;
    }

    while (usage_ > capacity_ && lru_.next != &lru_) {
      LRUHandle* oldest = lru_.next;
      table_.erase(oldest->key);
      FinishErase(oldest);
    }
    return e;
  }

  LRUHandle* Lookup(Slice key) {
    std::lock_guard lock(mutex_);
    auto it = table_.find(key);
    if (it == table_.end()) return nullptr;
    Ref(it->second);
    return it->second;
  }

  void Release(LRUHandle* e) {
    std::lock_guard lock(mutex_);
    Unref(e);
  }

  void Erase(Slice key) {
    std::lock_guard lock(mutex_);
    if (auto it = table_.find(key); it != table_.end()) {
      LRUHandle* e = it->second;
      table_.erase(it);
      FinishErase(e);
    }
  }

  size_t TotalCharge() {
    std::lock_guard lock(mutex_);
    return usage_;
  }

 private:
  void Ref(LRUHandle* e) {
    if (e->refs == 1 && e->in_cache) {
      ListRemove(e);
      ListAppend(&in_use_, e);
    }
    e->refs++;
  }

  void Unref(LRUHandle* e) {
    assert(e->refs > 0);
    e->refs--;
    if (e->refs == 0) {
      assert(!e->in_cache);
      e->deleter(e->key, e->value);
      delete e;
    } else if (e->in_cache && e->refs == 1) {
      // No client references remain; it becomes evictable.
      ListRemove(e);
      ListAppend(&lru_, e);
    }
  }

  // e has already been removed from table_. Drops the cache's reference.
  void FinishErase(LRUHandle* e) {
    if (e == nullptr) return;
    assert(e->in_cache);
    ListRemove(e);
    e->in_cache = false;
    usage_ -= e->charge;
    Unref(e);
  }

  std::mutex mutex_;
  size_t capacity_ = 0;
  size_t usage_ = 0;
  LRUHandle lru_;
  LRUHandle in_use_;
  // Keys view the std::string owned by each entry.
  std::unordered_map<Slice, LRUHandle*> table_;
};

ShardedLRUCache::ShardedLRUCache(size_t capacity) : shards_(new Shard[kNumShards]) {
  const size_t per_shard = (capacity + kNumShards - 1) / kNumShards;
  for (int i = 0; i < kNumShards; i++) {
    shards_[i].SetCapacity(per_shard);
  }
}

ShardedLRUCache::~ShardedLRUCache() = default;

ShardedLRUCache::Shard& ShardedLRUCache::ShardFor(Slice key) {
  return shards_[Hash(key.data(), key.size(), 0) >> 28];
}

ShardedLRUCache::Handle* ShardedLRUCache::Insert(Slice key, void* value, size_t charge,
                                                 Deleter deleter) {
  return reinterpret_cast<Handle*>(ShardFor(key).Insert(key, value, charge, deleter));
}

ShardedLRUCache::Handle* ShardedLRUCache::Lookup(Slice key) {
  LRUHandle* e = ShardFor(key).Lookup(key);
  (e != nullptr ? hits_ : misses_).fetch_add(1, std::memory_order_relaxed);
  return reinterpret_cast<Handle*>(e);
}

void ShardedLRUCache::Release(Handle* handle) {
  auto* e = reinterpret_cast<LRUHandle*>(handle);
  ShardFor(e->key).Release(e);
}

void* ShardedLRUCache::Value(Handle* handle) { return reinterpret_cast<LRUHandle*>(handle)->value; }

void ShardedLRUCache::Erase(Slice key) { ShardFor(key).Erase(key); }

size_t ShardedLRUCache::TotalCharge() const {
  size_t total = 0;
  for (int i = 0; i < kNumShards; i++) {
    total += shards_[i].TotalCharge();
  }
  return total;
}

}  // namespace lsm
