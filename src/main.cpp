// Copyright (c) 2026 Jean-Louis Leroy
// Distributed under the Boost Software License, Version 1.0.
//
// Times a single open-method dispatch with rdtsc, with the caches scrubbed
// beforehand. See README.md.

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "hierarchy.hpp"
#include "registries.hpp"
#include "use_methods.hpp"
#include "timing.hpp"

#include <boost/openmethod/initialize.hpp>

namespace omb {

// ---------------------------------------------------------------------------
// Build identification
//
// Emitted in the header and as the first two CSV columns, so a collected result
// file identifies the build it came from. clang also defines __GNUC__, so it
// has to be tested first.
// ---------------------------------------------------------------------------

inline auto compiler_id() -> std::string {
#if defined(__clang__)
    return "clang++ " + std::to_string(__clang_major__) + "." +
        std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "g++ " + std::to_string(__GNUC__) + "." +
        std::to_string(__GNUC_MINOR__);
#else
    return "unknown";
#endif
}

constexpr auto bitness = 8 * sizeof(void*);

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

struct config {
    std::size_t reps = 1000;
    std::size_t objects = 4096;
    std::size_t sweep_mb = 64; // 2x this machine's 32 MiB L3
    int cpu = 2;
    unsigned seed = 12345;
    bool csv = false;
    bool verify_only = false;
    bool warm = true;
    bool clflush = true;
    bool sweep = true;
};

template<class R>
using vp_vector = std::vector<om::virtual_ptr<const Base, R>>;

// One object population per inplace registry. They cannot share the main
// hierarchy's: an inplace class binds to a single registry.
template<class R>
struct ipopulation {
    void build(std::size_t n, std::mt19937& rng) {
        objects.reserve(n);
        ptrs.reserve(n);

        for (std::size_t i = 0; i != n; ++i) {
            objects.push_back(ifactories<R>()[i % num_classes]());
        }

        for (auto& obj : objects) {
            ptrs.push_back(obj.get());
        }

        std::shuffle(ptrs.begin(), ptrs.end(), rng);
    }

    std::vector<std::unique_ptr<IBase<R>>> objects;
    std::vector<const IBase<R>*> ptrs;
};

struct env {
    explicit env(const config& cfg)
        : scrub(cfg.sweep_mb * 1024 * 1024), rng(cfg.seed),
          pick(0, cfg.objects - 1) {
        objects.reserve(cfg.objects);
        ptrs.reserve(cfg.objects);

        // Round-robin over the leaf types, then shuffle the pointer array, so
        // that neither the type sequence nor the address sequence is
        // predictable at measurement time.
        for (std::size_t i = 0; i != cfg.objects; ++i) {
            objects.push_back(factories()[i % num_classes]());
        }

        for (auto& obj : objects) {
            ptrs.push_back(obj.get());
        }

        std::shuffle(ptrs.begin(), ptrs.end(), rng);

        vp_vec.reserve(cfg.objects);
        vp_map.reserve(cfg.objects);
        vp_flat.reserve(cfg.objects);
        vp_ind.reserve(cfg.objects);

        for (auto* p : ptrs) {
            vp_vec.emplace_back(*p);
            vp_map.emplace_back(*p);
            vp_flat.emplace_back(*p);
            vp_ind.emplace_back(*p);
        }

        ip_direct.build(cfg.objects, rng);
        ip_indirect.build(cfg.objects, rng);
    }

    auto draw() -> std::size_t {
        return pick(rng);
    }

    std::vector<std::unique_ptr<Base>> objects;
    std::vector<const Base*> ptrs;
    vp_vector<vector_registry> vp_vec;
    vp_vector<map_registry> vp_map;
    vp_vector<flat_registry> vp_flat;
    vp_vector<indirect_registry> vp_ind;
    ipopulation<inplace_registry> ip_direct;
    ipopulation<inplace_indirect_registry> ip_indirect;
    scrubber scrub;
    std::mt19937 rng;
    std::uniform_int_distribution<std::size_t> pick;
};

// Flush an object and the C++ v-table it points to.
//
// The v-table region starts 16 bytes BEFORE the address point: offset-to-top
// and the type_info pointer live at vptr-16/-8, and the type_info pointer is
// the first dependent load of every `om ref` dispatch. flush_range aligns its
// start down to a line, which covered vptr-8 only when the vptr was not
// 64-byte aligned -- a compiler-asymmetric hole (review finding: line-aligned
// vtables occur ~3x more often in the clang link layout than gcc's), which
// let those receivers skip a real cold miss.
template<class B>
inline void object_regions(const B* p, std::vector<region>& out) {
    out.push_back({p, sizeof(B) < 64 ? 64 : sizeof(B)});

    auto vtbl = *reinterpret_cast<const char* const*>(p);
    out.push_back({vtbl - 16, 512 + 16});
}

// ---------------------------------------------------------------------------
// Variants
//
// Each variant is a type with:
//   args      what the call needs, materialized *outside* the timed region
//   prepare   builds args from two object indices
//   call      the one thing being timed
//   regions   what `clflush` mode should evict
// ---------------------------------------------------------------------------

struct v_ovh {
    static constexpr bool touches_receiver = false;
    static constexpr const char* body = "-";
    static constexpr const char* name = "ovh";
    static constexpr const char* group = "baseline";
    static constexpr const char* hier = "main";
    static constexpr int arity = 0;

    using args = const Base*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return e.ptrs[i];
    }

    // No dispatch: measures the rdtsc pair plus reaching the call site with
    // cold code. Subtracting it from the others isolates the dispatch.
    static auto call(args, int x) -> int {
        return x;
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
    }
};

// Object-touch baselines. Same apparatus, same scrub, and they load the object,
// but they dispatch on nothing. `disp` is measured against these, so the cold
// miss on the receiver is charged to reaching the object rather than to
// dispatching on it.
struct v_nvf1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "-";
    static constexpr const char* name = "nvf";
    static constexpr const char* group = "baseline";
    static constexpr const char* hier = "main";
    static constexpr int arity = 1;

    using args = const Base*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return e.ptrs[i];
    }

    static auto call(args a, int x) -> int {
        return a->nvf(x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
    }
};

struct v_vf1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "const";
    static constexpr const char* name = "vf";
    static constexpr const char* group = "yardstick";
    static constexpr const char* hier = "main";
    static constexpr int arity = 1;

    using args = const Base*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return e.ptrs[i];
    }

    static auto call(args a, int x) -> int {
        return a->vf(x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
    }
};

struct pair_args {
    const Base* a;
    const Base* b;
};

struct v_nvf2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "-";
    static constexpr const char* name = "nvf+nvf";
    static constexpr const char* group = "baseline";
    static constexpr const char* hier = "main";
    static constexpr int arity = 2;

    using args = pair_args;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        return {e.ptrs[i], e.ptrs[j]};
    }

    // Touches both receivers, as every arity-2 dispatch does.
    static auto call(args p, int x) -> int {
        return p.a->nvf(x) + p.b->nvf(0);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a, out);
        object_regions(p.b, out);
    }
};

struct v_vf2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "const";
    static constexpr const char* name = "vf+vf";
    static constexpr const char* group = "yardstick";
    static constexpr const char* hier = "main";
    static constexpr int arity = 2;

    using args = pair_args;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        return {e.ptrs[i], e.ptrs[j]};
    }

    // The double dispatch idiom: dd() dispatches on a, then calls dd_with() on
    // b, which dispatches again. Two chained virtual calls.
    static auto call(args p, int x) -> int {
        return p.a->dd(*p.b, x);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a, out);
        object_regions(p.b, out);
    }
};

// Open-method variants, four per registry.

template<class R>
struct vp_traits;

template<>
struct vp_traits<vector_registry> {
    static auto array(env& e) -> vp_vector<vector_registry>& {
        return e.vp_vec;
    }
};

template<>
struct vp_traits<map_registry> {
    static auto array(env& e) -> vp_vector<map_registry>& {
        return e.vp_map;
    }
};

template<>
struct vp_traits<flat_registry> {
    static auto array(env& e) -> vp_vector<flat_registry>& {
        return e.vp_flat;
    }
};

template<>
struct vp_traits<indirect_registry> {
    static auto array(env& e) -> vp_vector<indirect_registry>& {
        return e.vp_ind;
    }
};

template<class R, const char* Group>
struct v_om_ref1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "const";
    static constexpr const char* name = "om ref";
    static constexpr const char* group = Group;
    static constexpr const char* hier = "main";
    static constexpr int arity = 1;

    using args = const Base*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return e.ptrs[i];
    }

    static auto call(args a, int x) -> int {
        return poke_ref<R>::fn(*a, x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
        out.push_back({&poke_ref<R>::fn, sizeof(poke_ref<R>)});
        registry_regions<R>(out);
    }
};

template<class R, const char* Group>
struct v_om_vp1 {
    static constexpr bool touches_receiver = false;
    static constexpr const char* body = "const";
    static constexpr const char* name = "om vptr";
    static constexpr const char* group = Group;
    static constexpr const char* hier = "main";
    static constexpr int arity = 1;

    using args = om::virtual_ptr<const Base, R>;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return vp_traits<R>::array(e)[i];
    }

    static auto call(args a, int x) -> int {
        return poke_vp<R>::fn(a, x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a.get(), out);
        out.push_back({&poke_vp<R>::fn, sizeof(poke_vp<R>)});
        registry_regions<R>(out);
    }
};

template<class R, const char* Group>
struct v_om_ref2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "const";
    static constexpr const char* name = "om ref";
    static constexpr const char* group = Group;
    static constexpr const char* hier = "main";
    static constexpr int arity = 2;

    using args = pair_args;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        return {e.ptrs[i], e.ptrs[j]};
    }

    static auto call(args p, int x) -> int {
        return collide_ref<R>::fn(*p.a, *p.b, x);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a, out);
        object_regions(p.b, out);
        out.push_back({&collide_ref<R>::fn, sizeof(collide_ref<R>)});
        registry_regions<R>(out);
    }
};

template<class R>
struct vp_pair_args {
    om::virtual_ptr<const Base, R> a;
    om::virtual_ptr<const Base, R> b;
};

template<class R, const char* Group>
struct v_om_vp2 {
    static constexpr bool touches_receiver = false;
    static constexpr const char* body = "const";
    static constexpr const char* name = "om vptr";
    static constexpr const char* group = Group;
    static constexpr const char* hier = "main";
    static constexpr int arity = 2;

    using args = vp_pair_args<R>;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        return {vp_traits<R>::array(e)[i], vp_traits<R>::array(e)[j]};
    }

    static auto call(args p, int x) -> int {
        return collide_vp<R>::fn(p.a, p.b, x);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a.get(), out);
        object_regions(p.b.get(), out);
        out.push_back({&collide_vp<R>::fn, sizeof(collide_vp<R>)});
        registry_regions<R>(out);
    }
};

// ---------------------------------------------------------------------------
// inplace_vptr variants
//
// Their own baselines and yardsticks, because the objects are a different shape
// (24 bytes against 16) and live in a different allocation. Reference form only
// -- see registries.hpp for why a virtual_ptr would be meaningless here.
// ---------------------------------------------------------------------------

template<class R>
struct ip_traits;

template<>
struct ip_traits<inplace_registry> {
    static auto pop(env& e) -> ipopulation<inplace_registry>& {
        return e.ip_direct;
    }
};

template<>
struct ip_traits<inplace_indirect_registry> {
    static auto pop(env& e) -> ipopulation<inplace_indirect_registry>& {
        return e.ip_indirect;
    }
};

template<class R>
struct ipair_args {
    const IBase<R>* a;
    const IBase<R>* b;
};

template<class R, const char* Hier>
struct v_ip_nvf1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "-";
    static constexpr const char* name = "nvf";
    static constexpr const char* group = "baseline";
    static constexpr const char* hier = Hier;
    static constexpr int arity = 1;

    using args = const IBase<R>*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return ip_traits<R>::pop(e).ptrs[i];
    }

    static auto call(args a, int x) -> int {
        return a->nvf(x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
    }
};

template<class R, const char* Hier>
struct v_ip_nvf2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "-";
    static constexpr const char* name = "nvf+nvf";
    static constexpr const char* group = "baseline";
    static constexpr const char* hier = Hier;
    static constexpr int arity = 2;

    using args = ipair_args<R>;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        auto& p = ip_traits<R>::pop(e).ptrs;
        return {p[i], p[j]};
    }

    static auto call(args p, int x) -> int {
        return p.a->nvf(x) + p.b->nvf(0);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a, out);
        object_regions(p.b, out);
    }
};

template<class R, const char* Hier>
struct v_ip_vf1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "const";
    static constexpr const char* name = "vf";
    static constexpr const char* group = "yardstick";
    static constexpr const char* hier = Hier;
    static constexpr int arity = 1;

    using args = const IBase<R>*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return ip_traits<R>::pop(e).ptrs[i];
    }

    static auto call(args a, int x) -> int {
        return a->vf(x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
    }
};

template<class R, const char* Hier>
struct v_ip_vf2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "const";
    static constexpr const char* name = "vf+vf";
    static constexpr const char* group = "yardstick";
    static constexpr const char* hier = Hier;
    static constexpr int arity = 2;

    using args = ipair_args<R>;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        auto& p = ip_traits<R>::pop(e).ptrs;
        return {p[i], p[j]};
    }

    static auto call(args p, int x) -> int {
        return p.a->dd(*p.b, x);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a, out);
        object_regions(p.b, out);
    }
};

template<class R, const char* Group, const char* Hier>
struct v_ip_ref1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "const";
    static constexpr const char* name = "om ref";
    static constexpr const char* group = Group;
    static constexpr const char* hier = Hier;
    static constexpr int arity = 1;

    using args = const IBase<R>*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return ip_traits<R>::pop(e).ptrs[i];
    }

    static auto call(args a, int x) -> int {
        return ipoke_ref<R>::fn(*a, x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
        out.push_back({&ipoke_ref<R>::fn, sizeof(ipoke_ref<R>)});
        inplace_registry_regions<R>(out);
    }
};

template<class R, const char* Group, const char* Hier>
struct v_ip_ref2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "const";
    static constexpr const char* name = "om ref";
    static constexpr const char* group = Group;
    static constexpr const char* hier = Hier;
    static constexpr int arity = 2;

    using args = ipair_args<R>;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        auto& p = ip_traits<R>::pop(e).ptrs;
        return {p[i], p[j]};
    }

    static auto call(args p, int x) -> int {
        return icollide_ref<R>::fn(*p.a, *p.b, x);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a, out);
        object_regions(p.b, out);
        out.push_back({&icollide_ref<R>::fn, sizeof(icollide_ref<R>)});
        inplace_registry_regions<R>(out);
    }
};

// ---------------------------------------------------------------------------
// Use-world variants: same shapes, use-flavored bodies. All touch the
// receiver -- that is the definition of this world -- so disp subtracts nvf
// for every one of them, including the virtual_ptr forms.
// ---------------------------------------------------------------------------

struct v_vfu1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "use";
    static constexpr const char* name = "vf";
    static constexpr const char* group = "yardstick";
    static constexpr const char* hier = "main";
    static constexpr int arity = 1;

    using args = const Base*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return e.ptrs[i];
    }

    static auto call(args a, int x) -> int {
        return a->vfu(x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
    }
};

struct v_vfu2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "use";
    static constexpr const char* name = "vf+vf";
    static constexpr const char* group = "yardstick";
    static constexpr const char* hier = "main";
    static constexpr int arity = 2;

    using args = pair_args;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        return {e.ptrs[i], e.ptrs[j]};
    }

    static auto call(args p, int x) -> int {
        return p.a->ddu(*p.b, x);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a, out);
        object_regions(p.b, out);
    }
};

template<class R, const char* Group>
struct v_om_refu1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "use";
    static constexpr const char* name = "om ref";
    static constexpr const char* group = Group;
    static constexpr const char* hier = "main";
    static constexpr int arity = 1;

    using args = const Base*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return e.ptrs[i];
    }

    static auto call(args a, int x) -> int {
        return pokeu_ref<R>::fn(*a, x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
        out.push_back({&pokeu_ref<R>::fn, sizeof(pokeu_ref<R>)});
        registry_regions<R>(out);
    }
};

template<class R, const char* Group>
struct v_om_vpu1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "use";
    static constexpr const char* name = "om vptr";
    static constexpr const char* group = Group;
    static constexpr const char* hier = "main";
    static constexpr int arity = 1;

    using args = om::virtual_ptr<const Base, R>;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return vp_traits<R>::array(e)[i];
    }

    static auto call(args a, int x) -> int {
        return pokeu_vp<R>::fn(a, x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a.get(), out);
        out.push_back({&pokeu_vp<R>::fn, sizeof(pokeu_vp<R>)});
        registry_regions<R>(out);
    }
};

template<class R, const char* Group>
struct v_om_refu2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "use";
    static constexpr const char* name = "om ref";
    static constexpr const char* group = Group;
    static constexpr const char* hier = "main";
    static constexpr int arity = 2;

    using args = pair_args;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        return {e.ptrs[i], e.ptrs[j]};
    }

    static auto call(args p, int x) -> int {
        return collideu_ref<R>::fn(*p.a, *p.b, x);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a, out);
        object_regions(p.b, out);
        out.push_back({&collideu_ref<R>::fn, sizeof(collideu_ref<R>)});
        registry_regions<R>(out);
    }
};

template<class R, const char* Group>
struct v_om_vpu2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "use";
    static constexpr const char* name = "om vptr";
    static constexpr const char* group = Group;
    static constexpr const char* hier = "main";
    static constexpr int arity = 2;

    using args = vp_pair_args<R>;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        return {vp_traits<R>::array(e)[i], vp_traits<R>::array(e)[j]};
    }

    static auto call(args p, int x) -> int {
        return collideu_vp<R>::fn(p.a, p.b, x);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a.get(), out);
        object_regions(p.b.get(), out);
        out.push_back({&collideu_vp<R>::fn, sizeof(collideu_vp<R>)});
        registry_regions<R>(out);
    }
};

template<class R, const char* Hier>
struct v_ip_vfu1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "use";
    static constexpr const char* name = "vf";
    static constexpr const char* group = "yardstick";
    static constexpr const char* hier = Hier;
    static constexpr int arity = 1;

    using args = const IBase<R>*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return ip_traits<R>::pop(e).ptrs[i];
    }

    static auto call(args a, int x) -> int {
        return a->vfu(x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
    }
};

template<class R, const char* Hier>
struct v_ip_vfu2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "use";
    static constexpr const char* name = "vf+vf";
    static constexpr const char* group = "yardstick";
    static constexpr const char* hier = Hier;
    static constexpr int arity = 2;

    using args = ipair_args<R>;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        auto& p = ip_traits<R>::pop(e).ptrs;
        return {p[i], p[j]};
    }

    static auto call(args p, int x) -> int {
        return p.a->ddu(*p.b, x);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a, out);
        object_regions(p.b, out);
    }
};

template<class R, const char* Group, const char* Hier>
struct v_ip_refu1 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "use";
    static constexpr const char* name = "om ref";
    static constexpr const char* group = Group;
    static constexpr const char* hier = Hier;
    static constexpr int arity = 1;

    using args = const IBase<R>*;

    static auto prepare(env& e, std::size_t i, std::size_t) -> args {
        return ip_traits<R>::pop(e).ptrs[i];
    }

    static auto call(args a, int x) -> int {
        return ipokeu_ref<R>::fn(*a, x);
    }

    static void regions(args a, std::vector<region>& out) {
        object_regions(a, out);
        out.push_back({&ipokeu_ref<R>::fn, sizeof(ipokeu_ref<R>)});
        inplace_registry_regions<R>(out);
    }
};

template<class R, const char* Group, const char* Hier>
struct v_ip_refu2 {
    static constexpr bool touches_receiver = true;
    static constexpr const char* body = "use";
    static constexpr const char* name = "om ref";
    static constexpr const char* group = Group;
    static constexpr const char* hier = Hier;
    static constexpr int arity = 2;

    using args = ipair_args<R>;

    static auto prepare(env& e, std::size_t i, std::size_t j) -> args {
        auto& p = ip_traits<R>::pop(e).ptrs;
        return {p[i], p[j]};
    }

    static auto call(args p, int x) -> int {
        return icollideu_ref<R>::fn(*p.a, *p.b, x);
    }

    static void regions(args p, std::vector<region>& out) {
        object_regions(p.a, out);
        object_regions(p.b, out);
        out.push_back({&icollideu_ref<R>::fn, sizeof(icollideu_ref<R>)});
        inplace_registry_regions<R>(out);
    }
};

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

// The rdtsc brackets live *inside* this function, so the call and the prologue
// are not timed, while the arguments still arrive through the ABI and cannot be
// constant-folded or devirtualized by the optimizer.
template<class V>
__attribute__((noinline)) auto
timed_call(typename V::args a, int x, std::uint64_t& cycles) -> int {
    auto t0 = tsc_start();
    auto r = V::call(a, x);
    auto t1 = tsc_stop();
    cycles = elapsed(t0, t1);
    return r;
}

struct row {
    std::string name;
    std::string group;
    std::string hier;
    int arity;
    bool touches_receiver;
    std::string body;
    cache_mode mode;
    stats st;
};

template<class V>
auto run(env& e, const config& cfg, cache_mode mode) -> row {
    std::vector<std::uint64_t> samples;
    samples.reserve(cfg.reps);

    std::vector<region> regs;

    // Every variant replays the identical sequence of receivers. `disp` is a
    // difference of two measurements, and in the cold modes both are dominated
    // by DRAM latency that depends on which objects were drawn; drawing from a
    // shared stream would leave each variant on different objects and make that
    // term fail to cancel. Paired draws make the subtraction meaningful.
    e.rng.seed(cfg.seed);

    for (std::size_t rep = 0; rep != cfg.reps; ++rep) {
        auto i = e.draw();
        auto j = e.draw();
        auto a = V::prepare(e, i, j);
        auto x = static_cast<int>(rep & 0xff);

        // Warm everything, so `warm` mode measures a hot cache and the cold
        // modes measure eviction rather than first-touch page faults.
        std::uint64_t discard;
        consume(timed_call<V>(a, x, discard));

        regs.clear();
        V::regions(a, regs);
        e.scrub.apply(mode, regs.data(), regs.size());

        std::uint64_t cycles;
        auto r = timed_call<V>(a, x, cycles);
        consume(r);

        samples.push_back(cycles);
    }

    return {V::name, V::group, V::hier,    V::arity, V::touches_receiver,
            V::body, mode,     summarize(std::move(samples))};
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

// Every dispatch path must reach the same overrider. A registry wired to the
// wrong method, or an overrider that failed to register, would otherwise
// produce a plausible but meaningless table.
auto verify(env& e) -> bool {
    bool ok = true;
    const int x = 7;

    for (std::size_t i = 0; i < e.ptrs.size(); i += 97) {
        for (std::size_t j = 0; j < e.ptrs.size(); j += 89) {
            const auto* a = e.ptrs[i];
            const auto* b = e.ptrs[j];

            // Independent oracles, from the class contracts alone. The old
            // expectation for arity 2 was collide_ref<vector_registry> -- one
            // of the paths under test -- so every registry being wrong the
            // same way (an unregistered overrider pack, say) passed unseen
            // (review finding). vf(x) contracts to x + tag; the diagonal
            // overrider fires iff both receivers are the same leaf; dd
            // dispatches on `a`, then dd_with on `b`, so it returns x + b.tag.
            auto want1 = x + a->tag;
            auto want2 = (a->tag == b->tag) ? x + a->tag : x;

            // Use-world contracts: every body reads every receiver.
            auto wantu2 = (a->tag == b->tag) ? x + a->tag + b->tag
                                             : x - a->tag - b->tag;

            struct check {
                const char* what;
                int got;
                int want;
            };

            const check checks[] = {
                {"vf yardstick", a->vf(x), want1},
                {"dd yardstick", a->dd(*b, x), x + b->tag},
                {"vfu yardstick", a->vfu(x), want1},
                {"ddu yardstick", a->ddu(*b, x), x + a->tag + b->tag},
                {"vec refu1", pokeu_ref<vector_registry>::fn(*a, x), want1},
                {"vec vpu1", pokeu_vp<vector_registry>::fn(e.vp_vec[i], x),
                 want1},
                {"map refu1", pokeu_ref<map_registry>::fn(*a, x), want1},
                {"flat refu1", pokeu_ref<flat_registry>::fn(*a, x), want1},
                {"ind refu1", pokeu_ref<indirect_registry>::fn(*a, x), want1},
                {"ind vpu1", pokeu_vp<indirect_registry>::fn(e.vp_ind[i], x),
                 want1},
                {"vec refu2", collideu_ref<vector_registry>::fn(*a, *b, x),
                 wantu2},
                {"vec vpu2",
                 collideu_vp<vector_registry>::fn(e.vp_vec[i], e.vp_vec[j], x),
                 wantu2},
                {"map refu2", collideu_ref<map_registry>::fn(*a, *b, x),
                 wantu2},
                {"flat refu2", collideu_ref<flat_registry>::fn(*a, *b, x),
                 wantu2},
                {"ind refu2", collideu_ref<indirect_registry>::fn(*a, *b, x),
                 wantu2},
                {"ind vpu2",
                 collideu_vp<indirect_registry>::fn(
                     e.vp_ind[i], e.vp_ind[j], x),
                 wantu2},
                {"vec ref2", collide_ref<vector_registry>::fn(*a, *b, x),
                 want2},
                {"nvf", a->nvf(x), want1},
                {"vec ref1", poke_ref<vector_registry>::fn(*a, x), want1},
                {"map ref1", poke_ref<map_registry>::fn(*a, x), want1},
                {"flat ref1", poke_ref<flat_registry>::fn(*a, x), want1},
                {"vec vp1", poke_vp<vector_registry>::fn(e.vp_vec[i], x),
                 want1},
                {"map vp1", poke_vp<map_registry>::fn(e.vp_map[i], x), want1},
                {"flat vp1", poke_vp<flat_registry>::fn(e.vp_flat[i], x),
                 want1},
                {"ind ref1", poke_ref<indirect_registry>::fn(*a, x), want1},
                {"ind vp1", poke_vp<indirect_registry>::fn(e.vp_ind[i], x),
                 want1},
                {"ind ref2",
                 collide_ref<indirect_registry>::fn(*a, *b, x), want2},
                {"ind vp2",
                 collide_vp<indirect_registry>::fn(e.vp_ind[i], e.vp_ind[j], x),
                 want2},
                {"map ref2", collide_ref<map_registry>::fn(*a, *b, x), want2},
                {"flat ref2", collide_ref<flat_registry>::fn(*a, *b, x),
                 want2},
                {"vec vp2",
                 collide_vp<vector_registry>::fn(e.vp_vec[i], e.vp_vec[j], x),
                 want2},
                {"map vp2",
                 collide_vp<map_registry>::fn(e.vp_map[i], e.vp_map[j], x),
                 want2},
                {"flat vp2",
                 collide_vp<flat_registry>::fn(e.vp_flat[i], e.vp_flat[j], x),
                 want2},
            };

            for (const auto& c : checks) {
                if (c.got != c.want) {
                    std::printf(
                        "MISMATCH %s at (%zu,%zu): got %d, want %d\n", c.what,
                        i, j, c.got, c.want);
                    ok = false;
                }
            }
        }
    }

    // The inplace hierarchies have their own objects, so they need their own
    // expectation: the virtual function on the same receiver.
    auto check_inplace = [&](auto& pop, auto ref1, auto ref2,
                             const char* what) {
        for (std::size_t i = 0; i < pop.ptrs.size(); i += 97) {
            for (std::size_t j = 0; j < pop.ptrs.size(); j += 89) {
                const auto* a = pop.ptrs[i];
                const auto* b = pop.ptrs[j];

                auto got1 = ref1(*a, x);
                auto want1 = x + a->tag;

                if (a->vf(x) != want1) {
                    std::printf(
                        "MISMATCH %s vf at %zu: got %d, want %d\n", what, i,
                        a->vf(x), want1);
                    ok = false;
                }

                if (got1 != want1) {
                    std::printf(
                        "MISMATCH %s ref1 at %zu: got %d, want %d\n", what, i,
                        got1, want1);
                    ok = false;
                }

                // Same rule as the main hierarchy: the diagonal overrider fires
                // when both receivers are the same leaf, otherwise the
                // (Base, Base) catch-all returns x unchanged.
                auto want2 = (a->tag == b->tag) ? x + a->tag : x;
                auto got2 = ref2(*a, *b, x);

                if (got2 != want2) {
                    std::printf(
                        "MISMATCH %s ref2 at (%zu,%zu): got %d, want %d\n",
                        what, i, j, got2, want2);
                    ok = false;
                }
            }
        }
    };

    auto check_inplace_use = [&](auto& pop, auto refu1, auto refu2,
                                 const char* what) {
        for (std::size_t i = 0; i < pop.ptrs.size(); i += 97) {
            for (std::size_t j = 0; j < pop.ptrs.size(); j += 89) {
                const auto* a = pop.ptrs[i];
                const auto* b = pop.ptrs[j];

                auto got1 = refu1(*a, x);

                if (got1 != x + a->tag) {
                    std::printf(
                        "MISMATCH %s refu1 at %zu: got %d, want %d\n", what,
                        i, got1, x + a->tag);
                    ok = false;
                }

                auto wantu2 = (a->tag == b->tag) ? x + a->tag + b->tag
                                                 : x - a->tag - b->tag;
                auto got2 = refu2(*a, *b, x);

                if (got2 != wantu2) {
                    std::printf(
                        "MISMATCH %s refu2 at (%zu,%zu): got %d, want %d\n",
                        what, i, j, got2, wantu2);
                    ok = false;
                }
            }
        }
    };

    check_inplace_use(
        e.ip_direct,
        [](const IBase<inplace_registry>& o, int v) {
            return ipokeu_ref<inplace_registry>::fn(o, v);
        },
        [](const IBase<inplace_registry>& o1,
           const IBase<inplace_registry>& o2, int v) {
            return icollideu_ref<inplace_registry>::fn(o1, o2, v);
        },
        "inplace-use");

    check_inplace_use(
        e.ip_indirect,
        [](const IBase<inplace_indirect_registry>& o, int v) {
            return ipokeu_ref<inplace_indirect_registry>::fn(o, v);
        },
        [](const IBase<inplace_indirect_registry>& o1,
           const IBase<inplace_indirect_registry>& o2, int v) {
            return icollideu_ref<inplace_indirect_registry>::fn(o1, o2, v);
        },
        "inplace_ind-use");

    check_inplace(
        e.ip_direct,
        [](const IBase<inplace_registry>& o, int v) {
            return ipoke_ref<inplace_registry>::fn(o, v);
        },
        [](const IBase<inplace_registry>& o1,
           const IBase<inplace_registry>& o2, int v) {
            return icollide_ref<inplace_registry>::fn(o1, o2, v);
        },
        "inplace");

    check_inplace(
        e.ip_indirect,
        [](const IBase<inplace_indirect_registry>& o, int v) {
            return ipoke_ref<inplace_indirect_registry>::fn(o, v);
        },
        [](const IBase<inplace_indirect_registry>& o1,
           const IBase<inplace_indirect_registry>& o2, int v) {
            return icollide_ref<inplace_indirect_registry>::fn(o1, o2, v);
        },
        "inplace_ind");

    return ok;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void report(
    const std::vector<row>& rows, const config& cfg, double tsc_hz,
    const env& e, const stats& floor_st) {
    // Baselines: the `ovh` row for each mode.
    auto baseline = [&](cache_mode m) -> const stats* {
        for (const auto& r : rows) {
            if (r.group == "baseline" && r.mode == m) {
                return &r.st;
            }
        }
        return nullptr;
    };

    // Uncertainty on a difference of two independent means.
    auto net_of = [&](const row& r, double& se) -> double {
        const auto* b = baseline(r.mode);

        if (b == nullptr) {
            se = r.st.se;
            return r.st.mean;
        }

        se = std::sqrt(r.st.se * r.st.se + b->se * b->se);
        return r.st.mean - b->mean;
    };

    // The object-touch baseline of the same arity: `nvf` / `nvf+nvf`, which
    // load the receivers but dispatch on nothing.
    auto obj_baseline = [&](cache_mode m, int arity,
                            const std::string& hier) -> const stats* {
        for (const auto& r : rows) {
            if (r.group == "baseline" && r.mode == m && r.arity == arity &&
                r.hier == hier) {
                return &r.st;
            }
        }
        return nullptr;
    };

    // Dispatch proper: the cost above reaching the same receivers. This is the
    // apparatus-and-object-compensated number; `net` still carries the cold
    // miss on the receiver, which in the cold modes dominates it.
    auto disp_of = [&](const row& r, double& se) -> double {
        // The om vptr timed regions never dereference the receiver: the
        // dispatch reads the virtual_ptr (already in the arguments), the
        // method slot and the table, and the overriders ignore the object.
        // Subtracting the nvf baseline would credit those rows with a
        // receiver miss they never paid -- cold, that fabricated ~270 cycles
        // and made "dispatch alone" read ~0.96x when the honest figure is the
        // row's whole net (review finding). For them, disp IS net.
        if (!r.touches_receiver) {
            return net_of(r, se);
        }

        const auto* b = obj_baseline(r.mode, r.arity, r.hier);

        if (b == nullptr || r.arity == 0) {
            se = 0;
            return 0;
        }

        se = std::sqrt(r.st.se * r.st.se + b->se * b->se);
        return r.st.mean - b->mean;
    };

    // Ratio to the virtual function yardstick of the same arity, measured in
    // the same run and the same cache mode, on the dispatch-proper figure.
    // Cold *absolutes* swing by ~25% from run to run on this machine; the ratio
    // is stable to a few percent, so it, not the cycle count, is what to quote.
    auto ratio_of = [&](const row& r, bool use_disp) -> double {
        if (r.arity == 0 || r.group == "baseline") {
            return 0;
        }

        for (const auto& y : rows) {
            if (y.group == "yardstick" && y.mode == r.mode &&
                y.arity == r.arity && y.hier == r.hier &&
                y.body == r.body) {
                double se;
                auto base = use_disp ? disp_of(y, se) : net_of(y, se);
                auto val = use_disp ? disp_of(r, se) : net_of(r, se);
                return base > 0 ? val / base : 0;
            }
        }

        return 0;
    };

    if (cfg.csv) {
        std::printf(
            "compiler,bits,mode,hier,body,group,dispatch,arity,min,median,p90,mean,"
            "stddev,se,net,"
            "net_se,disp,disp_se,disp_ns,x_net,x_disp\n");

        for (const auto& r : rows) {
            double se;
            auto net = net_of(r, se);
            double dse;
            auto disp = disp_of(r, dse);
            std::printf(
                "%s,%zu,%s,%s,%s,%s,%s,%d,%llu,%llu,%llu,%.2f,%.1f,%.2f,%.2f,%.2f,"
                "%.2f,%.2f,%.3f,%.3f,%.3f\n",
                compiler_id().c_str(), bitness,
                cache_mode_name(r.mode), r.hier.c_str(), r.body.c_str(),
                r.group.c_str(), r.name.c_str(), r.arity,
                (unsigned long long)r.st.min,
                (unsigned long long)r.st.median, (unsigned long long)r.st.p90,
                r.st.mean, r.st.stddev, r.st.se, net, se, disp, dse,
                disp * 1e9 / tsc_hz, ratio_of(r, false), ratio_of(r, true));
        }

        return;
    }

    std::printf(
        "\nBoost.OpenMethod single-dispatch latency (rdtsc reference "
        "cycles)\n");
    std::printf(
        "  build: %s, %zu-bit; sizeof void* %zu, word %zu, virtual_ptr %zu\n",
        compiler_id().c_str(), bitness, sizeof(void*),
        sizeof(boost::openmethod::detail::word),
        sizeof(om::virtual_ptr<const Base, vector_registry>));
    std::printf(
        "  %zu classes, %zu objects, %zu reps, TSC %.3f GHz, sweep %zu MiB\n",
        num_classes, e.objects.size(), cfg.reps, tsc_hz / 1e9,
        e.scrub.sweep_bytes() / (1024 * 1024));
    std::printf(
        "  empty timed region (noise floor): mean %.1f, median %llu cycles\n",
        floor_st.mean, (unsigned long long)floor_st.median);
    std::printf(
        "  mean trimmed of top 5%%; net = mean - ovh; disp = mean - nvf of "
        "same arity\n");
    std::printf(
        "  net still contains the cold miss on the receiver; disp is dispatch "
        "alone\n");
    std::printf(
        "  read the mean, not the median: a single sample is quantized by the "
        "floor\n\n");

    std::printf(
        "%-8s %-11s %-12s %-8s %5s %9s %7s %9s %8s %8s\n", "mode", "hierarchy",
        "group", "dispatch", "arity", "mean", "+/-", "disp", "x net",
        "x disp");
    std::printf("%s\n", std::string(100, '-').c_str());

    cache_mode last = rows.empty() ? cache_mode::warm : rows.front().mode;

    for (const auto& r : rows) {
        if (r.mode != last) {
            std::printf("\n");
            last = r.mode;
        }

        double se;
        (void)net_of(r, se);

        char rbuf[16], rdbuf[16];
        auto fmt_ratio = [](char* buf, std::size_t n, double v) {
            if (v > 0) {
                std::snprintf(buf, n, "%.2fx", v);
            } else {
                std::snprintf(buf, n, "%s", "-");
            }
        };

        fmt_ratio(rbuf, sizeof(rbuf), ratio_of(r, false));
        fmt_ratio(rdbuf, sizeof(rdbuf), ratio_of(r, true));

        double dse;
        auto disp = disp_of(r, dse);
        char dbuf[16];

        if (r.arity == 0 || r.group == "baseline") {
            std::snprintf(dbuf, sizeof(dbuf), "%s", "-");
        } else {
            std::snprintf(dbuf, sizeof(dbuf), "%.1f", disp);
        }

        std::printf(
            "%-8s %-11s %-12s %-8s %5d %9.1f %7.2f %9s %8s %8s\n",
            cache_mode_name(r.mode), r.hier.c_str(), r.group.c_str(),
            r.name.c_str(), r.arity, r.st.mean, se, dbuf, rbuf, rdbuf);
    }

    std::printf("\n");
}

// ---------------------------------------------------------------------------

extern const char group_vector[] = "vptr_vector";
extern const char group_map[] = "vptr_map";
extern const char group_flat[] = "flat_map";
extern const char group_indirect[] = "indirect";
extern const char group_inplace[] = "inplace";
extern const char group_inplace_ind[] = "inplace_ind";
extern const char hier_main[] = "main";
extern const char hier_inplace[] = "inplace";
extern const char hier_inplace_ind[] = "inplace_ind";

auto parse(int argc, char** argv, config& cfg) -> bool {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::printf("missing value for %s\n", arg.c_str());
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--reps") {
            const char* v = value();
            if (v == nullptr) {
                return false;
            }
            cfg.reps = std::strtoull(v, nullptr, 10);
        } else if (arg == "--objects") {
            const char* v = value();
            if (v == nullptr) {
                return false;
            }
            cfg.objects = std::strtoull(v, nullptr, 10);
        } else if (arg == "--sweep-mb") {
            const char* v = value();
            if (v == nullptr) {
                return false;
            }
            cfg.sweep_mb = std::strtoull(v, nullptr, 10);
        } else if (arg == "--cpu") {
            const char* v = value();
            if (v == nullptr) {
                return false;
            }
            cfg.cpu = std::atoi(v);
        } else if (arg == "--seed") {
            const char* v = value();
            if (v == nullptr) {
                return false;
            }
            cfg.seed = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (arg == "--csv") {
            cfg.csv = true;
        } else if (arg == "--verify") {
            cfg.verify_only = true;
        } else if (arg == "--mode") {
            const char* v = value();
            if (v == nullptr) {
                return false;
            }
            std::string m = v;
            cfg.warm = cfg.clflush = cfg.sweep = false;
            if (m == "warm") {
                cfg.warm = true;
            } else if (m == "clflush") {
                cfg.clflush = true;
            } else if (m == "sweep") {
                cfg.sweep = true;
            } else if (m == "all") {
                cfg.warm = cfg.clflush = cfg.sweep = true;
            } else {
                std::printf("unknown mode: %s\n", m.c_str());
                return false;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "usage: benchmark [--reps N] [--objects N] [--sweep-mb N]\n"
                "                 [--cpu N] [--seed N] [--mode "
                "warm|clflush|sweep|all]\n"
                "                 [--csv] [--verify]\n");
            return false;
        } else {
            std::printf("unknown option: %s\n", arg.c_str());
            return false;
        }
    }

    if (cfg.objects == 0 || cfg.reps == 0) {
        std::printf("--objects and --reps must be at least 1\n");
        return false;
    }

    if (cfg.sweep_mb == 0 ||
        cfg.sweep_mb > std::numeric_limits<std::size_t>::max() / (1024 * 1024)) {
        std::printf("--sweep-mb out of range for this build's size_t\n");
        return false;
    }

    if (cfg.cpu < 0) {
        std::printf("--cpu must be non-negative\n");
        return false;
    }

    return true;
}

auto main_impl(int argc, char** argv) -> int {
    config cfg;

    if (!parse(argc, argv, cfg)) {
        return 2;
    }

    if (!pin_to_cpu(cfg.cpu)) {
        std::fprintf(
            stderr, "warning: could not pin to cpu %d\n", cfg.cpu);
    }

    om::initialize<vector_registry>();
    om::initialize<map_registry>();
    om::initialize<flat_registry>();
    om::initialize<indirect_registry>();
    om::initialize<inplace_registry>();
    om::initialize<inplace_indirect_registry>();

    env e(cfg);

    if (!verify(e)) {
        std::fprintf(stderr, "verification FAILED\n");
        return 1;
    }

    if (cfg.verify_only) {
        std::printf("verification passed\n");
        return 0;
    }

    auto tsc_hz = calibrate_tsc_hz();
    auto floor_st = measure_tsc_floor();

    std::vector<cache_mode> modes;

    if (cfg.warm) {
        modes.push_back(cache_mode::warm);
    }

    if (cfg.clflush) {
        modes.push_back(cache_mode::clflush);
    }

    if (cfg.sweep) {
        modes.push_back(cache_mode::sweep);
    }

    std::vector<row> rows;

    for (auto mode : modes) {
        rows.push_back(run<v_ovh>(e, cfg, mode));
        rows.push_back(run<v_nvf1>(e, cfg, mode));
        rows.push_back(run<v_nvf2>(e, cfg, mode));
        rows.push_back(run<v_vf1>(e, cfg, mode));
        rows.push_back(run<v_vf2>(e, cfg, mode));

// virtual_ptr first throughout: it is the recommended way to use the library.
#define OMB_RUN(R, G)                                                          \
    rows.push_back(run<v_om_vp1<R, G>>(e, cfg, mode));                         \
    rows.push_back(run<v_om_ref1<R, G>>(e, cfg, mode));                        \
    rows.push_back(run<v_om_vp2<R, G>>(e, cfg, mode));                         \
    rows.push_back(run<v_om_ref2<R, G>>(e, cfg, mode))

        OMB_RUN(vector_registry, group_vector);
        OMB_RUN(map_registry, group_map);
        OMB_RUN(flat_registry, group_flat);
        OMB_RUN(indirect_registry, group_indirect);

#undef OMB_RUN

        // inplace: own baselines and yardsticks, reference form only.
#define OMB_RUN_INPLACE(R, G, H)                                               \
    rows.push_back(run<v_ip_nvf1<R, H>>(e, cfg, mode));                        \
    rows.push_back(run<v_ip_nvf2<R, H>>(e, cfg, mode));                        \
    rows.push_back(run<v_ip_vf1<R, H>>(e, cfg, mode));                         \
    rows.push_back(run<v_ip_vf2<R, H>>(e, cfg, mode));                         \
    rows.push_back(run<v_ip_ref1<R, G, H>>(e, cfg, mode));                     \
    rows.push_back(run<v_ip_ref2<R, G, H>>(e, cfg, mode))

        OMB_RUN_INPLACE(inplace_registry, group_inplace, hier_inplace);
        OMB_RUN_INPLACE(
            inplace_indirect_registry, group_inplace_ind, hier_inplace_ind);

#undef OMB_RUN_INPLACE

        // The use world: bodies read the receiver(s).
        rows.push_back(run<v_vfu1>(e, cfg, mode));
        rows.push_back(run<v_vfu2>(e, cfg, mode));

#define OMB_RUN_USE(R, G)                                                      \
    rows.push_back(run<v_om_vpu1<R, G>>(e, cfg, mode));                        \
    rows.push_back(run<v_om_refu1<R, G>>(e, cfg, mode));                       \
    rows.push_back(run<v_om_vpu2<R, G>>(e, cfg, mode));                        \
    rows.push_back(run<v_om_refu2<R, G>>(e, cfg, mode))

        OMB_RUN_USE(vector_registry, group_vector);
        OMB_RUN_USE(map_registry, group_map);
        OMB_RUN_USE(flat_registry, group_flat);
        OMB_RUN_USE(indirect_registry, group_indirect);

#undef OMB_RUN_USE

        rows.push_back(run<v_ip_vfu1<inplace_registry, hier_inplace>>(
            e, cfg, mode));
        rows.push_back(run<v_ip_vfu2<inplace_registry, hier_inplace>>(
            e, cfg, mode));
        rows.push_back(
            run<v_ip_vfu1<inplace_indirect_registry, hier_inplace_ind>>(
                e, cfg, mode));
        rows.push_back(
            run<v_ip_vfu2<inplace_indirect_registry, hier_inplace_ind>>(
                e, cfg, mode));
        rows.push_back(
            run<v_ip_refu1<inplace_registry, group_inplace, hier_inplace>>(
                e, cfg, mode));
        rows.push_back(
            run<v_ip_refu2<inplace_registry, group_inplace, hier_inplace>>(
                e, cfg, mode));
        rows.push_back(run<v_ip_refu1<
                           inplace_indirect_registry, group_inplace_ind,
                           hier_inplace_ind>>(e, cfg, mode));
        rows.push_back(run<v_ip_refu2<
                           inplace_indirect_registry, group_inplace_ind,
                           hier_inplace_ind>>(e, cfg, mode));
    }

    report(rows, cfg, tsc_hz, e, floor_st);

    return 0;
}

} // namespace omb

auto main(int argc, char** argv) -> int {
    return omb::main_impl(argc, argv);
}
