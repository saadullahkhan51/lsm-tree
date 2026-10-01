#include "table/table.h"

#include <string>

#include "db/dbformat.h"
#include "table/block.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/two_level_iterator.h"
#include "util/cache.h"
#include "util/coding.h"

namespace lsm {

struct Table::Rep {
  TableOptions options;
  std::unique_ptr<RandomAccessFile> file;
  uint64_t file_number;
  uint64_t file_size;
  std::unique_ptr<Block> index_block;
  BlockContents filter;  // empty if the table has none
};

Table::Table(std::unique_ptr<Rep> rep) : rep_(std::move(rep)) {}

Table::~Table() = default;

Status Table::Open(const TableOptions& options, std::unique_ptr<RandomAccessFile> file,
                   uint64_t file_number, uint64_t file_size, std::unique_ptr<Table>* table) {
  table->reset();
  if (file_size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  Status s = file->Read(file_size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input,
                        footer_space);
  if (!s.ok()) return s;
  Footer footer;
  s = footer.DecodeFrom(footer_input);
  if (!s.ok()) return s;

  BlockContents index_contents;
  s = ReadBlock(file.get(), file_size, footer.index_handle(), &index_contents);
  if (!s.ok()) return s;

  // The metaindex maps names to blocks. The only entry this code knows is
  // the bloom filter; a table without one is fine.
  BlockContents metaindex_contents;
  s = ReadBlock(file.get(), file_size, footer.metaindex_handle(), &metaindex_contents);
  if (!s.ok()) return s;
  BlockContents filter;
  {
    const Block metaindex(std::move(metaindex_contents));
    std::unique_ptr<Iterator> iter(metaindex.NewIterator(BytewiseComparator()));
    iter->Seek(kFilterMetaKey);
    if (iter->Valid() && iter->key() == kFilterMetaKey) {
      Slice handle_value = iter->value();
      BlockHandle filter_handle;
      s = filter_handle.DecodeFrom(&handle_value);
      if (s.ok()) s = ReadBlock(file.get(), file_size, filter_handle, &filter);
    } else {
      s = iter->status();
    }
    if (!s.ok()) return s;
  }

  auto rep = std::make_unique<Rep>();
  rep->options = options;
  rep->file = std::move(file);
  rep->file_number = file_number;
  rep->file_size = file_size;
  rep->index_block = std::make_unique<Block>(std::move(index_contents));
  rep->filter = std::move(filter);
  table->reset(new Table(std::move(rep)));
  return Status::OK();
}

namespace {

void DeleteCachedBlock(Slice, void* value) { delete static_cast<Block*>(value); }

void DeleteBlock(void* arg, void*) { delete static_cast<Block*>(arg); }

void ReleaseBlock(void* arg, void* h) {
  static_cast<ShardedLRUCache*>(arg)->Release(static_cast<ShardedLRUCache::Handle*>(h));
}

}  // namespace

Iterator* Table::BlockReader(const ReadOptions& options, Slice index_value) const {
  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  if (!s.ok()) return NewErrorIterator(s);

  ShardedLRUCache* block_cache = rep_->options.block_cache;
  const Block* block = nullptr;
  ShardedLRUCache::Handle* cache_handle = nullptr;

  if (block_cache != nullptr) {
    char cache_key[16];
    EncodeFixed64(cache_key, rep_->file_number);
    EncodeFixed64(cache_key + 8, handle.offset());
    const Slice key(cache_key, sizeof(cache_key));
    cache_handle = block_cache->Lookup(key);
    if (cache_handle != nullptr) {
      block = static_cast<Block*>(block_cache->Value(cache_handle));
    } else {
      BlockContents contents;
      s = ReadBlock(rep_->file.get(), rep_->file_size, handle, &contents);
      if (!s.ok()) return NewErrorIterator(s);
      Block* fresh = new Block(std::move(contents));
      block = fresh;
      if (options.fill_cache) {
        cache_handle = block_cache->Insert(key, fresh, fresh->size(), &DeleteCachedBlock);
      }
    }
  } else {
    BlockContents contents;
    s = ReadBlock(rep_->file.get(), rep_->file_size, handle, &contents);
    if (!s.ok()) return NewErrorIterator(s);
    block = new Block(std::move(contents));
  }

  Iterator* iter = block->NewIterator(rep_->options.comparator);
  if (cache_handle != nullptr) {
    // The cache owns the block; the iterator pins it.
    iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
  } else {
    iter->RegisterCleanup(&DeleteBlock, const_cast<Block*>(block), nullptr);
  }
  return iter;
}

Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      [this](const ReadOptions& read_options, Slice index_value) {
        return BlockReader(read_options, index_value);
      },
      options);
}

Status Table::InternalGet(const ReadOptions& options, Slice key, void* arg,
                          void (*handle_result)(void*, Slice, Slice)) const {
  // Filter first: it is in memory and answers most lookups for keys that
  // are not in this table without touching the index or the disk.
  if (rep_->filter.size > 0 &&
      !FilterKeyMayMatch(Slice(rep_->filter.data.get(), rep_->filter.size), ExtractUserKey(key))) {
    return Status::OK();
  }

  std::unique_ptr<Iterator> index_iter(rep_->index_block->NewIterator(rep_->options.comparator));
  index_iter->Seek(key);
  if (!index_iter->Valid()) return index_iter->status();

  std::unique_ptr<Iterator> block_iter(BlockReader(options, index_iter->value()));
  block_iter->Seek(key);
  if (block_iter->Valid()) {
    handle_result(arg, block_iter->key(), block_iter->value());
  }
  return block_iter->status();
}

}  // namespace lsm
