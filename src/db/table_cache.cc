#include "db/table_cache.h"

#include <memory>

#include "db/filename.h"
#include "table/table.h"
#include "util/coding.h"

namespace lsm {

namespace {

void DeleteEntry(Slice, void* value) { delete static_cast<Table*>(value); }

void UnrefEntry(void* arg1, void* arg2) {
  static_cast<ShardedLRUCache*>(arg1)->Release(static_cast<ShardedLRUCache::Handle*>(arg2));
}

std::string CacheKey(uint64_t file_number) {
  std::string key;
  PutFixed64(&key, file_number);
  return key;
}

}  // namespace

TableCache::TableCache(const std::string& dbname, const TableOptions& options, Env* env,
                       int entries)
    : dbname_(dbname), options_(options), env_(env), cache_(entries) {}

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             ShardedLRUCache::Handle** handle) {
  const std::string key = CacheKey(file_number);
  *handle = cache_.Lookup(key);
  if (*handle != nullptr) return Status::OK();

  std::unique_ptr<RandomAccessFile> file;
  Status s = env_->NewRandomAccessFile(TableFileName(dbname_, file_number), &file);
  if (!s.ok()) return s;
  std::unique_ptr<Table> table;
  s = Table::Open(options_, std::move(file), file_number, file_size, &table);
  // Errors are not cached: if the cause is transient, or the file is
  // repaired, the next lookup recovers.
  if (!s.ok()) return s;
  *handle = cache_.Insert(key, table.release(), 1, &DeleteEntry);
  return Status::OK();
}

Iterator* TableCache::NewIterator(const ReadOptions& options, uint64_t file_number,
                                  uint64_t file_size) {
  ShardedLRUCache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) return NewErrorIterator(s);

  Table* table = static_cast<Table*>(cache_.Value(handle));
  Iterator* result = table->NewIterator(options);
  result->RegisterCleanup(&UnrefEntry, &cache_, handle);
  return result;
}

Status TableCache::Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
                       Slice k, void* arg, void (*handle_result)(void*, Slice, Slice)) {
  ShardedLRUCache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) return s;
  Table* table = static_cast<Table*>(cache_.Value(handle));
  s = table->InternalGet(options, k, arg, handle_result);
  cache_.Release(handle);
  return s;
}

void TableCache::Evict(uint64_t file_number) { cache_.Erase(CacheKey(file_number)); }

}  // namespace lsm
