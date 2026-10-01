#pragma once

#include <string>

#include "lsm/env.h"
#include "lsm/iterator.h"
#include "lsm/status.h"
#include "table/table_options.h"

namespace lsm {

struct FileMetaData;
class TableCache;

// Writes the contents of iter to the table file named by meta->number and
// fills in the rest of *meta. The file's contents are synced; making its
// directory entry durable is left to whoever records the file in the
// manifest. If iter is empty, no file is created and meta->file_size is 0.
// On failure no file is left behind.
Status BuildTable(const std::string& dbname, Env* env, const TableOptions& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta);

}  // namespace lsm
