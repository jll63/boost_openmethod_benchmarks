// Copyright (c) 2026 Jean-Louis Leroy
// Distributed under the Boost Software License, Version 1.0.

#ifndef OMB_HIERARCHY_HPP
#define OMB_HIERARCHY_HPP

#include <array>
#include <cstddef>
#include <memory>
#include <utility>

#include <boost/openmethod/inplace_vptr.hpp>

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

    // Object-touch baseline: no dispatch of any kind, but it does load from the
    // object, which `ovh` does not. Subtracting this instead of `ovh` separates
    // the cost of reaching a cold object from the cost of dispatching on it.
    //
    // Inlines to a single load; that is the point. It reads `tag` at offset 8,
    // the same cache line as the v-table pointer a dispatch reads at offset 0,
    // so it pays exactly the same one object miss.
    auto nvf(int x) const -> int {
        return x + tag;
    }

    // 1-argument yardstick: one virtual call. The body returns a constant:
    // the receiver is loaded only because virtual dispatch has to (the vptr
    // lives in it).
    virtual auto vf(int) const -> int = 0;

    // Use-flavored yardstick: same call, but the body READS the receiver.
    // Pairs with the use-flavored methods, where every body reads a member of
    // every receiver, so `disp = mean - nvf` is fair for all call forms --
    // including virtual_ptr, which pays its receiver miss in the body instead
    // of in the dispatch. See README, "Two fair comparisons".
    virtual auto vfu(int) const -> int = 0;

    // 2-argument yardstick: the double dispatch idiom. `dd` is the first
    // dispatch, `dd_with` the second. Two chained virtual calls, which is what
    // the idiom costs.
    //
    // The textbook idiom would declare one `dd_with_DerivedK` per leaf in
    // `Base`; with 100 leaves that is unwritable, and it would not change the
    // cost, which is two virtual calls either way. See README.
    virtual auto dd(const Base&, int) const -> int = 0;
    virtual auto dd_with(const Base&, int) const -> int = 0;

    // Use-flavored double dispatch: dd_withu's body reads a member of BOTH
    // receivers. Contract: x + a.tag + b.tag.
    virtual auto ddu(const Base&, int) const -> int = 0;
    virtual auto dd_withu(const Base&, int) const -> int = 0;

    int tag;
};

template<std::size_t N>
struct Derived : Base {
    Derived() : Base(static_cast<int>(N)) {
    }

    auto vf(int x) const -> int override {
        return x + static_cast<int>(N);
    }

    auto vfu(int x) const -> int override {
        return x + tag; // member read: the use-world body
    }

    auto dd(const Base& other, int x) const -> int override {
        return other.dd_with(*this, x);
    }

    auto dd_with(const Base&, int x) const -> int override {
        return x + static_cast<int>(N);
    }

    auto ddu(const Base& other, int x) const -> int override {
        return other.dd_withu(*this, x);
    }

    auto dd_withu(const Base& other, int x) const -> int override {
        return x + tag + other.tag; // reads both receivers
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
// their own `nvf` baseline and `vf` yardstick rather than borrowing the main
// hierarchy's: `disp` and `x vf` must be computed against objects of the same
// shape.
// ---------------------------------------------------------------------------

template<class Registry>
struct IBase : boost::openmethod::inplace_vptr_base<IBase<Registry>, Registry> {
    explicit IBase(int t) : tag(t) {
    }

    virtual ~IBase() = default;

    auto nvf(int x) const -> int {
        return x + tag;
    }

    virtual auto vf(int) const -> int = 0;
    virtual auto vfu(int) const -> int = 0;
    virtual auto dd(const IBase&, int) const -> int = 0;
    virtual auto dd_with(const IBase&, int) const -> int = 0;
    virtual auto ddu(const IBase&, int) const -> int = 0;
    virtual auto dd_withu(const IBase&, int) const -> int = 0;

    int tag;
};

template<class Registry, std::size_t N>
struct IDerived
    : IBase<Registry>,
      boost::openmethod::inplace_vptr_derived<
          IDerived<Registry, N>, IBase<Registry>> {
    IDerived() : IBase<Registry>(static_cast<int>(N)) {
    }

    auto vf(int x) const -> int override {
        return x + static_cast<int>(N);
    }

    auto vfu(int x) const -> int override {
        return x + this->tag;
    }

    auto dd(const IBase<Registry>& other, int x) const -> int override {
        return other.dd_with(*this, x);
    }

    auto dd_with(const IBase<Registry>&, int x) const -> int override {
        return x + static_cast<int>(N);
    }

    auto ddu(const IBase<Registry>& other, int x) const -> int override {
        return other.dd_withu(*this, x);
    }

    auto dd_withu(const IBase<Registry>& other, int x) const -> int override {
        return x + this->tag + other.tag;
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
