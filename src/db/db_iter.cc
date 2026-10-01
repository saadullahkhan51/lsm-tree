#include "db/db_iter.h"

#include <cassert>
#include <memory>
#include <string>

namespace lsm {

namespace {

// The internal iterator holds every version of every key:
//
//   a@9:value  a@4:value  b@7:deletion  b@2:value  c@5:value
//
// Entries for one user key are adjacent, newest first. Going forward, the
// first entry at or below the read sequence decides the key: if it is a
// value it is yielded, and everything else for that key, being older, is
// skipped; if it is a tombstone the whole key is skipped.
//
// Going backward the versions of a key arrive oldest first, so the entry
// that decides is the last one seen before the key changes. The iterator
// has to walk through all of a key's versions, remembering the newest
// visible one, before it knows what to yield, and so it ends up parked on
// the previous key's last entry. That is why the two directions differ:
//
//   kForward: iter_ is at the internal entry that yields key() and value().
//   kReverse: iter_ is just before all entries for key(); the key and value
//             being yielded are held in saved_key_ and saved_value_.
class DBIter : public Iterator {
 public:
  DBIter(const Comparator* user_comparator, Iterator* iter, SequenceNumber sequence)
      : user_comparator_(user_comparator), iter_(iter), sequence_(sequence) {}

  bool Valid() const override { return valid_; }

  Slice key() const override {
    assert(valid_);
    return direction_ == kForward ? ExtractUserKey(iter_->key()) : Slice(saved_key_);
  }

  Slice value() const override {
    assert(valid_);
    return direction_ == kForward ? iter_->value() : Slice(saved_value_);
  }

  Status status() const override { return status_.ok() ? iter_->status() : status_; }

  void Next() override;
  void Prev() override;
  void Seek(Slice target) override;
  void SeekToFirst() override;
  void SeekToLast() override;

 private:
  enum Direction { kForward, kReverse };

  void FindNextUserEntry(bool skipping, std::string* skip);
  void FindPrevUserEntry();
  bool ParseKey(ParsedInternalKey* key);

  void Invalidate() {
    valid_ = false;
    saved_key_.clear();
    ClearSavedValue();
  }

  void ClearSavedValue() {
    if (saved_value_.capacity() > (1 << 20)) {
      std::string().swap(saved_value_);  // do not hold on to a huge buffer
    } else {
      saved_value_.clear();
    }
  }

  const Comparator* const user_comparator_;
  const std::unique_ptr<Iterator> iter_;
  const SequenceNumber sequence_;

  Status status_;
  std::string saved_key_;    // kReverse: the current key. kForward: scratch
  std::string saved_value_;  // kReverse: the current value
  Direction direction_ = kForward;
  bool valid_ = false;
};

bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  if (ParseInternalKey(iter_->key(), ikey)) return true;
  // The entry passed its block's checksum, so this is damage in memory or
  // a bug. Stop rather than step over it: the entry could have been the
  // one that decides its key.
  status_ = Status::Corruption("corrupted internal key in iterator");
  return false;
}

void DBIter::Next() {
  assert(valid_);

  if (direction_ == kReverse) {
    direction_ = kForward;
    // iter_ is just before the entries for key(). Step into them; the
    // skipping below then passes over all of them, since saved_key_ already
    // holds the key to skip.
    if (!iter_->Valid()) {
      iter_->SeekToFirst();
    } else {
      iter_->Next();
    }
    if (!iter_->Valid()) {
      Invalidate();
      return;
    }
  } else {
    // Remember the current key so that its older versions are skipped.
    saved_key_.assign(ExtractUserKey(iter_->key()));
    iter_->Next();
    if (!iter_->Valid()) {
      Invalidate();
      return;
    }
  }

  FindNextUserEntry(true, &saved_key_);
}

// Moves forward to the next entry that should be yielded. If skipping,
// entries for user keys <= *skip are passed over.
void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {
  assert(iter_->Valid());
  assert(direction_ == kForward);
  do {
    ParsedInternalKey ikey;
    if (!ParseKey(&ikey)) {
      Invalidate();
      return;
    }
    if (ikey.sequence <= sequence_) {
      switch (ikey.type) {
        case kTypeDeletion:
          // The key is deleted as of this read. Skip its older versions.
          skip->assign(ikey.user_key);
          skipping = true;
          break;
        case kTypeValue:
          if (skipping && user_comparator_->Compare(ikey.user_key, *skip) <= 0) {
            // Hidden by a newer value or a tombstone already seen.
          } else {
            valid_ = true;
            saved_key_.clear();
            return;
          }
          break;
      }
    }
    iter_->Next();
  } while (iter_->Valid());
  Invalidate();
}

void DBIter::Prev() {
  assert(valid_);

  if (direction_ == kForward) {
    // iter_ is at the entry for key(). Back up until just before all of
    // that key's entries, which is where reverse scanning starts from.
    assert(iter_->Valid());
    saved_key_.assign(ExtractUserKey(iter_->key()));
    while (true) {
      iter_->Prev();
      if (!iter_->Valid()) {
        Invalidate();
        return;
      }
      if (user_comparator_->Compare(ExtractUserKey(iter_->key()), saved_key_) < 0) {
        break;
      }
    }
    direction_ = kReverse;
  }

  FindPrevUserEntry();
}

// Scans backward through the versions of the previous key (and of keys
// before it, if they turn out to be deleted) and stops with iter_ just
// before the entries of the key to yield.
void DBIter::FindPrevUserEntry() {
  assert(direction_ == kReverse);

  // The type of the newest visible entry seen so far for the key being
  // assembled. kTypeDeletion also means "none yet".
  ValueType value_type = kTypeDeletion;
  if (iter_->Valid()) {
    do {
      ParsedInternalKey ikey;
      if (!ParseKey(&ikey)) {
        Invalidate();
        direction_ = kForward;
        return;
      }
      if (ikey.sequence <= sequence_) {
        if (value_type != kTypeDeletion &&
            user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
          // Reached an earlier key, and the key just assembled has a
          // visible value: that is the one to yield.
          break;
        }
        // Either a newer version of the key being assembled, which
        // overrides what was saved, or the first version of an earlier key.
        value_type = ikey.type;
        if (value_type == kTypeDeletion) {
          saved_key_.clear();
          ClearSavedValue();
        } else {
          saved_key_.assign(ExtractUserKey(iter_->key()));
          saved_value_.assign(iter_->value());
        }
      }
      iter_->Prev();
    } while (iter_->Valid());
  }

  if (value_type == kTypeDeletion) {
    // Ran off the front without finding a live key.
    Invalidate();
    direction_ = kForward;
  } else {
    valid_ = true;
  }
}

void DBIter::Seek(Slice target) {
  direction_ = kForward;
  ClearSavedValue();
  saved_key_.clear();
  // The newest version of target visible at sequence_, or whatever follows.
  AppendInternalKey(&saved_key_, ParsedInternalKey{target, sequence_, kValueTypeForSeek});
  iter_->Seek(saved_key_);
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_ /* scratch */);
  } else {
    Invalidate();
  }
}

void DBIter::SeekToFirst() {
  direction_ = kForward;
  ClearSavedValue();
  iter_->SeekToFirst();
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_ /* scratch */);
  } else {
    Invalidate();
  }
}

void DBIter::SeekToLast() {
  direction_ = kReverse;
  ClearSavedValue();
  iter_->SeekToLast();
  FindPrevUserEntry();
}

}  // namespace

Iterator* NewDBIterator(const Comparator* user_comparator, Iterator* internal_iter,
                        SequenceNumber sequence) {
  return new DBIter(user_comparator, internal_iter, sequence);
}

}  // namespace lsm
