// Copyright (c) 2026 Jean-Louis Leroy
// Distributed under the Boost Software License, Version 1.0.
//
// The oracle for asmtab.py: every dispatch, compiled on its own.
//
// asmtab.py publishes the dispatch as it appears inside the *timed* window,
// which is the code that actually ran -- but that window also contains the
// timing rig (the start stamp being assembled, the `cycles` out-parameter
// being parked, the i386 PIC base, the i386 argument marshalling), and a
// backward slice is what separates the two. A slice is a heuristic, and
// compilers move things: clang 22 alone moved the PIC base and the
// out-parameter save from the prologue into the window.
//
// So the same dispatches are compiled here with nothing around them. These
// functions contain the dispatch and nothing else, which makes them an
// independent statement of what the slice must not lose: every instruction
// here has to appear in the corresponding sliced window. asmtab.py checks
// that and fails loudly otherwise.
//
// They are not published, and they are not what is measured -- with no return
// value to keep, the compiler tail-calls the overrider (`jmp` where the real
// code has `call`) and allocates registers differently. Build with probes.sh.
//
// Each body mirrors the matching `V::call` in main.cpp exactly. If one drifts,
// the oracle reports a divergence that is this file's fault, not the slicer's.

#include "hierarchy.hpp"
#include "registries.hpp"
#include "timing.hpp"

namespace omb {

#define OMB_PROBE __attribute__((noinline)) auto

// The yardsticks: one virtual call, and the double-dispatch idiom's first.
OMB_PROBE probe_vf1(const Base* a) -> stamp_id {
    return a->vf();
}

OMB_PROBE probe_vf2(const Base* a, const Base* b) -> stamp_id {
    return a->dd(*b);
}

// The main hierarchy, both call forms, both arities.
template<class R>
OMB_PROBE probe_om_ref1(const Base* a) -> stamp_id {
    return poke_ref<R>::fn(*a);
}

template<class R>
OMB_PROBE probe_om_vp1(om::virtual_ptr<const Base, R> a) -> stamp_id {
    return poke_vp<R>::fn(a);
}

template<class R>
OMB_PROBE probe_om_ref2(const Base* a, const Base* b) -> stamp_id {
    return collide_ref<R>::fn(*a, *b);
}

template<class R>
OMB_PROBE probe_om_vp2(
    om::virtual_ptr<const Base, R> a, om::virtual_ptr<const Base, R> b)
    -> stamp_id {
    return collide_vp<R>::fn(a, b);
}

// The inplace hierarchies, reference form only.
template<class R>
OMB_PROBE probe_ip_ref1(const IBase<R>* a) -> stamp_id {
    return ipoke_ref<R>::fn(*a);
}

template<class R>
OMB_PROBE probe_ip_ref2(const IBase<R>* a, const IBase<R>* b) -> stamp_id {
    return icollide_ref<R>::fn(*a, *b);
}

// Explicit instantiations. asmtab.py finds each by function name plus the
// registry spelled in the mangled symbol.
template OMB_PROBE probe_om_ref1<vector_registry>(const Base*) -> stamp_id;
template OMB_PROBE probe_om_vp1<vector_registry>(
    om::virtual_ptr<const Base, vector_registry>) -> stamp_id;
template OMB_PROBE probe_om_ref2<vector_registry>(const Base*, const Base*)
    -> stamp_id;
template OMB_PROBE probe_om_vp2<vector_registry>(
    om::virtual_ptr<const Base, vector_registry>,
    om::virtual_ptr<const Base, vector_registry>) -> stamp_id;

template OMB_PROBE probe_om_ref1<indirect_registry>(const Base*) -> stamp_id;
template OMB_PROBE probe_om_vp1<indirect_registry>(
    om::virtual_ptr<const Base, indirect_registry>) -> stamp_id;
template OMB_PROBE probe_om_ref2<indirect_registry>(const Base*, const Base*)
    -> stamp_id;
template OMB_PROBE probe_om_vp2<indirect_registry>(
    om::virtual_ptr<const Base, indirect_registry>,
    om::virtual_ptr<const Base, indirect_registry>) -> stamp_id;

template OMB_PROBE probe_ip_ref1<inplace_registry>(
    const IBase<inplace_registry>*) -> stamp_id;
template OMB_PROBE probe_ip_ref2<inplace_registry>(
    const IBase<inplace_registry>*, const IBase<inplace_registry>*)
    -> stamp_id;
template OMB_PROBE probe_ip_ref1<inplace_indirect_registry>(
    const IBase<inplace_indirect_registry>*) -> stamp_id;
template OMB_PROBE probe_ip_ref2<inplace_indirect_registry>(
    const IBase<inplace_indirect_registry>*,
    const IBase<inplace_indirect_registry>*) -> stamp_id;

} // namespace omb

auto main() -> int {
    return 0;
}
