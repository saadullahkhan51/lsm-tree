#include "db/compaction_check.h"

#include <string>

namespace lsm {

namespace {

struct Entry {
  SequenceNumber sequence;
  ValueType type;
  std::string value;

  bool operator==(const Entry&) const = default;
};

// Reads every entry for the user key the iterator is on into *group,
// newest first, leaving the iterator on the next user key. If the iterator
// is exhausted, or on a different key than user_key, the group is empty.
Status ReadGroup(const Comparator* ucmp, Iterator* iter, Slice user_key,
                 std::vector<Entry>* group) {
  group->clear();
  while (iter->Valid()) {
    ParsedInternalKey parsed;
    if (!ParseInternalKey(iter->key(), &parsed)) {
      return Status::Corruption("compaction check: unparseable key");
    }
    if (ucmp->Compare(parsed.user_key, user_key) != 0) break;
    group->push_back({parsed.sequence, parsed.type, std::string(iter->value())});
    iter->Next();
  }
  return iter->status();
}

// The entry a read at read_point sees: the newest one at or below it.
const Entry* Visible(const std::vector<Entry>& group, SequenceNumber read_point) {
  for (const Entry& e : group) {
    if (e.sequence <= read_point) return &e;
  }
  return nullptr;
}

std::string Describe(Slice user_key, const std::string& what) {
  return "compaction changed what readers see for key '" + std::string(user_key) + "': " + what;
}

}  // namespace

Status CheckCompactionOutput(const Comparator* ucmp, Iterator* input, Iterator* output,
                             const std::vector<SequenceNumber>& read_points,
                             const std::function<bool(Slice user_key)>& key_may_exist_below,
                             std::string* violation) {
  violation->clear();
  // Records a difference and ends the check.
  auto differ = [&](Slice user_key, const std::string& what) {
    *violation = Describe(user_key, what);
    return Status::OK();
  };

  input->SeekToFirst();
  output->SeekToFirst();

  std::vector<Entry> in_group, out_group;
  std::string user_key;
  while (input->Valid()) {
    ParsedInternalKey parsed;
    if (!ParseInternalKey(input->key(), &parsed)) {
      return Status::Corruption("compaction check: unparseable input key");
    }
    user_key.assign(parsed.user_key);

    // An output key that sorts before every remaining input key came from
    // nowhere.
    if (output->Valid() && ucmp->Compare(ExtractUserKey(output->key()), user_key) < 0) {
      return differ(ExtractUserKey(output->key()), "the output has a key the input lacks");
    }

    Status s = ReadGroup(ucmp, input, user_key, &in_group);
    if (s.ok()) s = ReadGroup(ucmp, output, user_key, &out_group);
    if (!s.ok()) return s;

    // Everything in the output must have come from the input, unchanged.
    // Both groups are ordered newest first.
    size_t i = 0;
    for (const Entry& out : out_group) {
      while (i < in_group.size() && in_group[i].sequence > out.sequence) i++;
      if (i == in_group.size() || !(in_group[i] == out)) {
        return differ(user_key, "the output has an entry at sequence " +
                                       std::to_string(out.sequence) + " that the input lacks");
      }
    }

    for (SequenceNumber read_point : read_points) {
      const Entry* before = Visible(in_group, read_point);
      const Entry* after = Visible(out_group, read_point);
      const std::string at = " at read point " + std::to_string(read_point);
      if (before == nullptr) {
        if (after != nullptr) return differ(user_key, "an entry appeared" + at);
        continue;
      }
      if (after != nullptr) {
        if (!(*before == *after)) return differ(user_key, "a different entry is visible" + at);
        continue;
      }
      // The visible entry is gone.
      if (before->type == kTypeValue) {
        return differ(user_key, "a visible value was dropped" + at);
      }
      if (key_may_exist_below(user_key)) {
        return differ(user_key, "a tombstone was dropped" + at +
                                       " while a lower level may still hold the key");
      }
    }
  }
  if (!input->status().ok()) return input->status();

  if (output->Valid()) {
    return differ(ExtractUserKey(output->key()), "the output has a key the input lacks");
  }
  return output->status();
}

}  // namespace lsm
