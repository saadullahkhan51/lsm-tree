#pragma once

#include <string>

#include "lsm/slice.h"

namespace lsm {

class [[nodiscard]] Status {
 public:
  Status() = default;

  static Status OK() { return Status(); }
  static Status NotFound(Slice msg, Slice msg2 = {}) { return Status(kNotFound, msg, msg2); }
  static Status Corruption(Slice msg, Slice msg2 = {}) { return Status(kCorruption, msg, msg2); }
  static Status NotSupported(Slice msg, Slice msg2 = {}) { return Status(kNotSupported, msg, msg2); }
  static Status InvalidArgument(Slice msg, Slice msg2 = {}) { return Status(kInvalidArgument, msg, msg2); }
  static Status IOError(Slice msg, Slice msg2 = {}) { return Status(kIOError, msg, msg2); }

  bool ok() const { return code_ == kOk; }
  bool IsNotFound() const { return code_ == kNotFound; }
  bool IsCorruption() const { return code_ == kCorruption; }
  bool IsNotSupported() const { return code_ == kNotSupported; }
  bool IsInvalidArgument() const { return code_ == kInvalidArgument; }
  bool IsIOError() const { return code_ == kIOError; }

  std::string ToString() const {
    static constexpr const char* kNames[] = {"OK",
                                             "NotFound: ",
                                             "Corruption: ",
                                             "Not implemented: ",
                                             "Invalid argument: ",
                                             "IO error: "};
    return kNames[code_] + msg_;
  }

 private:
  enum Code { kOk, kNotFound, kCorruption, kNotSupported, kInvalidArgument, kIOError };

  Status(Code code, Slice msg, Slice msg2) : code_(code), msg_(msg) {
    if (!msg2.empty()) {
      msg_.append(": ").append(msg2);
    }
  }

  Code code_ = kOk;
  std::string msg_;
};

}  // namespace lsm
