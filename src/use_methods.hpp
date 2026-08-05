// Copyright (c) 2026 Jean-Louis Leroy
// Distributed under the Boost Software License, Version 1.0.

#ifndef OMB_USE_METHODS_HPP
#define OMB_USE_METHODS_HPP

// The use-flavored methods: same dispatch mechanisms as registries.hpp, but
// every overrider body READS a member of every receiver.
//
// The const-body methods measure *delivery*: getting to the right code, the
// receiver untouched by anything the mechanism does not itself require. That
// is the world where virtual_ptr's structural advantage is at its largest --
// it alone can dispatch without loading the receiver.
//
// These methods measure the *use* world: the body touches the object, as real
// code almost always does. Every call form then owes the receiver's cache
// line exactly once, so `disp = mean - touch` is fair for every row, and the
// interesting physics becomes measurable: a virtual function's mandatory
// receiver load doubles as a prefetch for its body, while a virtual_ptr call
// pays its receiver miss inside the body -- serialized after the table misses
// or overlapped under them, depending on what the core's speculation does
// across a mispredicted indirect call. That is not derivable; it is why these
// methods exist. See README, "Two fair comparisons".
//
// Contracts (the verify oracles, carried in stamp_id::id):
//   pokeu:    a.tag
//   collideu: same leaf -> a.tag + b.tag, else -> -a.tag - b.tag - 1
//             (distinct for every pair, so verify can tell which overrider
//             ran; both read both receivers)

#include "registries.hpp"

namespace omb {

struct pokeu_ref_id;
struct pokeu_vp_id;
struct collideu_ref_id;
struct collideu_vp_id;
struct ipokeu_ref_id;
struct icollideu_ref_id;

template<class R>
using pokeu_ref =
    om::method<pokeu_ref_id, stamp_id(om::virtual_<const Base&>), R>;

template<class R>
using pokeu_vp =
    om::method<pokeu_vp_id, stamp_id(om::virtual_ptr<const Base, R>), R>;

template<class R>
using collideu_ref = om::method<
    collideu_ref_id,
    stamp_id(om::virtual_<const Base&>, om::virtual_<const Base&>), R>;

template<class R>
using collideu_vp = om::method<
    collideu_vp_id,
    stamp_id(om::virtual_ptr<const Base, R>, om::virtual_ptr<const Base, R>),
    R>;

// ---------------------------------------------------------------------------
// Overriders. Every body loads `tag` from every receiver -- the member reads
// are the point, and the disassembly check in the README confirms the
// compiler cannot fold them (tag is a runtime member reached through an
// indirect call).
// ---------------------------------------------------------------------------

template<class R, std::size_t I>
auto pokeu_ref_impl(const Derived<I>& self) -> stamp_id {
    auto t = self.tag; // the use-world receiver read, above the stamp
    return {stop_stamp(), t};
}

template<class R, std::size_t I>
auto pokeu_vp_impl(om::virtual_ptr<const Derived<I>, R> self) -> stamp_id {
    auto t = self->tag; // deferred receiver read: the overlap under test
    return {stop_stamp(), t};
}

template<class R>
auto collideu_ref_base(const Base& a, const Base& b) -> stamp_id {
    auto t = -a.tag - b.tag - 1;
    return {stop_stamp(), t};
}

template<class R, std::size_t I>
auto collideu_ref_impl(const Derived<I>& a, const Derived<I>& b) -> stamp_id {
    auto t = a.tag + b.tag;
    return {stop_stamp(), t};
}

template<class R>
auto collideu_vp_base(
    om::virtual_ptr<const Base, R> a, om::virtual_ptr<const Base, R> b)
    -> stamp_id {
    auto t = -a->tag - b->tag - 1;
    return {stop_stamp(), t};
}

template<class R, std::size_t I>
auto collideu_vp_impl(
    om::virtual_ptr<const Derived<I>, R> a,
    om::virtual_ptr<const Derived<I>, R> b) -> stamp_id {
    auto t = a->tag + b->tag;
    return {stop_stamp(), t};
}

template<class R, std::size_t... I>
auto pokeu_ref_registrar(std::index_sequence<I...>)
    -> typename pokeu_ref<R>::template override<pokeu_ref_impl<R, I>...>;

template<class R, std::size_t... I>
auto pokeu_vp_registrar(std::index_sequence<I...>)
    -> typename pokeu_vp<R>::template override<pokeu_vp_impl<R, I>...>;

template<class R, std::size_t... I>
auto collideu_ref_registrar(std::index_sequence<I...>) ->
    typename collideu_ref<R>::template override<
        collideu_ref_base<R>, collideu_ref_impl<R, I>...>;

template<class R, std::size_t... I>
auto collideu_vp_registrar(std::index_sequence<I...>) ->
    typename collideu_vp<R>::template override<
        collideu_vp_base<R>, collideu_vp_impl<R, I>...>;

// The classes are already registered by registries.hpp; only the overrider
// packs are new.
#define OMB_REGISTER_USE(R)                                                    \
    BOOST_OPENMETHOD_REGISTER(                                                 \
        decltype(pokeu_ref_registrar<R>(all_indices{})));                      \
    BOOST_OPENMETHOD_REGISTER(decltype(pokeu_vp_registrar<R>(all_indices{}))); \
    BOOST_OPENMETHOD_REGISTER(                                                 \
        decltype(collideu_ref_registrar<R>(all_indices{})));                   \
    BOOST_OPENMETHOD_REGISTER(decltype(collideu_vp_registrar<R>(all_indices{})))

OMB_REGISTER_USE(vector_registry);
OMB_REGISTER_USE(map_registry);
OMB_REGISTER_USE(flat_registry);
OMB_REGISTER_USE(indirect_registry);

#undef OMB_REGISTER_USE

// ---------------------------------------------------------------------------
// Inplace hierarchy, reference form only (same rationale as registries.hpp)
// ---------------------------------------------------------------------------

template<class R>
using ipokeu_ref =
    om::method<ipokeu_ref_id, stamp_id(om::virtual_<const IBase<R>&>), R>;

template<class R>
using icollideu_ref = om::method<
    icollideu_ref_id,
    stamp_id(om::virtual_<const IBase<R>&>, om::virtual_<const IBase<R>&>), R>;

template<class R, std::size_t I>
auto ipokeu_ref_impl(const IDerived<R, I>& self) -> stamp_id {
    auto t = self.tag;
    return {stop_stamp(), t};
}

template<class R>
auto icollideu_ref_base(const IBase<R>& a, const IBase<R>& b) -> stamp_id {
    auto t = -a.tag - b.tag - 1;
    return {stop_stamp(), t};
}

template<class R, std::size_t I>
auto icollideu_ref_impl(
    const IDerived<R, I>& a, const IDerived<R, I>& b) -> stamp_id {
    auto t = a.tag + b.tag;
    return {stop_stamp(), t};
}

template<class R, std::size_t... I>
auto ipokeu_ref_registrar(std::index_sequence<I...>)
    -> typename ipokeu_ref<R>::template override<ipokeu_ref_impl<R, I>...>;

template<class R, std::size_t... I>
auto icollideu_ref_registrar(std::index_sequence<I...>) ->
    typename icollideu_ref<R>::template override<
        icollideu_ref_base<R>, icollideu_ref_impl<R, I>...>;

#define OMB_REGISTER_USE_INPLACE(R)                                            \
    BOOST_OPENMETHOD_REGISTER(                                                 \
        decltype(ipokeu_ref_registrar<R>(all_indices{})));                     \
    BOOST_OPENMETHOD_REGISTER(                                                 \
        decltype(icollideu_ref_registrar<R>(all_indices{})))

OMB_REGISTER_USE_INPLACE(inplace_registry);
OMB_REGISTER_USE_INPLACE(inplace_indirect_registry);

#undef OMB_REGISTER_USE_INPLACE

} // namespace omb

#endif
