#include "db/builder.h"

#include <memory>

#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "table/table_builder.h"

namespace lsm {

Status BuildTable(const std::string& dbname, Env* env, const TableOptions& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta) {
  meta->file_size = 0;
  iter->SeekToFirst();
  if (!iter->Valid()) return iter->status();

  const std::string fname = TableFileName(dbname, meta->number);
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(fname, &file);
  if (!s.ok()) return s;

  {
    TableBuilder builder(options, file.get());
    meta->smallest.DecodeFrom(iter->key());
    for (; iter->Valid(); iter->Next()) {
      // Copied every time: the key's storage may not outlive the Next().
      meta->largest.DecodeFrom(iter->key());
      builder.Add(iter->key(), iter->value());
    }

    s = builder.Finish();
    if (s.ok()) meta->file_size = builder.FileSize();
  }

  if (s.ok()) s = file->Sync();
  if (s.ok()) s = file->Close();
  file.reset();

  if (s.ok()) s = iter->status();
  if (s.ok()) {
    // Read it back through the cache: an unreadable table is caught here,
    // while its source is still in the memtable and the log.
    std::unique_ptr<Iterator> check(
        table_cache->NewIterator(ReadOptions(), meta->number, meta->file_size));
    s = check->status();
  }

  if (!s.ok()) {
    (void)env->RemoveFile(fname);
    meta->file_size = 0;
  }
  return s;
}

}  // namespace lsm
