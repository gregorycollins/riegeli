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

#ifndef RIEGELI_BYTES_CHAIN_READER_H_
#define RIEGELI_BYTES_CHAIN_READER_H_

#include <stddef.h>

#include <tuple>
#include <utility>

#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/dependency.h"
#include "riegeli/base/object.h"
#include "riegeli/base/resetter.h"
#include "riegeli/bytes/backward_writer.h"
#include "riegeli/bytes/pullable_reader.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

// Template parameter independent part of `ChainReader`.
class ChainReaderBase : public PullableReader {
 public:
  // Returns the `Chain` being read from. Unchanged by `Close()`.
  virtual const Chain* src_chain() const = 0;

  bool SupportsRandomAccess() const override { return true; }
  bool Size(Position* size) override;

 protected:
  explicit ChainReaderBase(InitiallyClosed) noexcept
      : PullableReader(kInitiallyClosed) {}
  explicit ChainReaderBase(InitiallyOpen) noexcept
      : PullableReader(kInitiallyOpen) {}

  ChainReaderBase(ChainReaderBase&& that) noexcept;
  ChainReaderBase& operator=(ChainReaderBase&& that) noexcept;

  void Reset(InitiallyClosed);
  void Reset(InitiallyOpen);
  void Initialize(const Chain* src);

  void Done() override;
  bool PullSlow(size_t min_length, size_t recommended_length) override;
  using PullableReader::ReadSlow;
  bool ReadSlow(Chain* dest, size_t length) override;
  using PullableReader::CopyToSlow;
  bool CopyToSlow(Writer* dest, Position length) override;
  bool CopyToSlow(BackwardWriter* dest, size_t length) override;
  bool SeekSlow(Position new_pos) override;

  Chain::BlockIterator iter_;

  // Invariants if `healthy()` and scratch is not used:
  //   `iter_.chain() == src_chain()`
  //   `start_ ==
  //       (iter_ == src_chain()->blocks().cend() ? nullptr : iter_->data())`
  //   `buffer_size() ==
  //       (iter_ == src_chain()->blocks().cend() ? 0 : iter_->size())`
  //   `start_pos()` is the position of `iter_` in `*src_chain()`
};

// A `Reader` which reads from a `Chain`. It supports random access.
//
// The `Src` template parameter specifies the type of the object providing and
// possibly owning the `Chain` being read from. `Src` must support
// `Dependency<const Chain*, Src>`, e.g. `const Chain*` (not owned, default),
// `Chain` (owned).
//
// The `Chain` must not be changed until the `ChainReader` is closed or no
// longer used.
template <typename Src = const Chain*>
class ChainReader : public ChainReaderBase {
 public:
  // Creates a closed `ChainReader`.
  ChainReader() noexcept : ChainReaderBase(kInitiallyClosed) {}

  // Will read from the `Chain` provided by `src`.
  explicit ChainReader(const Src& src);
  explicit ChainReader(Src&& src);

  // Will read from the `Chain` provided by a `Src` constructed from elements of
  // `src_args`. This avoids constructing a temporary `Src` and moving from it.
  template <typename... SrcArgs>
  explicit ChainReader(std::tuple<SrcArgs...> src_args);

  ChainReader(ChainReader&& that) noexcept;
  ChainReader& operator=(ChainReader&& that) noexcept;

  // Makes `*this` equivalent to a newly constructed `ChainReader`. This avoids
  // constructing a temporary `ChainReader` and moving from it.
  void Reset();
  void Reset(const Src& src);
  void Reset(Src&& src);
  template <typename... SrcArgs>
  void Reset(std::tuple<SrcArgs...> src_args);

  // Returns the object providing and possibly owning the `Chain` being read
  // from. Unchanged by `Close()`.
  Src& src() { return src_.manager(); }
  const Src& src() const { return src_.manager(); }
  const Chain* src_chain() const override { return src_.get(); }

 private:
  void MoveSrc(ChainReader&& that);

  // The object providing and possibly owning the `Chain` being read from.
  Dependency<const Chain*, Src> src_;
};

// Implementation details follow.

inline ChainReaderBase::ChainReaderBase(ChainReaderBase&& that) noexcept
    : PullableReader(std::move(that)),
      iter_(std::exchange(that.iter_, Chain::BlockIterator())) {}

inline ChainReaderBase& ChainReaderBase::operator=(
    ChainReaderBase&& that) noexcept {
  PullableReader::operator=(std::move(that));
  iter_ = std::exchange(that.iter_, Chain::BlockIterator());
  return *this;
}

inline void ChainReaderBase::Reset(InitiallyClosed) {
  Reader::Reset(kInitiallyClosed);
  iter_ = Chain::BlockIterator();
}

inline void ChainReaderBase::Reset(InitiallyOpen) {
  Reader::Reset(kInitiallyOpen);
  // `iter_` will be set by `Initialize()`.
}

inline void ChainReaderBase::Initialize(const Chain* src) {
  RIEGELI_ASSERT(src != nullptr)
      << "Failed precondition of ChainReader: null Chain pointer";
  iter_ = src->blocks().cbegin();
  if (iter_ != src->blocks().cend()) {
    start_ = iter_->data();
    cursor_ = start_;
    limit_ = start_ + iter_->size();
    limit_pos_ += available();
  }
}

template <typename Src>
inline ChainReader<Src>::ChainReader(const Src& src)
    : ChainReaderBase(kInitiallyOpen), src_(src) {
  Initialize(src_.get());
}

template <typename Src>
inline ChainReader<Src>::ChainReader(Src&& src)
    : ChainReaderBase(kInitiallyOpen), src_(std::move(src)) {
  Initialize(src_.get());
}

template <typename Src>
template <typename... SrcArgs>
inline ChainReader<Src>::ChainReader(std::tuple<SrcArgs...> src_args)
    : ChainReaderBase(kInitiallyOpen), src_(std::move(src_args)) {
  Initialize(src_.get());
}

template <typename Src>
inline ChainReader<Src>::ChainReader(ChainReader&& that) noexcept
    : ChainReaderBase(std::move(that)) {
  MoveSrc(std::move(that));
}

template <typename Src>
inline ChainReader<Src>& ChainReader<Src>::operator=(
    ChainReader&& that) noexcept {
  ChainReaderBase::operator=(std::move(that));
  MoveSrc(std::move(that));
  return *this;
}

template <typename Src>
void ChainReader<Src>::Reset() {
  ChainReaderBase::Reset(kInitiallyClosed);
  src_.Reset();
}

template <typename Src>
void ChainReader<Src>::Reset(const Src& src) {
  ChainReaderBase::Reset(kInitiallyOpen);
  src_.Reset(src);
  Initialize(src_.get());
}

template <typename Src>
void ChainReader<Src>::Reset(Src&& src) {
  ChainReaderBase::Reset(kInitiallyOpen);
  src_.Reset(std::move(src));
  Initialize(src_.get());
}

template <typename Src>
template <typename... SrcArgs>
void ChainReader<Src>::Reset(std::tuple<SrcArgs...> src_args) {
  ChainReaderBase::Reset(kInitiallyOpen);
  src_.Reset(std::move(src_args));
  Initialize(src_.get());
}

template <typename Src>
inline void ChainReader<Src>::MoveSrc(ChainReader&& that) {
  if (src_.kIsStable()) {
    src_ = std::move(that.src_);
  } else {
    SwapScratchBegin();
    const size_t block_index = iter_.block_index();
    const size_t cursor_index = read_from_buffer();
    src_ = std::move(that.src_);
    if (iter_.chain() != nullptr) {
      iter_ = Chain::BlockIterator(src_.get(), block_index);
      if (start_ != nullptr) {
        start_ = iter_->data();
        cursor_ = start_ + cursor_index;
        limit_ = start_ + iter_->size();
      }
    }
    SwapScratchEnd();
  }
}

template <typename Src>
struct Resetter<ChainReader<Src>> : ResetterByReset<ChainReader<Src>> {};

}  // namespace riegeli

#endif  // RIEGELI_BYTES_CHAIN_READER_H_
