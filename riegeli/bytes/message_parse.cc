// Copyright 2017 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "riegeli/bytes/message_parse.h"

#include <stddef.h>
#include <stdint.h>

#include <limits>

#include "absl/base/optimization.h"
#include "absl/strings/str_cat.h"
#include "absl/types/optional.h"
#include "google/protobuf/io/zero_copy_stream.h"
#include "google/protobuf/message_lite.h"
#include "riegeli/base/base.h"
#include "riegeli/base/canonical_errors.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/status.h"
#include "riegeli/bytes/chain_reader.h"
#include "riegeli/bytes/reader.h"

namespace riegeli {

namespace {

// Adapts a `Reader` to a `google::protobuf::io::ZeroCopyInputStream`.
class ReaderInputStream : public google::protobuf::io::ZeroCopyInputStream {
 public:
  explicit ReaderInputStream(Reader* src)
      : src_(RIEGELI_ASSERT_NOTNULL(src)), initial_pos_(src_->pos()) {}

  bool Next(const void** data, int* size) override;
  void BackUp(int length) override;
  bool Skip(int length) override;
  int64_t ByteCount() const override;

 private:
  Position relative_pos() const;

  Reader* src_;
  // Invariants:
  //   `src_->pos() >= initial_pos_`
  //   `src_->pos() - initial_pos_ <= std::numeric_limits<int64_t>::max()`
  Position initial_pos_;
};

inline Position ReaderInputStream::relative_pos() const {
  RIEGELI_ASSERT_GE(src_->pos(), initial_pos_)
      << "Failed invariant of ReaderInputStream: "
         "current position smaller than initial position";
  const Position pos = src_->pos() - initial_pos_;
  RIEGELI_ASSERT_LE(pos, Position{std::numeric_limits<int64_t>::max()})
      << "Failed invariant of ReaderInputStream: "
         "relative position overflow";
  return pos;
}

bool ReaderInputStream::Next(const void** data, int* size) {
  const Position pos = relative_pos();
  if (ABSL_PREDICT_FALSE(pos ==
                         Position{std::numeric_limits<int64_t>::max()})) {
    return false;
  }
  if (ABSL_PREDICT_FALSE(!src_->Pull())) return false;
  *data = src_->cursor();
  *size = IntCast<int>(
      UnsignedMin(src_->available(), size_t{std::numeric_limits<int>::max()},
                  Position{std::numeric_limits<int64_t>::max()} - pos));
  src_->set_cursor(src_->cursor() + *size);
  return true;
}

void ReaderInputStream::BackUp(int length) {
  RIEGELI_ASSERT_GE(length, 0)
      << "Failed precondition of ZeroCopyInputStream::BackUp(): "
         "negative length";
  RIEGELI_ASSERT_LE(IntCast<size_t>(length), src_->read_from_buffer())
      << "Failed precondition of ZeroCopyInputStream::BackUp(): "
         "length larger than the amount of buffered data";
  src_->set_cursor(src_->cursor() - length);
}

bool ReaderInputStream::Skip(int length) {
  RIEGELI_ASSERT_GE(length, 0)
      << "Failed precondition of ZeroCopyInputStream::Skip(): negative length";
  const Position max_length =
      Position{std::numeric_limits<int64_t>::max()} - relative_pos();
  if (ABSL_PREDICT_FALSE(IntCast<size_t>(length) > max_length)) {
    src_->Skip(max_length);
    return false;
  }
  return src_->Skip(IntCast<size_t>(length));
}

int64_t ReaderInputStream::ByteCount() const {
  return IntCast<int64_t>(relative_pos());
}

}  // namespace

namespace internal {

Status ParseFromReaderImpl(google::protobuf::MessageLite* dest, Reader* src) {
  {
    const Status status = ParsePartialFromReaderImpl(dest, src);
    if (ABSL_PREDICT_FALSE(!status.ok())) {
      return status;
    }
  }
  if (ABSL_PREDICT_FALSE(!dest->IsInitialized())) {
    return DataLossError(
        absl::StrCat("Failed to parse message of type ", dest->GetTypeName(),
                     " because it is missing required fields: ",
                     dest->InitializationErrorString()));
  }
  return OkStatus();
}

Status ParsePartialFromReaderImpl(google::protobuf::MessageLite* dest,
                                  Reader* src) {
  src->Pull();
  if (src->available() <= kMaxBytesToCopy && src->SupportsRandomAccess()) {
    Position size;
    if (ABSL_PREDICT_FALSE(!src->Size(&size))) return src->status();
    if (src->pos() + src->available() == size &&
        ABSL_PREDICT_TRUE(src->available() <=
                          size_t{std::numeric_limits<int>::max()})) {
      // The data are flat. `ParsePartialFromArray()` is faster than
      // `ParsePartialFromZeroCopyStream()`.
      bool ok = dest->ParsePartialFromArray(src->cursor(),
                                            IntCast<int>(src->available()));
      src->set_cursor(src->cursor() + src->available());
      if (ABSL_PREDICT_FALSE(!ok)) {
        return DataLossError(absl::StrCat("Failed to parse message of type ",
                                          dest->GetTypeName()));
      }
      return OkStatus();
    }
  }
  return ParsePartialFromReaderUsingInputStream(dest, src);
}

Status ParseFromReaderUsingInputStream(google::protobuf::MessageLite* dest,
                                       Reader* src) {
  {
    const Status status = ParsePartialFromReaderUsingInputStream(dest, src);
    if (ABSL_PREDICT_FALSE(!status.ok())) {
      return status;
    }
  }
  if (ABSL_PREDICT_FALSE(!dest->IsInitialized())) {
    return DataLossError(
        absl::StrCat("Failed to parse message of type ", dest->GetTypeName(),
                     " because it is missing required fields: ",
                     dest->InitializationErrorString()));
  }
  return OkStatus();
}

Status ParsePartialFromReaderUsingInputStream(
    google::protobuf::MessageLite* dest, Reader* src) {
  ReaderInputStream input_stream(src);
  if (ABSL_PREDICT_FALSE(
          !dest->ParsePartialFromZeroCopyStream(&input_stream))) {
    if (ABSL_PREDICT_FALSE(!src->healthy())) return src->status();
    return DataLossError(
        absl::StrCat("Failed to parse message of type ", dest->GetTypeName()));
  }
  return OkStatus();
}

}  // namespace internal

Status ParseFromChain(google::protobuf::MessageLite* dest, const Chain& src) {
  {
    const Status status = ParsePartialFromChain(dest, src);
    if (ABSL_PREDICT_FALSE(!status.ok())) {
      return status;
    }
  }
  if (ABSL_PREDICT_FALSE(!dest->IsInitialized())) {
    return DataLossError(
        absl::StrCat("Failed to parse message of type ", dest->GetTypeName(),
                     " because it is missing required fields: ",
                     dest->InitializationErrorString()));
  }
  return OkStatus();
}

Status ParsePartialFromChain(google::protobuf::MessageLite* dest,
                             const Chain& src) {
  if (src.size() <= kMaxBytesToCopy) {
    if (absl::optional<absl::string_view> flat = src.TryFlat()) {
      // The data are flat. `ParsePartialFromArray()` is faster than
      // `ParsePartialFromZeroCopyStream()`.
      if (ABSL_PREDICT_FALSE(!dest->ParsePartialFromArray(
              flat->data(), IntCast<int>(flat->size())))) {
        return DataLossError(absl::StrCat("Failed to parse message of type ",
                                          dest->GetTypeName()));
      }
      return OkStatus();
    }
  }
  ChainReader<> reader(&src);
  return internal::ParsePartialFromReaderImpl(dest, &reader);
  // Do not bother closing the `ChainReader<>`, it can never fail.
}

}  // namespace riegeli
