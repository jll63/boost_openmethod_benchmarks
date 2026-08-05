// Copyright (c) 2026 Jean-Louis Leroy
// Distributed under the Boost Software License, Version 1.0.

#ifndef OMB_HIERARCHY_HPP
#define OMB_HIERARCHY_HPP

#include <array>
#include <cstddef>
#include <memory>
#include <utility>

#include <boost/openmethod/inplace_vptr.hpp>

#include "timing.hpp"

// Number of leaf classes. Compile-time, because the leaves are a class
// template: changing it means recompiling. Override with -DOMB_CLASSES=n.
#ifndef OMB_CLASSES
#define OMB_CLASSES 100
#endif

namespace omb {

constexpr std::size_t num_classes = OMB_CLASSES;

// The one hierarchy used by every measurement, so that the virtual function
// yardsticks and the open-methods dispatch on exactly the same objects.
struct Base {
    explicit Base(int t) : tag(t) {
    }

    virtual ~Base() = default;

    // The receiver-touch baseline: a plain call that loads the receiver and
    // stamps. It reads `tag` on the same cache line as the v-table pointer, so
    // it pays the same one object miss a dispatch does. noinline so it is a
    // real call, symmetric with the dispatched rows and the direct reference.
    __attribute__((noinline)) auto touch() const -> stamp_id {
        auto t = tag;
        return {stop_stamp(), t};
    }

    // 1-argument yardstick: one virtual call. The body returns a constant:
    // the receiver is loaded only because virtual dispatch has to (the vptr
    // lives in it).
    virtual auto vf() const -> stamp_id = 0;

    // Use-flavored yardstick: same call, but the body READS the receiver.
    // Pairs with the use-flavored methods, where every body reads a member of
    // every receiver, so `disp = mean - touch` is fair for all call forms --
    // including virtual_ptr, which pays its receiver miss in the body instead
    // of in the dispatch. See README, "Two fair comparisons".
    virtual auto vfu() const -> stamp_id = 0;

    // 2-argument yardstick: the double dispatch idiom. `dd` is the first
    // dispatch, `dd_with` the second. Two chained virtual calls, which is what
    // the idiom costs.
    //
    // The textbook idiom would declare one `dd_with_DerivedK` per leaf in
    // `Base`; with 100 leaves that is unwritable, and it would not change the
    // cost, which is two virtual calls either way. See README.
    virtual auto dd(const Base&) const -> stamp_id = 0;
    virtual auto dd_with(const Base&) const -> stamp_id = 0;

    // Use-flavored double dispatch: dd_withu's body reads a member of BOTH
    // receivers. Contract: x + a.tag + b.tag.
    virtual auto ddu(const Base&) const -> stamp_id = 0;
    virtual auto dd_withu(const Base&) const -> stamp_id = 0;

    int tag;
};

template<std::size_t N>
struct Derived : Base {
    Derived() : Base(static_cast<int>(N)) {
    }

    auto vf() const -> stamp_id override {
        return {stop_stamp(), static_cast<int>(N)};
    }

    auto vfu() const -> stamp_id override {
        auto t = tag; // member read, kept above the stamp by its mem clobber
        return {stop_stamp(), t};
    }

    auto dd(const Base& other) const -> stamp_id override {
        return other.dd_with(*this);
    }

    auto dd_with(const Base&) const -> stamp_id override {
        return {stop_stamp(), static_cast<int>(N)};
    }

    auto ddu(const Base& other) const -> stamp_id override {
        return other.dd_withu(*this);
    }

    auto dd_withu(const Base& other) const -> stamp_id override {
        auto t = tag + other.tag; // reads both receivers, above the stamp
        return {stop_stamp(), t};
    }
};

// ---------------------------------------------------------------------------
// Object population
// ---------------------------------------------------------------------------

using factory = auto (*)() -> std::unique_ptr<Base>;

template<std::size_t... I>
auto make_factories(std::index_sequence<I...>)
    -> std::array<factory, sizeof...(I)> {
    return {{+[]() -> std::unique_ptr<Base> {
        return std::make_unique<Derived<I>>();
    }...}};
}

inline auto factories() -> const std::array<factory, num_classes>& {
    static const auto table =
        make_factories(std::make_index_sequence<num_classes>{});
    return table;
}

// ---------------------------------------------------------------------------
// The inplace_vptr hierarchy
//
// A second hierarchy is unavoidable: inplace_vptr_base declares
// `friend auto boost_openmethod_registry(Class*) -> Registry`, so a class binds
// to exactly one registry, and the direct and indirect variants therefore need
// one hierarchy each (both come from this template).
//
// It carries the same members as Base/Derived so the yardsticks measure the
// same work, but the objects are larger -- 24 bytes against 16 -- because the
// v-table pointer now lives in the object. That is why the inplace variants get
// their own `touch` baseline and `vf` yardstick rather than borrowing the main
// hierarchy's: `disp` and `x vf` must be computed against objects of the same
// shape.
// ---------------------------------------------------------------------------

template<class Registry>
struct IBase : boost::openmethod::inplace_vptr_base<IBase<Registry>, Registry> {
    explicit IBase(int t) : tag(t) {
    }

    virtual ~IBase() = default;

    __attribute__((noinline)) auto touch() const -> stamp_id {
        auto t = tag;
        return {stop_stamp(), t};
    }

    virtual auto vf() const -> stamp_id = 0;
    virtual auto vfu() const -> stamp_id = 0;
    virtual auto dd(const IBase&) const -> stamp_id = 0;
    virtual auto dd_with(const IBase&) const -> stamp_id = 0;
    virtual auto ddu(const IBase&) const -> stamp_id = 0;
    virtual auto dd_withu(const IBase&) const -> stamp_id = 0;

    int tag;
};

template<class Registry, std::size_t N>
struct IDerived
    : IBase<Registry>,
      boost::openmethod::inplace_vptr_derived<
          IDerived<Registry, N>, IBase<Registry>> {
    IDerived() : IBase<Registry>(static_cast<int>(N)) {
    }

    auto vf() const -> stamp_id override {
        return {stop_stamp(), static_cast<int>(N)};
    }

    auto vfu() const -> stamp_id override {
        auto t = this->tag;
        return {stop_stamp(), t};
    }

    auto dd(const IBase<Registry>& other) const -> stamp_id override {
        return other.dd_with(*this);
    }

    auto dd_with(const IBase<Registry>&) const -> stamp_id override {
        return {stop_stamp(), static_cast<int>(N)};
    }

    auto ddu(const IBase<Registry>& other) const -> stamp_id override {
        return other.dd_withu(*this);
    }

    auto dd_withu(const IBase<Registry>& other) const -> stamp_id override {
        auto t = this->tag + other.tag;
        return {stop_stamp(), t};
    }
};

// Instantiating every constructor is also what *registers* every class: the
// inplace_vptr_base constructor ODR-uses a static registrar per (Class,
// Registry). A leaf that is never constructed is never registered, and
// initialize() then rejects the overrider that names it.
template<class R>
using ifactory = auto (*)() -> std::unique_ptr<IBase<R>>;

template<class R, std::size_t... I>
auto make_ifactories(std::index_sequence<I...>)
    -> std::array<ifactory<R>, sizeof...(I)> {
    return {{+[]() -> std::unique_ptr<IBase<R>> {
        return std::make_unique<IDerived<R, I>>();
    }...}};
}

template<class R>
auto ifactories() -> const std::array<ifactory<R>, num_classes>& {
    static const auto table =
        make_ifactories<R>(std::make_index_sequence<num_classes>{});
    return table;
}

} // namespace omb

#endif
