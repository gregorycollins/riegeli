// Copyright 2019 Google LLC
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

#include "riegeli/bytes/framed_snappy_writer.h"

#include <stddef.h>
#include <stdint.h>

#include <cstring>
#include <limits>

#include "absl/base/optimization.h"
#include "absl/strings/string_view.h"
#include "crc32c/crc32c.h"
#include "riegeli/base/base.h"
#include "riegeli/base/buffer.h"
#include "riegeli/base/endian.h"
#include "riegeli/bytes/pushable_writer.h"
#include "riegeli/bytes/writer.h"
#include "snappy.h"

namespace riegeli {

namespace {

// https://github.com/google/snappy/blob/e9e11b84e629c3e06fbaa4f0a86de02ceb9d6992/framing_format.txt#L39
inline uint32_t MaskChecksum(uint32_t x) {
  return ((x >> 15) | (x << 17)) + 0xa282ead8;
}

}  // namespace

void FramedSnappyWriterBase::Initialize(Writer* dest) {
  RIEGELI_ASSERT(dest != nullptr)
      << "Failed precondition of FramedSnappyWriter: null Writer pointer";
  if (dest->pos() == 0) {
    // Stream identifier.
    if (ABSL_PREDICT_FALSE(!dest->Write(absl::string_view("\xff\x06\x00\x00"
                                                          "sNaPpY",
                                                          10)))) {
      Fail(*dest);
    }
  } else {
    if (ABSL_PREDICT_FALSE(!dest->healthy())) Fail(*dest);
  }
}

void FramedSnappyWriterBase::Done() {
  if (ABSL_PREDICT_TRUE(healthy())) {
    if (ABSL_PREDICT_TRUE(SyncScratch())) {
      Writer* const dest = dest_writer();
      PushInternal(dest);
    }
  }
  PushableWriter::Done();
}

bool FramedSnappyWriterBase::PushSlow(size_t min_length,
                                      size_t recommended_length) {
  RIEGELI_ASSERT_GT(min_length, available())
      << "Failed precondition of Writer::PushSlow(): "
         "length too small, use Push() instead";
  if (ABSL_PREDICT_FALSE(!PushUsingScratch(min_length))) {
    return available() >= min_length;
  }
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  Writer* const dest = dest_writer();
  if (ABSL_PREDICT_FALSE(!PushInternal(dest))) return false;
  if (ABSL_PREDICT_FALSE(start_pos_ == std::numeric_limits<Position>::max())) {
    return FailOverflow();
  }
  size_t length = snappy::kBlockSize;
  if (start_pos_ < size_hint_) {
    length = UnsignedMin(length, size_hint_ - start_pos_);
  }
  uncompressed_.Resize(length);
  start_ = uncompressed_.GetData();
  cursor_ = start_;
  limit_ = start_ + UnsignedMin(length, std::numeric_limits<Position>::max() -
                                            start_pos_);
  return true;
}

inline bool FramedSnappyWriterBase::PushInternal(Writer* dest) {
  const size_t uncompressed_length = written_to_buffer();
  if (uncompressed_length == 0) return true;
  cursor_ = start_;
  const char* const uncompressed_data = cursor_;
  struct {
    uint32_t chunk_header, checksum;
  } header;
  if (ABSL_PREDICT_FALSE(!dest->Push(
          sizeof(header) + snappy::MaxCompressedLength(uncompressed_length)))) {
    return Fail(*dest);
  }
  char* const compressed_chunk = dest->cursor();
  size_t compressed_length;
  snappy::RawCompress(uncompressed_data, uncompressed_length,
                      compressed_chunk + sizeof(header), &compressed_length);
  if (compressed_length < uncompressed_length) {
    header.chunk_header = WriteLittleEndian32(
        0x00 /* Compressed data */ +
        ((sizeof(header.checksum) + compressed_length) << 8));
  } else {
    std::memcpy(compressed_chunk + sizeof(header), uncompressed_data,
                uncompressed_length);
    compressed_length = uncompressed_length;
    header.chunk_header = WriteLittleEndian32(
        0x01 /* Uncompressed data */ +
        ((sizeof(header.checksum) + compressed_length) << 8));
  }
  header.checksum = WriteLittleEndian32(
      MaskChecksum(crc32c::Crc32c(uncompressed_data, uncompressed_length)));
  std::memcpy(compressed_chunk, &header, sizeof(header));
  dest->set_cursor(compressed_chunk + sizeof(header) + compressed_length);
  start_pos_ += uncompressed_length;
  return true;
}

bool FramedSnappyWriterBase::Flush(FlushType flush_type) {
  if (ABSL_PREDICT_FALSE(!SyncScratch())) return false;
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  Writer* const dest = dest_writer();
  if (ABSL_PREDICT_FALSE(!PushInternal(dest))) return false;
  if (ABSL_PREDICT_FALSE(!dest->Flush(flush_type))) return Fail(*dest);
  return true;
}

}  // namespace riegeli
