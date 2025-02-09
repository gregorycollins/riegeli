// Copyright 2018 Google LLC
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

#ifndef RIEGELI_BYTES_LIMITING_WRITER_H_
#define RIEGELI_BYTES_LIMITING_WRITER_H_

#include <stddef.h>

#include <limits>
#include <tuple>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/strings/string_view.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/dependency.h"
#include "riegeli/base/object.h"
#include "riegeli/base/resetter.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

// Template parameter independent part of `LimitingWriter`.
class LimitingWriterBase : public Writer {
 public:
  // An infinite size limit.
  static constexpr Position kNoSizeLimit = std::numeric_limits<Position>::max();

  // Changes the size limit.
  //
  // Precondition: `size_limit >= pos()`
  void set_size_limit(Position size_limit);

  // Returns the current size limit.
  Position size_limit() const { return size_limit_; }

  // Returns the original `Writer`. Unchanged by `Close()`.
  virtual Writer* dest_writer() = 0;
  virtual const Writer* dest_writer() const = 0;

  bool Flush(FlushType flush_type) override;
  bool SupportsRandomAccess() const override;
  bool Size(Position* size) override;
  bool SupportsTruncate() const override;
  bool Truncate(Position new_size) override;

 protected:
  LimitingWriterBase() noexcept : Writer(kInitiallyClosed) {}

  explicit LimitingWriterBase(Position size_limit);

  LimitingWriterBase(LimitingWriterBase&& that) noexcept;
  LimitingWriterBase& operator=(LimitingWriterBase&& that) noexcept;

  void Reset();
  void Reset(Position size_limit);
  void Initialize(Writer* dest);

  void Done() override;
  bool PushSlow(size_t min_length, size_t recommended_length) override;
  using Writer::WriteSlow;
  bool WriteSlow(absl::string_view src) override;
  bool WriteSlow(const Chain& src) override;
  bool WriteSlow(Chain&& src) override;
  bool SeekSlow(Position new_pos) override;

  // Sets cursor of `*dest` to cursor of `*this`.
  void SyncBuffer(Writer* dest);

  // Sets buffer pointers of `*this` to buffer pointers of `*dest`, adjusting
  // them for the size limit. Fails `*this` if `*dest` failed.
  void MakeBuffer(Writer* dest);

  Position size_limit_ = kNoSizeLimit;

 private:
  template <typename Src>
  bool WriteInternal(Src&& src);

  // Invariants if `healthy()`:
  //   `start_ == dest_writer()->start_`
  //   `limit_ <= dest_writer()->limit_`
  //   `start_pos_ == dest_writer()->start_pos_`
  //   `limit_pos() <= UnsignedMin(size_limit_, dest_writer()->limit_pos())`
};

// A `Writer` which writes to another `Writer` up to the specified size limit.
// An attempt to write more fails, leaving destination contents unspecified.
//
// The `Dest` template parameter specifies the type of the object providing and
// possibly owning the original `Writer`. `Dest` must support
// `Dependency<Writer*, Dest>`, e.g. `Writer*` (not owned, default),
// `std::unique_ptr<Writer>` (owned), `ChainWriter<>` (owned).
//
// The original `Writer` must not be accessed until the `LimitingWriter` is
// closed or no longer used, except that it is allowed to read the destination
// of the original `Writer` immediately after `Flush()`.
template <typename Dest = Writer*>
class LimitingWriter : public LimitingWriterBase {
 public:
  // Creates a closed `LimitingWriter`.
  LimitingWriter() noexcept {}

  // Will write to the original `Writer` provided by `dest`.
  //
  // Precondition: `size_limit >= dest->pos()`
  explicit LimitingWriter(const Dest& dest, Position size_limit = kNoSizeLimit);
  explicit LimitingWriter(Dest&& dest, Position size_limit = kNoSizeLimit);

  // Will write to the original `Writer` provided by a `Dest` constructed from
  // elements of `dest_args`. This avoids constructing a temporary `Dest` and
  // moving from it.
  //
  // Precondition: `size_limit >= dest->pos()`
  template <typename... DestArgs>
  explicit LimitingWriter(std::tuple<DestArgs...> dest_args,
                          Position size_limit = kNoSizeLimit);

  LimitingWriter(LimitingWriter&& that) noexcept;
  LimitingWriter& operator=(LimitingWriter&& that) noexcept;

  // Makes `*this` equivalent to a newly constructed `LimitingWriter`. This
  // avoids constructing a temporary `LimitingWriter` and moving from it.
  void Reset();
  void Reset(const Dest& dest, Position size_limit = kNoSizeLimit);
  void Reset(Dest&& dest, Position size_limit = kNoSizeLimit);
  template <typename... DestArgs>
  void Reset(std::tuple<DestArgs...> dest_args,
             Position size_limit = kNoSizeLimit);

  // Returns the object providing and possibly owning the original `Writer`.
  // Unchanged by `Close()`.
  Dest& dest() { return dest_.manager(); }
  const Dest& dest() const { return dest_.manager(); }
  Writer* dest_writer() override { return dest_.get(); }
  const Writer* dest_writer() const override { return dest_.get(); }

 protected:
  void Done() override;

 private:
  void MoveDest(LimitingWriter&& that);

  // The object providing and possibly owning the original `Writer`.
  Dependency<Writer*, Dest> dest_;
};

// Implementation details follow.

inline LimitingWriterBase::LimitingWriterBase(Position size_limit)
    : Writer(kInitiallyOpen), size_limit_(size_limit) {}

inline LimitingWriterBase::LimitingWriterBase(
    LimitingWriterBase&& that) noexcept
    : Writer(std::move(that)), size_limit_(that.size_limit_) {}

inline LimitingWriterBase& LimitingWriterBase::operator=(
    LimitingWriterBase&& that) noexcept {
  Writer::operator=(std::move(that));
  size_limit_ = that.size_limit_;
  return *this;
}

inline void LimitingWriterBase::Reset() {
  Writer::Reset(kInitiallyClosed);
  size_limit_ = kNoSizeLimit;
}

inline void LimitingWriterBase::Reset(Position size_limit) {
  Writer::Reset(kInitiallyOpen);
  size_limit_ = size_limit;
}

inline void LimitingWriterBase::Initialize(Writer* dest) {
  RIEGELI_ASSERT(dest != nullptr)
      << "Failed precondition of LimitingWriter: null Writer pointer";
  RIEGELI_ASSERT_GE(size_limit_, dest->pos())
      << "Failed precondition of LimitingWriter: "
         "size limit smaller than current position";
  MakeBuffer(dest);
}

inline void LimitingWriterBase::set_size_limit(Position size_limit) {
  RIEGELI_ASSERT_GE(size_limit, pos())
      << "Failed precondition of LimitingWriterBase::set_size_limit(): "
         "size limit smaller than current position";
  size_limit_ = size_limit;
  if (limit_pos() > size_limit_) {
    limit_ -= IntCast<size_t>(limit_pos() - size_limit_);
  }
}

inline void LimitingWriterBase::SyncBuffer(Writer* dest) {
  dest->set_cursor(cursor_);
}

inline void LimitingWriterBase::MakeBuffer(Writer* dest) {
  start_ = dest->start();
  cursor_ = dest->cursor();
  limit_ = dest->limit();
  start_pos_ = dest->pos() - dest->written_to_buffer();  // `dest->start_pos_`
  if (limit_pos() > size_limit_) {
    limit_ -= IntCast<size_t>(limit_pos() - size_limit_);
  }
  if (ABSL_PREDICT_FALSE(!dest->healthy())) Fail(*dest);
}

template <typename Dest>
inline LimitingWriter<Dest>::LimitingWriter(const Dest& dest,
                                            Position size_limit)
    : LimitingWriterBase(size_limit), dest_(dest) {
  Initialize(dest_.get());
}

template <typename Dest>
inline LimitingWriter<Dest>::LimitingWriter(Dest&& dest, Position size_limit)
    : LimitingWriterBase(size_limit), dest_(std::move(dest)) {
  Initialize(dest_.get());
}

template <typename Dest>
template <typename... DestArgs>
inline LimitingWriter<Dest>::LimitingWriter(std::tuple<DestArgs...> dest_args,
                                            Position size_limit)
    : LimitingWriterBase(size_limit), dest_(std::move(dest_args)) {
  Initialize(dest_.get());
}

template <typename Dest>
inline LimitingWriter<Dest>::LimitingWriter(LimitingWriter&& that) noexcept
    : LimitingWriterBase(std::move(that)) {
  MoveDest(std::move(that));
}

template <typename Dest>
inline LimitingWriter<Dest>& LimitingWriter<Dest>::operator=(
    LimitingWriter&& that) noexcept {
  LimitingWriterBase::operator=(std::move(that));
  MoveDest(std::move(that));
  return *this;
}

template <typename Dest>
inline void LimitingWriter<Dest>::Reset() {
  LimitingWriterBase::Reset();
  dest_.Reset();
}

template <typename Dest>
inline void LimitingWriter<Dest>::Reset(const Dest& dest, Position size_limit) {
  LimitingWriterBase::Reset(size_limit);
  dest_.Reset(dest);
  Initialize(dest_.get());
}

template <typename Dest>
inline void LimitingWriter<Dest>::Reset(Dest&& dest, Position size_limit) {
  LimitingWriterBase::Reset(size_limit);
  dest_.Reset(std::move(dest));
  Initialize(dest_.get());
}

template <typename Dest>
template <typename... DestArgs>
inline void LimitingWriter<Dest>::Reset(std::tuple<DestArgs...> dest_args,
                                        Position size_limit) {
  LimitingWriterBase::Reset(size_limit);
  dest_.Reset(std::move(dest_args));
  Initialize(dest_.get());
}

template <typename Dest>
inline void LimitingWriter<Dest>::MoveDest(LimitingWriter&& that) {
  if (dest_.kIsStable()) {
    dest_ = std::move(that.dest_);
  } else {
    // Buffer pointers are already moved so `SyncBuffer()` is called on `*this`,
    // `dest_` is not moved yet so `dest_` is taken from `that`.
    SyncBuffer(that.dest_.get());
    dest_ = std::move(that.dest_);
    MakeBuffer(dest_.get());
  }
}

template <typename Dest>
void LimitingWriter<Dest>::Done() {
  LimitingWriterBase::Done();
  if (dest_.is_owning()) {
    if (ABSL_PREDICT_FALSE(!dest_->Close())) Fail(*dest_);
  }
}

template <typename Dest>
struct Resetter<LimitingWriter<Dest>> : ResetterByReset<LimitingWriter<Dest>> {
};

}  // namespace riegeli

#endif  // RIEGELI_BYTES_LIMITING_WRITER_H_
