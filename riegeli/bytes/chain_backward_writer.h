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

#ifndef RIEGELI_BYTES_CHAIN_BACKWARD_WRITER_H_
#define RIEGELI_BYTES_CHAIN_BACKWARD_WRITER_H_

#include <stddef.h>

#include <limits>
#include <tuple>
#include <utility>

#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/dependency.h"
#include "riegeli/base/object.h"
#include "riegeli/base/resetter.h"
#include "riegeli/bytes/backward_writer.h"

namespace riegeli {

// Template parameter independent part of `ChainBackwardWriter`.
class ChainBackwardWriterBase : public BackwardWriter {
 public:
  class Options {
   public:
    Options() noexcept {}

    // Expected final size, or 0 if unknown. This may improve performance and
    // memory usage.
    //
    // If the size hint turns out to not match reality, nothing breaks.
    Options& set_size_hint(Position size_hint) & {
      size_hint_ = size_hint;
      return *this;
    }
    Options&& set_size_hint(Position size_hint) && {
      return std::move(set_size_hint(size_hint));
    }

   private:
    template <typename Dest>
    friend class ChainBackwardWriter;

    Position size_hint_ = 0;
  };

  // Returns the `Chain` being written to.
  virtual Chain* dest_chain() = 0;
  virtual const Chain* dest_chain() const = 0;

  bool Flush(FlushType flush_type) override;
  bool SupportsTruncate() const override { return true; }
  bool Truncate(Position new_size) override;

 protected:
  ChainBackwardWriterBase() noexcept : BackwardWriter(kInitiallyClosed) {}

  explicit ChainBackwardWriterBase(Position size_hint);

  ChainBackwardWriterBase(ChainBackwardWriterBase&& that) noexcept;
  ChainBackwardWriterBase& operator=(ChainBackwardWriterBase&& that) noexcept;

  void Reset();
  void Reset(Position size_hint);
  void Initialize(Chain* dest);

  void Done() override;
  bool PushSlow(size_t min_length, size_t recommended_length) override;
  using BackwardWriter::WriteSlow;
  bool WriteSlow(const Chain& src) override;
  bool WriteSlow(Chain&& src) override;

 private:
  // Discards uninitialized space from the beginning of `*dest`, so that it
  // contains only actual data written.
  void SyncBuffer(Chain* dest);

  // Prepends uninitialized space to `*dest`.
  void MakeBuffer(Chain* dest, size_t min_length = 0,
                  size_t recommended_length = 0);

  size_t size_hint_ = 0;

  // Invariants if `healthy()`:
  //   `limit_ == nullptr || limit_ == dest_chain()->blocks().front().data()`
  //   `limit_pos() == dest_chain()->size()`
};

// A `BackwardWriter` which prepends to a `Chain`.
//
// The `Dest` template parameter specifies the type of the object providing and
// possibly owning the `Chain` being written to. `Dest` must support
// `Dependency<Chain*, Dest>`, e.g. `Chain*` (not owned, default),
// `Chain` (owned).
//
// The `Chain` must not be accessed until the `ChainBackwardWriter` is closed or
// no longer used.
template <typename Dest = Chain*>
class ChainBackwardWriter : public ChainBackwardWriterBase {
 public:
  // Creates a closed `ChainBackwardWriter`.
  ChainBackwardWriter() noexcept {}

  // Will prepend to the `Chain` provided by `dest`.
  explicit ChainBackwardWriter(const Dest& dest, Options options = Options());
  explicit ChainBackwardWriter(Dest&& dest, Options options = Options());

  // Will prepend to the `Chain` provided by a `Dest` constructed from elements
  // of `dest_args`. This avoids constructing a temporary `Dest` and moving from
  // it.
  template <typename... DestArgs>
  explicit ChainBackwardWriter(std::tuple<DestArgs...> dest_args,
                               Options options = Options());

  ChainBackwardWriter(ChainBackwardWriter&& that) noexcept;
  ChainBackwardWriter& operator=(ChainBackwardWriter&& that) noexcept;

  // Makes `*this` equivalent to a newly constructed `ChainBackwardWriter`. This
  // avoids constructing a temporary `ChainBackwardWriter` and moving from it.
  void Reset();
  void Reset(const Dest& dest, Options options = Options());
  void Reset(Dest&& dest, Options options = Options());
  template <typename... DestArgs>
  void Reset(std::tuple<DestArgs...> dest_args, Options options = Options());

  // Returns the object providing and possibly owning the `Chain` being written
  // to.
  Dest& dest() { return dest_.manager(); }
  const Dest& dest() const { return dest_.manager(); }
  Chain* dest_chain() override { return dest_.get(); }
  const Chain* dest_chain() const override { return dest_.get(); }

 private:
  void MoveDest(ChainBackwardWriter&& that);

  // The object providing and possibly owning the `Chain` being written to, with
  // uninitialized space prepended (possibly empty); `cursor_` points to the end
  // of the uninitialized space, except that it can be `nullptr` if the
  // uninitialized space is empty.
  Dependency<Chain*, Dest> dest_;
};

// Implementation details follow.

inline ChainBackwardWriterBase::ChainBackwardWriterBase(Position size_hint)
    : BackwardWriter(kInitiallyOpen),
      size_hint_(UnsignedMin(size_hint, std::numeric_limits<size_t>::max())) {}

inline ChainBackwardWriterBase::ChainBackwardWriterBase(
    ChainBackwardWriterBase&& that) noexcept
    : BackwardWriter(std::move(that)), size_hint_(that.size_hint_) {}

inline ChainBackwardWriterBase& ChainBackwardWriterBase::operator=(
    ChainBackwardWriterBase&& that) noexcept {
  BackwardWriter::operator=(std::move(that));
  size_hint_ = that.size_hint_;
  return *this;
}

inline void ChainBackwardWriterBase::Reset() {
  BackwardWriter::Reset(kInitiallyClosed);
  size_hint_ = 0;
}

inline void ChainBackwardWriterBase::Reset(Position size_hint) {
  BackwardWriter::Reset(kInitiallyOpen);
  size_hint_ = UnsignedMin(size_hint, std::numeric_limits<size_t>::max());
}

inline void ChainBackwardWriterBase::Initialize(Chain* dest) {
  RIEGELI_ASSERT(dest != nullptr)
      << "Failed precondition of ChainBackwardWriter: null Chain pointer";
  start_pos_ = dest->size();
}

template <typename Dest>
inline ChainBackwardWriter<Dest>::ChainBackwardWriter(const Dest& dest,
                                                      Options options)
    : ChainBackwardWriterBase(options.size_hint_), dest_(dest) {
  Initialize(dest_.get());
}

template <typename Dest>
inline ChainBackwardWriter<Dest>::ChainBackwardWriter(Dest&& dest,
                                                      Options options)
    : ChainBackwardWriterBase(options.size_hint_), dest_(std::move(dest)) {
  Initialize(dest_.get());
}

template <typename Dest>
template <typename... DestArgs>
inline ChainBackwardWriter<Dest>::ChainBackwardWriter(
    std::tuple<DestArgs...> dest_args, Options options)
    : ChainBackwardWriterBase(options.size_hint_), dest_(std::move(dest_args)) {
  Initialize(dest_.get());
}

template <typename Dest>
inline ChainBackwardWriter<Dest>::ChainBackwardWriter(
    ChainBackwardWriter&& that) noexcept
    : ChainBackwardWriterBase(std::move(that)) {
  MoveDest(std::move(that));
}

template <typename Dest>
inline ChainBackwardWriter<Dest>& ChainBackwardWriter<Dest>::operator=(
    ChainBackwardWriter&& that) noexcept {
  ChainBackwardWriterBase::operator=(std::move(that));
  MoveDest(std::move(that));
  return *this;
}

template <typename Dest>
inline void ChainBackwardWriter<Dest>::Reset() {
  ChainBackwardWriterBase::Reset();
  dest_.Reset();
}

template <typename Dest>
inline void ChainBackwardWriter<Dest>::Reset(const Dest& dest,
                                             Options options) {
  ChainBackwardWriterBase::Reset(options.size_hint_);
  dest_.Reset(dest);
  Initialize(dest_.get());
}

template <typename Dest>
inline void ChainBackwardWriter<Dest>::Reset(Dest&& dest, Options options) {
  ChainBackwardWriterBase::Reset(options.size_hint_);
  dest_.Reset(std::move(dest));
  Initialize(dest_.get());
}

template <typename Dest>
template <typename... DestArgs>
inline void ChainBackwardWriter<Dest>::Reset(std::tuple<DestArgs...> dest_args,
                                             Options options) {
  ChainBackwardWriterBase::Reset(options.size_hint_);
  dest_.Reset(std::move(dest_args));
  Initialize(dest_.get());
}

template <typename Dest>
inline void ChainBackwardWriter<Dest>::MoveDest(ChainBackwardWriter&& that) {
  if (dest_.kIsStable()) {
    dest_ = std::move(that.dest_);
  } else {
    const size_t cursor_index = written_to_buffer();
    dest_ = std::move(that.dest_);
    if (start_ != nullptr) {
      limit_ = const_cast<char*>(dest_->blocks().front().data());
      start_ = limit_ + (dest_->size() - IntCast<size_t>(start_pos_));
      cursor_ = start_ - cursor_index;
    }
  }
}

template <typename Dest>
struct Resetter<ChainBackwardWriter<Dest>>
    : ResetterByReset<ChainBackwardWriter<Dest>> {};

}  // namespace riegeli

#endif  // RIEGELI_BYTES_CHAIN_BACKWARD_WRITER_H_
