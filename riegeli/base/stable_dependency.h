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

#ifndef RIEGELI_BASE_STABLE_DEPENDENCY_H_
#define RIEGELI_BASE_STABLE_DEPENDENCY_H_

#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include "absl/base/optimization.h"
#include "riegeli/base/dependency.h"

namespace riegeli {

// Similar to `Dependency<Ptr, Manager>`, but ensures that `Ptr` stays unchanged
// when `StableDependency<Ptr, Manager>` is moved. `StableDependency` can be
// used instead of `Dependency` if `Ptr` stability is required, e.g. if
// background threads access the `Ptr`.
//
// Exception: a dummy `Manager` created by a default constructed
// `StableDependency` may change its address when the `StableDependency` is
// moved. The dummy `Manager` should not be used by the host object, so making
// its address change is not a problem. Since the `Manager` is exposed, making
// it unconditionally available avoids a special case in the public interface
// where accessing the dependency would be invalid. This exception avoids
// dynamic allocation in the default constructor.
template <typename Ptr, typename Manager, typename Enable = void>
class StableDependency;

// Specialization when `Dependency<P*, M>` is already stable.
template <typename P, typename M>
class StableDependency<P*, M, std::enable_if_t<Dependency<P*, M>::kIsStable()>>
    : public Dependency<P*, M> {
  using Dependency<P*, M>::Dependency;
};

// Specialization when `Dependency<P*, M>` is not stable: allocates the
// dependency dynamically.
template <typename P, typename M>
class StableDependency<P*, M,
                       std::enable_if_t<!Dependency<P*, M>::kIsStable()>> {
 private:
  using DerivedP =
      std::remove_pointer_t<decltype(std::declval<Dependency<P*, M>>().get())>;

 public:
  StableDependency() noexcept : dummy_() {}

  explicit StableDependency(const M& manager)
      : dep_(std::make_unique<Dependency<P*, M>>(manager)) {}
  explicit StableDependency(M&& manager) noexcept
      : dep_(std::make_unique<Dependency<P*, M>>(std::move(manager))) {}

  template <typename... MArgs>
  explicit StableDependency(std::tuple<MArgs...> manager_args)
      : dep_(std::make_unique<Dependency<P*, M>>(std::move(manager_args))) {}

  ~StableDependency() {
    if (dep_ == nullptr) dummy_.~Dependency<P*, M>();
  }

  StableDependency(StableDependency&& that) noexcept {
    if (that.dep_ == nullptr) {
      new (&dummy_) Dependency<P*, M>();
      return;
    }
    dep_ = std::move(that.dep_);
    new (&that.dummy_) Dependency<P*, M>();
  }
  StableDependency& operator=(StableDependency&& that) noexcept {
    if (that.dep_ == nullptr) {
      if (dep_ != nullptr) {
        dep_.reset();
        new (&dummy_) Dependency<P*, M>();
      }
      return *this;
    }
    if (dep_ == nullptr) dummy_.~Dependency<P*, M>();
    dep_ = std::move(that.dep_);
    new (&that.dummy_) Dependency<P*, M>();
    return *this;
  }

  void Reset() {
    if (dep_ != nullptr) {
      dep_.reset();
      new (&dummy_) Dependency<P*, M>();
    }
  }

  void Reset(const M& manager) {
    if (dep_ == nullptr) {
      dummy_.~Dependency<P*, M>();
      dep_ = std::make_unique<Dependency<P*, M>>(manager);
    } else {
      dep_->Reset(manager);
    }
  }
  void Reset(M&& manager) {
    if (dep_ == nullptr) {
      dummy_.~Dependency<P*, M>();
      dep_ = std::make_unique<Dependency<P*, M>>(std::move(manager));
    } else {
      dep_->Reset(std::move(manager));
    }
  }

  template <typename... ManagerArgs>
  void Reset(std::tuple<ManagerArgs...> manager_args) {
    if (dep_ == nullptr) {
      dummy_.~Dependency<P*, M>();
      dep_ = std::make_unique<Dependency<P*, M>>(std::move(manager_args));
    } else {
      dep_->Reset(std::move(manager_args));
    }
  }

  M& manager() {
    if (ABSL_PREDICT_FALSE(dep_ == nullptr)) return dummy_.manager();
    return dep_->manager();
  }
  const M& manager() const {
    if (ABSL_PREDICT_FALSE(dep_ == nullptr)) return dummy_.manager();
    return dep_->manager();
  }

  DerivedP* get() {
    if (ABSL_PREDICT_FALSE(dep_ == nullptr)) return dummy_.get();
    return dep_->get();
  }
  const DerivedP* get() const {
    if (ABSL_PREDICT_FALSE(dep_ == nullptr)) return dummy_.get();
    return dep_->get();
  }
  DerivedP& operator*() { return *get(); }
  const DerivedP& operator*() const { return *get(); }
  DerivedP* operator->() { return get(); }
  const DerivedP* operator->() const { return get(); }

  bool is_owning() const { return dep_->is_owning(); }

 private:
  std::unique_ptr<Dependency<P*, M>> dep_;
  union {
    Dependency<P*, M> dummy_;
  };

  // Invariants:
  //   if `dep_ != nullptr` then `dummy_` is not constructed
  //   if `dep_ == nullptr` then `dummy_` is default constructed
};

}  // namespace riegeli

#endif  // RIEGELI_BASE_STABLE_DEPENDENCY_H_
