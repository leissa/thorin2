#include <iostream>

#include "thorin/axiom.h"
#include "thorin/world.h"

#include "dialects/autodiff/autodiff.h"
#include "dialects/autodiff/auxiliary/autodiff_aux.h"
#include "dialects/core/core.h"

namespace thorin::autodiff {

const Def* normalize_autodiff(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto& world = type->world();
    // auto [mat, index, val] = arg->projs<3>();

    // do nothing (everything handled in the rewrite pass)
    // TODO: maybe directly handle operations

    return world.raw_app(callee, arg, dbg);
}

const Def* normalize_autodiff_type(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto& world = type->world();
    // return arg;
    // return world.lit_int_width(32,42);
    auto ad_ty = autodiff_type_fun(arg);
    if (ad_ty) return ad_ty;
    return world.raw_app(callee, arg, dbg);
}

const Def* normalize_tangent_type(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto& world = type->world();
    return tangent_type_fun(arg);
}

// TODO: zero of type Nat, Real, Int -> 0
const Def* normalize_zero(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto& world = type->world();

    // TODO: move to pass
    // do not normalize complex types (arrays, tuples, etc) here
    // as add would no longer be able to shortcut them

    auto T = arg;
    // auto zero = zero_def(T);
    // if(zero)
    //     return zero;

    return world.raw_app(callee, arg, dbg);
}

const Def* normalize_add(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto& world = type->world();
    // auto [mat, index, val] = arg->projs<3>();

    // TODO: add tuple -> tuple of adds
    // TODO: add zero -> other
    // TODO: unify mapping over structure with other aspects like zero

    auto T      = callee->as<App>()->arg();
    auto [a, b] = arg->projs<2>();

    world.DLOG("add {} {} {}", T, a, b);

    if (match<zero>(a)) {
        world.DLOG("0+b");
        return b;
    }
    if (match<zero>(b)) {
        world.DLOG("0+a");
        return a;
    }
    // TODO: value vs type level match (what is easier)
    // value level match harder as a tuple might in reality be a var or extract
    if (auto sig = T->isa<Sigma>()) {
        world.DLOG("add tuple");
        auto p = sig->num_ops(); // TODO: or num_projs
        DefArray ops(p, [&](size_t i) {
            return world.app(world.app(world.ax<add>(), sig->op(i)), {a->proj(i), b->proj(i)});
        });
        return world.tuple(ops);
    } else if (auto arr = T->isa<Arr>()) {
        // TODO: is this working for non-lit (non-tuple)?
        //   or do we need a loop
        world.DLOG("add arrays {} {} {}", T, a, b);
        auto pack      = world.nom_pack(T);
        auto body_type = arr->body();
        world.DLOG("body type {}", body_type);
        pack->set(world.app(world.app(world.ax<add>(), body_type),
                            {world.extract(a, pack->var()), world.extract(b, pack->var())}));
        world.DLOG("pack {}", pack);
        return pack;
        // assert(0);
    } else if (auto idx = T->isa<Idx>()) {
        world.DLOG("add int");
        auto width = as_lit(world.iinfer(a));
        world.DLOG("width {}", width);
        // auto int_add = core::op(thorin::core::wrap::add, 0, a,b);
        auto int_add =
            world.app(world.app(world.ax(core::wrap::add), {world.lit_nat_0(), world.lit_nat(width)}), {a, b});
        world.DLOG("int add {} : {}", int_add, world.iinfer(int_add));
        return int_add;
        // return
        // world.app(world.app(
        //     world.ax(core::)
        // ))
    } else if (auto app = T->isa<App>()) {
        auto callee = app->callee();
        // if (callee->isa<Idx>()) {
        //     world.DLOG("add int");
        //     auto width = as_lit(world.iinfer(a));
        //     world.DLOG("width {}", width);
        //     // auto int_add = core::op(thorin::core::wrap::add, 0, a,b);
        //     auto int_add =
        //         world.app(world.app(world.ax(core::wrap::add), {world.lit_nat_0(), world.lit_nat(width)}), {a, b});
        //     world.DLOG("int add {} : {}", int_add, world.iinfer(int_add));
        //     return int_add;
        //     // return
        //     // world.app(world.app(
        //     //     world.ax(core::)
        //     // ))
        // }
        assert(0 && "not handled");
    }
    // TODO: mem stays here (only resolved after direct simplification)
    // assert(0);

    return world.raw_app(callee, arg, dbg);
}

const Def* normalize_sum(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto& world = type->world();
    // sum (n,T) arr

    auto [count, T] = callee->as<App>()->args<2>();

    // world.DLOG("sum {}:{} {}:{}", count, count->type(), T, T->type());
    if (auto lit = count->isa<Lit>()) {
        // auto val = lit
        // auto val=lit->as<nat_t>();
        auto val = lit->get<nat_t>();
        world.DLOG("val: {}", val);
        // R not necessary if sum is evaluated before zero (needs zero as pass)
        // R if(val == 1)
        // R     return arg;
        DefArray args = arg->projs(val);
        auto sum      = world.app(world.ax<zero>(), T);
        // would also be handled by add zero
        if (val >= 1) { sum = args[0]; }
        for (auto i = 1; i < val; ++i) { sum = world.app(world.app(world.ax<add>(), T), {sum, args[i]}); }
        return sum;
    }
    assert(0);

    return world.raw_app(callee, arg, dbg);
}

THORIN_autodiff_NORMALIZER_IMPL

} // namespace thorin::autodiff
