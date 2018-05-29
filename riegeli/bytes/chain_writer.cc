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

#include "riegeli/bytes/chain_writer.h"

#include <stddef.h>
#include <limits>
#include <string>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

void ChainWriter::Done() {
  if (ABSL_PREDICT_TRUE(healthy())) {
    RIEGELI_ASSERT_EQ(limit_pos(), dest_->size())
        << "ChainWriter destination changed unexpectedly";
    DiscardBuffer();
    start_pos_ = dest_->size();
  }
  Writer::Done();
}

bool ChainWriter::PushSlow() {
  RIEGELI_ASSERT_EQ(available(), 0u)
      << "Failed precondition of Writer::PushSlow(): "
         "space available, use Push() instead";
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  RIEGELI_ASSERT_EQ(limit_pos(), dest_->size())
      << "ChainWriter destination changed unexpectedly";
  if (ABSL_PREDICT_FALSE(dest_->size() == std::numeric_limits<size_t>::max())) {
    cursor_ = start_;
    limit_ = start_;
    return FailOverflow();
  }
  start_pos_ = dest_->size();
  const absl::Span<char> buffer = dest_->AppendBuffer(1, 0, size_hint_);
  start_ = buffer.data();
  cursor_ = start_;
  limit_ = start_ + buffer.size();
  return true;
}

bool ChainWriter::WriteSlow(absl::string_view src) {
  RIEGELI_ASSERT_GT(src.size(), available())
      << "Failed precondition of Writer::WriteSlow(string_view): "
         "length too small, use Write(string_view) instead";
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  RIEGELI_ASSERT_EQ(limit_pos(), dest_->size())
      << "ChainWriter destination changed unexpectedly";
  if (ABSL_PREDICT_FALSE(src.size() > std::numeric_limits<size_t>::max() -
                                          IntCast<size_t>(pos()))) {
    cursor_ = start_;
    limit_ = start_;
    return FailOverflow();
  }
  DiscardBuffer();
  dest_->Append(src, size_hint_);
  MakeBuffer();
  return true;
}

bool ChainWriter::WriteSlow(std::string&& src) {
  RIEGELI_ASSERT_GT(src.size(), UnsignedMin(available(), kMaxBytesToCopy()))
      << "Failed precondition of Writer::WriteSlow(string&&): "
         "length too small, use Write(string&&) instead";
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  RIEGELI_ASSERT_EQ(limit_pos(), dest_->size())
      << "ChainWriter destination changed unexpectedly";
  if (ABSL_PREDICT_FALSE(src.size() > std::numeric_limits<size_t>::max() -
                                          IntCast<size_t>(pos()))) {
    cursor_ = start_;
    limit_ = start_;
    return FailOverflow();
  }
  DiscardBuffer();
  dest_->Append(std::move(src), size_hint_);
  MakeBuffer();
  return true;
}

bool ChainWriter::WriteSlow(const Chain& src) {
  RIEGELI_ASSERT_GT(src.size(), UnsignedMin(available(), kMaxBytesToCopy()))
      << "Failed precondition of Writer::WriteSlow(Chain): "
         "length too small, use Write(Chain) instead";
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  RIEGELI_ASSERT_EQ(limit_pos(), dest_->size())
      << "ChainWriter destination changed unexpectedly";
  if (ABSL_PREDICT_FALSE(src.size() > std::numeric_limits<size_t>::max() -
                                          IntCast<size_t>(pos()))) {
    cursor_ = start_;
    limit_ = start_;
    return FailOverflow();
  }
  DiscardBuffer();
  dest_->Append(src, size_hint_);
  MakeBuffer();
  return true;
}

bool ChainWriter::WriteSlow(Chain&& src) {
  RIEGELI_ASSERT_GT(src.size(), UnsignedMin(available(), kMaxBytesToCopy()))
      << "Failed precondition of Writer::WriteSlow(Chain&&): "
         "length too small, use Write(Chain&&) instead";
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  DiscardBuffer();
  dest_->Append(std::move(src), size_hint_);
  MakeBuffer();
  return true;
}

bool ChainWriter::Flush(FlushType flush_type) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  RIEGELI_ASSERT_EQ(limit_pos(), dest_->size())
      << "ChainWriter destination changed unexpectedly";
  DiscardBuffer();
  start_pos_ = dest_->size();
  start_ = nullptr;
  cursor_ = nullptr;
  limit_ = nullptr;
  return true;
}

bool ChainWriter::Truncate(Position new_size) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  RIEGELI_ASSERT_EQ(limit_pos(), dest_->size())
      << "ChainWriter destination changed unexpectedly";
  if (new_size >= start_pos_) {
    if (ABSL_PREDICT_FALSE(new_size > pos())) return false;
    cursor_ = start_ + (new_size - start_pos_);
    return true;
  }
  dest_->RemoveSuffix(IntCast<size_t>(dest_->size() - new_size));
  MakeBuffer();
  return true;
}

inline void ChainWriter::DiscardBuffer() { dest_->RemoveSuffix(available()); }

inline void ChainWriter::MakeBuffer() {
  start_pos_ = dest_->size();
  const absl::Span<char> buffer = dest_->AppendBuffer(0, 0, size_hint_);
  start_ = buffer.data();
  cursor_ = start_;
  limit_ = start_ + buffer.size();
}

}  // namespace riegeli
