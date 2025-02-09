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

#ifndef RIEGELI_BYTES_ZLIB_READER_H_
#define RIEGELI_BYTES_ZLIB_READER_H_

#include <stddef.h>

#include <tuple>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/strings/string_view.h"
#include "riegeli/base/base.h"
#include "riegeli/base/dependency.h"
#include "riegeli/base/recycling_pool.h"
#include "riegeli/base/resetter.h"
#include "riegeli/base/status.h"
#include "riegeli/bytes/buffered_reader.h"
#include "riegeli/bytes/reader.h"
#include "zconf.h"
#include "zlib.h"

namespace riegeli {

// Template parameter independent part of `ZlibReader`.
class ZlibReaderBase : public BufferedReader {
 public:
  enum class Header { kZlib = 0, kGzip = 16, kZlibOrGzip = 32, kRaw = -1 };

  class Options {
   public:
    Options() noexcept {}

    // Maximum acceptable logarithm of the LZ77 sliding window size.
    //
    // `kDefaultWindowLog` means any value is acceptable, otherwise this must
    // not be lower than the corresponding setting of the compressor.
    //
    // `window_log` must be `kDefaultWindowLog` (0) or between
    // `kMinWindowLog` (9) and `kMaxWindowLog` (15).
    // Default: `kDefaultWindowLog` (0).
    static constexpr int kMinWindowLog = 9;
    static constexpr int kMaxWindowLog = MAX_WBITS;
    static constexpr int kDefaultWindowLog = 0;
    Options& set_window_log(int window_log) & {
      if (window_log != kDefaultWindowLog) {
        RIEGELI_ASSERT_GE(window_log, kMinWindowLog)
            << "Failed precondition of "
               "ZlibReaderBase::Options::set_window_log(): "
               "window log out of range";
        RIEGELI_ASSERT_LE(window_log, kMaxWindowLog)
            << "Failed precondition of "
               "ZlibReaderBase::Options::set_window_log(): "
               "window log out of range";
      }
      return *this;
    }
    Options&& set_window_log(int window_log) && {
      return std::move(set_window_log(window_log));
    }

    // What format of header to expect:
    //
    //  * `Header::kZlib`       - zlib header
    //  * `Header::kGzip`       - gzip header
    //  * `Header::kZlibOrGzip` - zlib or gzip header
    //  * `Header::kRaw`        - no header
    //                            (compressor must write no header too)
    //
    // Default: `Header::kZlibOrGzip`.
    static constexpr Header kDefaultHeader = Header::kZlibOrGzip;
    Options& set_header(Header header) & {
      header_ = header;
      return *this;
    }
    Options&& set_header(Header header) && {
      return std::move(set_header(header));
    }

    // Expected uncompressed size, or 0 if unknown. This may improve
    // performance.
    //
    // If the size hint turns out to not match reality, nothing breaks.
    Options& set_size_hint(Position size_hint) & {
      size_hint_ = size_hint;
      return *this;
    }
    Options&& set_size_hint(Position size_hint) && {
      return std::move(set_size_hint(size_hint));
    }

    // Tunes how much data is buffered after calling the decompression engine.
    //
    // Default: 64K
    Options& set_buffer_size(size_t buffer_size) & {
      RIEGELI_ASSERT_GT(buffer_size, 0u)
          << "Failed precondition of "
             "ZlibReaderBase::Options::set_buffer_size(): "
             "zero buffer size";
      buffer_size_ = buffer_size;
      return *this;
    }
    Options&& set_buffer_size(size_t buffer_size) && {
      return std::move(set_buffer_size(buffer_size));
    }

   private:
    friend class ZlibReaderBase;
    template <typename Src>
    friend class ZlibReader;

    int window_log_ = kDefaultWindowLog;
    Header header_ = kDefaultHeader;
    Position size_hint_ = 0;
    size_t buffer_size_ = kDefaultBufferSize;
  };

  // Returns the compressed `Reader`. Unchanged by `Close()`.
  virtual Reader* src_reader() = 0;
  virtual const Reader* src_reader() const = 0;

 protected:
  ZlibReaderBase() noexcept {}

  explicit ZlibReaderBase(size_t buffer_size, Position size_hint);

  ZlibReaderBase(ZlibReaderBase&& that) noexcept;
  ZlibReaderBase& operator=(ZlibReaderBase&& that) noexcept;

  void Reset();
  void Reset(size_t buffer_size, Position size_hint);
  static int GetWindowBits(const Options& options);
  void Initialize(Reader* src, int window_bits);

  void Done() override;
  bool PullSlow(size_t min_length, size_t recommended_length) override;
  bool ReadInternal(char* dest, size_t min_length, size_t max_length) override;

 private:
  struct ZStreamDeleter {
    void operator()(z_stream* ptr) const {
      const int result = inflateEnd(ptr);
      RIEGELI_ASSERT_EQ(result, Z_OK) << "inflateEnd() failed";
      delete ptr;
    }
  };

  ABSL_ATTRIBUTE_COLD bool FailOperation(StatusCode code,
                                         absl::string_view operation);

  // If `true`, the source is truncated (without a clean end of the compressed
  // stream) at the current position. If the source does not grow, `Close()`
  // will fail.
  bool truncated_ = false;
  RecyclingPool<z_stream, ZStreamDeleter>::Handle decompressor_;
};

// A `Reader` which decompresses data with Zlib after getting it from another
// `Reader`.
//
// The `Src` template parameter specifies the type of the object providing and
// possibly owning the compressed `Reader`. `Src` must support
// `Dependency<Reader*, Src>`, e.g. `Reader*` (not owned, default),
// `std::unique_ptr<Reader>` (owned), `ChainReader<>` (owned).
//
// The compressed `Reader` must not be accessed until the `ZlibReader` is closed
// or no longer used.
template <typename Src = Reader*>
class ZlibReader : public ZlibReaderBase {
 public:
  // Creates a closed `ZlibReader`.
  ZlibReader() noexcept {}

  // Will read from the compressed `Reader` provided by `src`.
  explicit ZlibReader(const Src& src, Options options = Options());
  explicit ZlibReader(Src&& src, Options options = Options());

  // Will read from the compressed `Reader` provided by a `Src` constructed from
  // elements of `src_args`. This avoids constructing a temporary `Src` and
  // moving from it.
  template <typename... SrcArgs>
  explicit ZlibReader(std::tuple<SrcArgs...> src_args,
                      Options options = Options());

  ZlibReader(ZlibReader&&) noexcept;
  ZlibReader& operator=(ZlibReader&&) noexcept;

  // Makes `*this` equivalent to a newly constructed `ZlibReader`. This avoids
  // constructing a temporary `ZlibReader` and moving from it.
  void Reset();
  void Reset(const Src& src, Options options = Options());
  void Reset(Src&& src, Options options = Options());
  template <typename... SrcArgs>
  void Reset(std::tuple<SrcArgs...> src_args, Options options = Options());

  // Returns the object providing and possibly owning the compressed `Reader`.
  // Unchanged by `Close()`.
  Src& src() { return src_.manager(); }
  const Src& src() const { return src_.manager(); }
  Reader* src_reader() override { return src_.get(); }
  const Reader* src_reader() const override { return src_.get(); }

  void VerifyEnd() override;

 protected:
  void Done() override;

 private:
  // The object providing and possibly owning the compressed `Reader`.
  Dependency<Reader*, Src> src_;
};

// Implementation details follow.

inline ZlibReaderBase::ZlibReaderBase(size_t buffer_size, Position size_hint)
    : BufferedReader(buffer_size, size_hint) {}

inline ZlibReaderBase::ZlibReaderBase(ZlibReaderBase&& that) noexcept
    : BufferedReader(std::move(that)),
      truncated_(that.truncated_),
      decompressor_(std::move(that.decompressor_)) {}

inline ZlibReaderBase& ZlibReaderBase::operator=(
    ZlibReaderBase&& that) noexcept {
  BufferedReader::operator=(std::move(that));
  truncated_ = that.truncated_;
  decompressor_ = std::move(that.decompressor_);
  return *this;
}

inline void ZlibReaderBase::Reset() {
  BufferedReader::Reset();
  truncated_ = false;
  decompressor_.reset();
}

inline void ZlibReaderBase::Reset(size_t buffer_size, Position size_hint) {
  BufferedReader::Reset(buffer_size, size_hint);
  truncated_ = false;
  decompressor_.reset();
}

inline int ZlibReaderBase::GetWindowBits(const Options& options) {
  return options.header_ == Header::kRaw
             ? -options.window_log_
             : options.window_log_ + static_cast<int>(options.header_);
}

template <typename Src>
inline ZlibReader<Src>::ZlibReader(const Src& src, Options options)
    : ZlibReaderBase(options.buffer_size_, options.size_hint_), src_(src) {
  Initialize(src_.get(), GetWindowBits(options));
}

template <typename Src>
inline ZlibReader<Src>::ZlibReader(Src&& src, Options options)
    : ZlibReaderBase(options.buffer_size_, options.size_hint_),
      src_(std::move(src)) {
  Initialize(src_.get(), GetWindowBits(options));
}

template <typename Src>
template <typename... SrcArgs>
inline ZlibReader<Src>::ZlibReader(std::tuple<SrcArgs...> src_args,
                                   Options options)
    : ZlibReaderBase(options.buffer_size_, options.size_hint_),
      src_(std::move(src_args)) {
  Initialize(src_.get(), GetWindowBits(options));
}

template <typename Src>
inline ZlibReader<Src>::ZlibReader(ZlibReader&& that) noexcept
    : ZlibReaderBase(std::move(that)), src_(std::move(that.src_)) {}

template <typename Src>
inline ZlibReader<Src>& ZlibReader<Src>::operator=(ZlibReader&& that) noexcept {
  ZlibReaderBase::operator=(std::move(that));
  src_ = std::move(that.src_);
  return *this;
}

template <typename Src>
inline void ZlibReader<Src>::Reset() {
  ZlibReaderBase::Reset();
  src_.Reset();
}

template <typename Src>
inline void ZlibReader<Src>::Reset(const Src& src, Options options) {
  ZlibReaderBase::Reset(options.buffer_size_, options.size_hint_);
  src_.Reset(src);
  Initialize(src_.get(), GetWindowBits(options));
}

template <typename Src>
inline void ZlibReader<Src>::Reset(Src&& src, Options options) {
  ZlibReaderBase::Reset(options.buffer_size_, options.size_hint_);
  src_.Reset(std::move(src));
  Initialize(src_.get(), GetWindowBits(options));
}

template <typename Src>
template <typename... SrcArgs>
inline void ZlibReader<Src>::Reset(std::tuple<SrcArgs...> src_args,
                                   Options options) {
  ZlibReaderBase::Reset(options.buffer_size_, options.size_hint_);
  src_.Reset(std::move(src_args));
  Initialize(src_.get(), GetWindowBits(options));
}

template <typename Src>
void ZlibReader<Src>::Done() {
  ZlibReaderBase::Done();
  if (src_.is_owning()) {
    if (ABSL_PREDICT_FALSE(!src_->Close())) Fail(*src_);
  }
}

template <typename Src>
void ZlibReader<Src>::VerifyEnd() {
  ZlibReaderBase::VerifyEnd();
  if (src_.is_owning() && ABSL_PREDICT_TRUE(healthy())) src_->VerifyEnd();
}

template <typename Src>
struct Resetter<ZlibReader<Src>> : ResetterByReset<ZlibReader<Src>> {};

}  // namespace riegeli

#endif  // RIEGELI_BYTES_ZLIB_READER_H_
