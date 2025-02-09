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

#include "riegeli/bytes/string_reader.h"

#include <stddef.h>

#include "absl/base/optimization.h"
#include "riegeli/base/base.h"

namespace riegeli {

bool StringReaderBase::PullSlow(size_t min_length, size_t recommended_length) {
  RIEGELI_ASSERT_GT(min_length, available())
      << "Failed precondition of Reader::PullSlow(): "
         "length too small, use Pull() instead";
  return false;
}

bool StringReaderBase::SeekSlow(Position new_pos) {
  RIEGELI_ASSERT(new_pos < start_pos() || new_pos > limit_pos_)
      << "Failed precondition of Reader::SeekSlow(): "
         "position in the buffer, use Seek() instead";
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  RIEGELI_ASSERT_EQ(start_pos(), 0u)
      << "Failed invariant of StringReader: non-zero position of buffer start";
  // Seeking forwards. Source ends.
  cursor_ = limit_;
  return false;
}

bool StringReaderBase::Size(Position* size) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  *size = limit_pos_;
  return true;
}

}  // namespace riegeli
