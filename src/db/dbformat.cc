#include "db/dbformat.h"

namespace lsm {

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key);
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

bool ParseInternalKey(Slice internal_key, ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  const uint64_t trailer = DecodeFixed64(internal_key.data() + n - 8);
  const uint8_t type = trailer & 0xff;
  result->sequence = trailer >> 8;
  result->type = static_cast<ValueType>(type);
  result->user_key = internal_key.substr(0, n - 8);
  return type <= kTypeValue;
}

std::string DebugInternalKey(Slice internal_key) {
  ParsedInternalKey parsed;
  if (!ParseInternalKey(internal_key, &parsed)) {
    return "(bad internal key, " + std::to_string(internal_key.size()) + " bytes)";
  }
  std::string result = "'";
  for (const char c : parsed.user_key) {
    if (c >= ' ' && c <= '~') {
      result.push_back(c);
    } else {
      static constexpr char kHex[] = "0123456789abcdef";
      const auto byte = static_cast<uint8_t>(c);
      result += {'\\', 'x', kHex[byte >> 4], kHex[byte & 0xf]};
    }
  }
  result += "' @ " + std::to_string(parsed.sequence);
  result += parsed.type == kTypeValue ? " : value" : " : deletion";
  return result;
}

int InternalKeyComparator::Compare(Slice a, Slice b) const {
  int r = user_comparator_->Compare(ExtractUserKey(a), ExtractUserKey(b));
  if (r == 0) {
    const uint64_t atrailer = DecodeFixed64(a.data() + a.size() - 8);
    const uint64_t btrailer = DecodeFixed64(b.data() + b.size() - 8);
    if (atrailer > btrailer) {
      r = -1;
    } else if (atrailer < btrailer) {
      r = +1;
    }
  }
  return r;
}

const char* InternalKeyComparator::Name() const { return "lsm.InternalKeyComparator"; }

void InternalKeyComparator::FindShortestSeparator(std::string* start, Slice limit) const {
  // Shorten the user key portion only.
  Slice user_start = ExtractUserKey(*start);
  Slice user_limit = ExtractUserKey(limit);
  std::string tmp(user_start);
  user_comparator_->FindShortestSeparator(&tmp, user_limit);
  if (tmp.size() < user_start.size() && user_comparator_->Compare(user_start, tmp) < 0) {
    // The user key grew logically while shrinking physically. Give it the
    // earliest possible trailer so it sorts before every real entry for tmp.
    PutFixed64(&tmp, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(Compare(*start, tmp) < 0);
    assert(Compare(tmp, limit) < 0);
    start->swap(tmp);
  }
}

void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
  Slice user_key = ExtractUserKey(*key);
  std::string tmp(user_key);
  user_comparator_->FindShortSuccessor(&tmp);
  if (tmp.size() < user_key.size() && user_comparator_->Compare(user_key, tmp) < 0) {
    PutFixed64(&tmp, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(Compare(*key, tmp) < 0);
    key->swap(tmp);
  }
}

LookupKey::LookupKey(Slice user_key, SequenceNumber sequence) {
  const size_t usize = user_key.size();
  const size_t needed = usize + 13;  // 5 for the varint32, 8 for the trailer
  char* dst = needed <= sizeof(space_) ? space_ : new char[needed];
  start_ = dst;
  dst = EncodeVarint32(dst, static_cast<uint32_t>(usize + 8));
  kstart_ = dst;
  std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  EncodeFixed64(dst, PackSequenceAndType(sequence, kValueTypeForSeek));
  end_ = dst + 8;
}

LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}

}  // namespace lsm
