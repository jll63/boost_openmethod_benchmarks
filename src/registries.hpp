// Copyright (c) 2026 Jean-Louis Leroy
// Distributed under the Boost Software License, Version 1.0.

#ifndef OMB_REGISTRIES_HPP
#define OMB_REGISTRIES_HPP

#include <cstddef>
#include <utility>
#include <vector>

#include <boost/mp11.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <boost/openmethod.hpp>
#include <boost/openmethod/policies/vptr_map.hpp>

#include "hierarchy.hpp"
#include "timing.hpp"

namespace omb {

namespace om = boost::openmethod;
namespace pol = boost::openmethod::policies;
namespace mp = boost::mp11;

// ---------------------------------------------------------------------------
// The three registries
//
// A registry is identified by its *policy list*, not by the struct that derives
// from it (preamble.hpp:1134). These three lists differ, so these are three
// distinct registries with three distinct states. `initialize` must be called
// for each.
// ---------------------------------------------------------------------------

// std_rtti + fast_perfect_hash + vptr_vector
using vector_registry = om::default_registry;

// vptr_map over std::unordered_map. `without<type_hash>` because vptr_map does
// not use the hash; keeping fast_perfect_hash would only build a table nothing
// reads. Matches test/test_virtual_ptr_value_semantics.hpp:65.
struct map_registry
    : om::default_registry::with<pol::vptr_map<>>::without<pol::type_hash> {};

// vptr_map over boost::unordered_flat_map. vptr_map takes an mp11 *quoted
// metafunction*, not a template-template parameter (policies/vptr_map.hpp:32),
// and unordered_flat_map has the same 5-parameter shape as std::unordered_map.
struct flat_registry
    : om::default_registry
          ::with<pol::vptr_map<mp::mp_quote<boost::unordered_flat_map>>>
          ::without<pol::type_hash> {};

// The default policies plus indirect_vptr, which the library predefines
// (default_registry.hpp:78). With it, vptr_vector holds `const vptr_type*`
// rather than `vptr_type`, and a virtual_ptr stores a pointer to the v-table
// pointer instead of the v-table pointer itself (core.hpp:751-754) -- one more
// load on every dispatch, in exchange for v-tables that can be replaced after
// initialize() without rebuilding every virtual_ptr in flight.
using indirect_registry = om::indirect_registry;

// The two inplace_vptr registries. With the v-table pointer in the object there
// is nothing to look it up in and nothing to hash, so both the vptr policy and
// the type_hash it depends on come out (inplace_vptr.hpp:73-77).
struct inplace_registry
    : om::default_registry::without<pol::vptr, pol::type_hash> {};

struct inplace_indirect_registry
    : inplace_registry::with<pol::indirect_vptr> {};

// ---------------------------------------------------------------------------
// Class registration
// ---------------------------------------------------------------------------

template<class I>
using derived_at = Derived<I::value>;

using derived_list = mp::mp_transform<derived_at, mp::mp_iota_c<num_classes>>;

template<class R>
using class_list = mp::mp_push_back<mp::mp_push_front<derived_list, Base>, R>;

// ---------------------------------------------------------------------------
// Methods
//
// Four methods per registry: {reference, virtual_ptr} x {arity 1, arity 2}.
// Declared with the core `method` template rather than BOOST_OPENMETHOD,
// because the overriders are generated over a class template and have to be
// registered in bulk.
// ---------------------------------------------------------------------------

struct poke_ref_id;
struct poke_vp_id;
struct collide_ref_id;
struct collide_vp_id;

template<class R>
using poke_ref = om::method<poke_ref_id, int(om::virtual_<const Base&>, int), R>;

template<class R>
using poke_vp =
    om::method<poke_vp_id, int(om::virtual_ptr<const Base, R>, int), R>;

template<class R>
using collide_ref = om::method<
    collide_ref_id,
    int(om::virtual_<const Base&>, om::virtual_<const Base&>, int), R>;

template<class R>
using collide_vp = om::method<
    collide_vp_id,
    int(om::virtual_ptr<const Base, R>, om::virtual_ptr<const Base, R>, int),
    R>;

// ---------------------------------------------------------------------------
// Overriders
//
// Bodies are as trivial as the yardsticks' so that the measurement is dominated
// by dispatch. Arity 2 gets the diagonal (Derived<I>, Derived<I>) plus a
// (Base, Base) catch-all: that is N+1 groups per dimension, i.e. a genuinely
// two-dimensional dispatch table, without the N^2 overriders a full cross
// product would need.
// ---------------------------------------------------------------------------

template<class R, std::size_t I>
auto poke_ref_impl(const Derived<I>&, int x) -> int {
    return x + static_cast<int>(I);
}

template<class R, std::size_t I>
auto poke_vp_impl(om::virtual_ptr<const Derived<I>, R>, int x) -> int {
    return x + static_cast<int>(I);
}

template<class R>
auto collide_ref_base(const Base&, const Base&, int x) -> int {
    return x;
}

template<class R, std::size_t I>
auto collide_ref_impl(const Derived<I>&, const Derived<I>&, int x) -> int {
    return x + static_cast<int>(I);
}

template<class R>
auto collide_vp_base(
    om::virtual_ptr<const Base, R>, om::virtual_ptr<const Base, R>, int x)
    -> int {
    return x;
}

template<class R, std::size_t I>
auto collide_vp_impl(
    om::virtual_ptr<const Derived<I>, R>, om::virtual_ptr<const Derived<I>, R>,
    int x) -> int {
    return x + static_cast<int>(I);
}

// `override` accepts a pack of functions, so one static registrar object can
// carry all N overriders. These are declared, never defined: they are only used
// inside decltype.
template<class R, std::size_t... I>
auto poke_ref_registrar(std::index_sequence<I...>)
    -> typename poke_ref<R>::template override<poke_ref_impl<R, I>...>;

template<class R, std::size_t... I>
auto poke_vp_registrar(std::index_sequence<I...>)
    -> typename poke_vp<R>::template override<poke_vp_impl<R, I>...>;

template<class R, std::size_t... I>
auto collide_ref_registrar(std::index_sequence<I...>) ->
    typename collide_ref<R>::template override<
        collide_ref_base<R>, collide_ref_impl<R, I>...>;

template<class R, std::size_t... I>
auto collide_vp_registrar(std::index_sequence<I...>) ->
    typename collide_vp<R>::template override<
        collide_vp_base<R>, collide_vp_impl<R, I>...>;

using all_indices = std::make_index_sequence<num_classes>;

#define OMB_REGISTER(R)                                                        \
    BOOST_OPENMETHOD_REGISTER(mp::mp_apply<om::use_classes, class_list<R>>);   \
    BOOST_OPENMETHOD_REGISTER(                                                 \
        decltype(poke_ref_registrar<R>(all_indices{})));                       \
    BOOST_OPENMETHOD_REGISTER(decltype(poke_vp_registrar<R>(all_indices{})));  \
    BOOST_OPENMETHOD_REGISTER(                                                 \
        decltype(collide_ref_registrar<R>(all_indices{})));                    \
    BOOST_OPENMETHOD_REGISTER(decltype(collide_vp_registrar<R>(all_indices{})))

OMB_REGISTER(vector_registry);
OMB_REGISTER(map_registry);
OMB_REGISTER(flat_registry);
OMB_REGISTER(indirect_registry);

#undef OMB_REGISTER

// ---------------------------------------------------------------------------
// Methods on the inplace hierarchy
//
// Reference form only. A virtual_ptr would be pointless here: its whole purpose
// is to carry a v-table pointer that would otherwise have to be looked up, and
// with inplace_vptr the object already holds one, so the reference form does no
// lookup either. Building a virtual_ptr would only copy the same pointer into a
// wider argument.
//
// No use_classes registration either: inplace_vptr_base and
// inplace_vptr_derived register their classes themselves.
// ---------------------------------------------------------------------------

struct ipoke_ref_id;
struct icollide_ref_id;

template<class R>
using ipoke_ref =
    om::method<ipoke_ref_id, int(om::virtual_<const IBase<R>&>, int), R>;

template<class R>
using icollide_ref = om::method<
    icollide_ref_id,
    int(om::virtual_<const IBase<R>&>, om::virtual_<const IBase<R>&>, int), R>;

template<class R, std::size_t I>
auto ipoke_ref_impl(const IDerived<R, I>&, int x) -> int {
    return x + static_cast<int>(I);
}

template<class R>
auto icollide_ref_base(const IBase<R>&, const IBase<R>&, int x) -> int {
    return x;
}

template<class R, std::size_t I>
auto icollide_ref_impl(
    const IDerived<R, I>&, const IDerived<R, I>&, int x) -> int {
    return x + static_cast<int>(I);
}

template<class R, std::size_t... I>
auto ipoke_ref_registrar(std::index_sequence<I...>)
    -> typename ipoke_ref<R>::template override<ipoke_ref_impl<R, I>...>;

template<class R, std::size_t... I>
auto icollide_ref_registrar(std::index_sequence<I...>) ->
    typename icollide_ref<R>::template override<
        icollide_ref_base<R>, icollide_ref_impl<R, I>...>;

#define OMB_REGISTER_INPLACE(R)                                                \
    BOOST_OPENMETHOD_REGISTER(                                                 \
        decltype(ipoke_ref_registrar<R>(all_indices{})));                      \
    BOOST_OPENMETHOD_REGISTER(                                                 \
        decltype(icollide_ref_registrar<R>(all_indices{})))

OMB_REGISTER_INPLACE(inplace_registry);
OMB_REGISTER_INPLACE(inplace_indirect_registry);

#undef OMB_REGISTER_INPLACE

// ---------------------------------------------------------------------------
// Regions to flush in `clflush` mode
//
// Reaches into boost::openmethod::detail for the dispatch table arena
// (registry_state_type::dispatch_data, preamble.hpp:931). That is deliberate:
// the point of this mode is to evict exactly what the dispatch reads.
// ---------------------------------------------------------------------------

// With indirect_vptr, what the vptr container holds is the address of
// `Registry::static_vptr<Class>`, and *that* is what the dispatch dereferences.
// Those are ordinary variable-template globals, so flushing them means naming
// every class -- which this benchmark can do, since it knows the whole
// hierarchy at compile time.
template<class R, std::size_t... I>
void static_vptr_regions(
    std::vector<region>& out, std::index_sequence<I...>) {
    out.push_back({&R::template static_vptr<Base>, sizeof(om::vptr_type)});
    (out.push_back(
         {&R::template static_vptr<Derived<I>>, sizeof(om::vptr_type)}),
     ...);
}

// Same, for the inplace hierarchy. Those registries have no vptr policy at all,
// so the only shared state on the dispatch path is the v-table arena -- plus,
// when indirect, the per-class static_vptr globals the in-object pointer points
// at.
template<class R, std::size_t... I>
void istatic_vptr_regions(
    std::vector<region>& out, std::index_sequence<I...>) {
    out.push_back({&R::template static_vptr<IBase<R>>, sizeof(om::vptr_type)});
    (out.push_back(
         {&R::template static_vptr<IDerived<R, I>>, sizeof(om::vptr_type)}),
     ...);
}

template<class R>
void inplace_registry_regions(std::vector<region>& out) {
    auto& dispatch = R::state().dispatch_data;
    out.push_back({dispatch.data(), dispatch.size() * sizeof(dispatch[0])});

    if constexpr (R::has_indirect_vptr) {
        istatic_vptr_regions<R>(out, all_indices{});
    }
}

template<class R>
void registry_regions(std::vector<region>& out) {
    auto& dispatch = R::state().dispatch_data;
    out.push_back({dispatch.data(), dispatch.size() * sizeof(dispatch[0])});

    if constexpr (std::is_same_v<R, map_registry>) {
        auto& vptrs = R::template state<pol::vptr_map<>>().vptrs;
        out.push_back({&vptrs, sizeof(vptrs)});
    } else if constexpr (std::is_same_v<R, flat_registry>) {
        auto& vptrs =
            R::template state<
                 pol::vptr_map<mp::mp_quote<boost::unordered_flat_map>>>()
                .vptrs;
        out.push_back({&vptrs, sizeof(vptrs)});
    } else {
        // vector_registry and indirect_registry: both vptr_vector + hash.
        auto& vptrs = R::template state<pol::vptr_vector>().vptrs;
        out.push_back({vptrs.data(), vptrs.size() * sizeof(vptrs[0])});

        auto& hash = R::template state<pol::fast_perfect_hash>();
        out.push_back({&hash, sizeof(hash)});
    }

    if constexpr (R::has_indirect_vptr) {
        static_vptr_regions<R>(out, all_indices{});
    }
}

} // namespace omb

#endif
