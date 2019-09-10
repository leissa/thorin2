#include "thorin/def.h"
#include "thorin/fold.h"
#include "thorin/util.h"
#include "thorin/world.h"

namespace thorin {

using namespace thorin::fold;

/*
 * helpers
 */

static bool is_allset(const Def* def) {
    if (auto lit = isa_lit<u64>(def)) {
        if (auto width = isa_lit<u64>(as<Tag::Int>(def->type())->arg()))
            return (*lit >> (64_u64 - *width) == u64(-1) >> (64_u64 - *width));
    }
    return false;
}

static bool is_not(const Def* def) {
    if (auto ixor = isa<Tag::IOp, IOp::ixor>(def)) {
        auto [x, y] = ixor->split<2>();
        if (is_allset(x)) return true;
    }
    return false;
}

/*
 * fold
 */

template<template<nat_t> class F>
static const Def* fold_i(const Def* type, const Def* callee, const Def* a, const Def* b, const Def* dbg) {
    auto& world = callee->world();
    if (a->isa<Bot>() || b->isa<Bot>()) return world.bot(type, dbg);

    auto la = a->isa<Lit>(), lb = b->isa<Lit>();
    if (la && lb) {
        auto w = as_lit<nat_t>(as<Tag::Int>(type)->arg());
        Res res;
        switch (w) {
            case  1: res = F< 1>::run(la->get(), lb->get()); break;
            case  8: res = F< 8>::run(la->get(), lb->get()); break;
            case 16: res = F<16>::run(la->get(), lb->get()); break;
            case 32: res = F<32>::run(la->get(), lb->get()); break;
            case 64: res = F<64>::run(la->get(), lb->get()); break;
            default: THORIN_UNREACHABLE;
        }

        if (res) return world.lit(type, *res, dbg);
        return world.bot(type, dbg);
    }

    return nullptr;
}

template<template<nat_t> class F>
static const Def* fold_w(const Def* type, const Def* callee, const Def* a, const Def* b, const Def* dbg) {
    auto& world = callee->world();
    if (a->isa<Bot>() || b->isa<Bot>()) return world.bot(type, dbg);

    auto la = a->isa<Lit>(), lb = b->isa<Lit>();
    if (la && lb) {
        auto [f, w] = callee->as<App>()->split<2>(isa_lit<nat_t>);
        if (f && w) {
            bool nsw = *f & WMode::nsw;
            bool nuw = *f & WMode::nuw;
            Res res;
            switch (*w) {
                case  8: res = F< 8>::run(la->get(), lb->get(), nsw, nuw); break;
                case 16: res = F<16>::run(la->get(), lb->get(), nsw, nuw); break;
                case 32: res = F<32>::run(la->get(), lb->get(), nsw, nuw); break;
                case 64: res = F<64>::run(la->get(), lb->get(), nsw, nuw); break;
                default: THORIN_UNREACHABLE;
            }

            if (res) return world.lit(type, *res, dbg);
            return world.bot(type, dbg);
        }
    }

    return nullptr;
}

template<template<nat_t> class F>
static const Def* fold_z(const Def* type, const Def* callee, const Def* m, const Def* a, const Def* b, const Def* dbg) {
    auto& world = callee->world();
    if (a->isa<Bot>() || b->isa<Bot>()) return world.bot(type, dbg);

    auto la = a->isa<Lit>(), lb = b->isa<Lit>();
    if (la && lb) {
        auto w = as_lit<nat_t>(as<Tag::Int>(type)->arg());
        Res res;
        switch (w) {
            case  8: res = F< 8>::run(la->get(), lb->get()); break;
            case 16: res = F<16>::run(la->get(), lb->get()); break;
            case 32: res = F<32>::run(la->get(), lb->get()); break;
            case 64: res = F<64>::run(la->get(), lb->get()); break;
            default: THORIN_UNREACHABLE;
        }

        if (res) return world.tuple({m, world.lit(type, *res, dbg)}, dbg);
        return world.tuple({m, world.bot(type, dbg)}, dbg);
    }

    return nullptr;
}

template<template<nat_t> class F>
static const Def* fold_r(const Def* type, const Def* callee, const Def* a, const Def* b, const Def* dbg) {
    auto& world = callee->world();
    if (a->isa<Bot>() || b->isa<Bot>()) return world.bot(type, dbg);

    auto la = a->isa<Lit>(), lb = b->isa<Lit>();
    if (la && lb) {
        auto w = as_lit<nat_t>(as<Tag::Real>(type)->arg());
        Res res;
        switch (w) {
            case 16: res = F<16>::run(la->get(), lb->get()); break;
            case 32: res = F<32>::run(la->get(), lb->get()); break;
            case 64: res = F<64>::run(la->get(), lb->get()); break;
            default: THORIN_UNREACHABLE;
        }

        if (res) return world.lit(type, *res, dbg);
        return world.bot(type, dbg);
    }

    return nullptr;
}

#define TABLE(m) m( 1,  1) m( 1,  8) m( 1, 16) m( 1, 32) m( 1, 64) \
                 m( 8,  1) m( 8,  8) m( 8, 16) m( 8, 32) m( 8, 64) \
                 m(16,  1) m(16,  8) m(16, 16) m(16, 32) m(16, 64) \
                 m(32,  1) m(32,  8) m(32, 16) m(32, 32) m(32, 64) \
                 m(64,  1) m(64,  8) m(64, 16) m(64, 32) m(64, 64)

template<nat_t min_sw, nat_t min_dw, template<nat_t, nat_t> class F>
static const Def* fold_Conv(const Def* dst_type, const Def* callee, const Def* src, const Def* dbg) {
    auto& world = callee->world();
    if (src->isa<Bot>()) return world.bot(dst_type, dbg);

    auto [sw, dw] = callee->as<App>()->decurry()->split<2>();
    if (sw == dw && dst_type == src->type()) return src;

    auto lit_src = src->isa<Lit>();
    auto lit_sw = isa_lit<nat_t>(sw), lit_dw = isa_lit<nat_t>(dw);
    if (lit_src && lit_sw && lit_dw) {
        Res res;
#define CODE(sw, dw)                                                                          \
        else if (*lit_sw == sw && *lit_dw == dw) {                                            \
            if constexpr (sw >= min_sw && dw >= min_dw) res = F<sw, dw>::run(lit_src->get()); \
        }
        if (false) {} TABLE(CODE)
#undef CODE
        if (res) return world.lit(dst_type, *res, dbg);
        return world.bot(dst_type, dbg);
    }

    return nullptr;
}

/*
 * normalize
 */

template<IOp op>
const Def* normalize_IOp(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto [a, b] = split<2>(arg);
    if (auto result = fold_i<FoldIOp<op>::template Fold>(type, callee, a, b, dbg)) return result;

    return nullptr;
}

template<WOp op>
const Def* normalize_WOp(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto [a, b] = split<2>(arg);
    if (auto result = fold_w<FoldWOp<op>::template Fold>(type, callee, a, b, dbg)) return result;

    return nullptr;
}

template<ZOp op>
const Def* normalize_ZOp(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto [m, a, b] = split<3>(arg);
    if (auto result = fold_z<FoldZOp<op>::template Fold>(type, callee, m, a, b, dbg)) return result;

    return nullptr;
}

template<ROp op>
const Def* normalize_ROp(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto [a, b] = split<2>(arg);
    if (auto result = fold_r<FoldROp<op>::template Fold>(type, callee, a, b, dbg)) return result;

    return nullptr;
}

template<ICmp op>
const Def* normalize_ICmp(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto& world = callee->world();
    auto [a, b] = split<2>(arg);

    if (auto result = fold_i<FoldICmp<op>::template Fold>(type, callee, a, b, dbg)) return result;
    if constexpr (op == ICmp::_f) return world.lit_false();
    if constexpr (op == ICmp::_t) return world.lit_true();

    return nullptr;
}

template<RCmp op>
const Def* normalize_RCmp(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto& world = callee->world();

    auto [a, b] = split<2>(arg);
    if (auto result = fold_r<FoldRCmp<op>::template Fold>(type, callee, a, b, dbg)) return result;
    if constexpr (op == RCmp::f) return world.lit_false();
    if constexpr (op == RCmp::t) return world.lit_true();

    return nullptr;
}

template<Conv op>
const Def* normalize_Conv(const Def* dst_type, const Def* callee, const Def* src, const Def* dbg) {
    auto& world = callee->world();

    static constexpr auto min_sw = op == Conv::r2s || op == Conv::r2u || op == Conv::r2r ? 16 : 1;
    static constexpr auto min_dw = op == Conv::s2r || op == Conv::u2r || op == Conv::r2r ? 16 : 1;
    if (auto result = fold_Conv<min_sw, min_dw, FoldConv<op>::template Fold>(dst_type, callee, src, dbg)) return result;

    auto [sw, dw] = callee->decurry()->split<2>(isa_lit<nat_t>);
    if (sw == dw && dst_type == src->type()) return src;

    if constexpr (op == Conv::s2s) {
        if (sw && dw && *sw < *dw) return world.op<Conv::u2u>(dst_type, src, dbg);
    }

    return nullptr;
}

const Def* normalize_select(const Def* type, const Def* callee, const Def* arg, const Def* dbg) {
    auto& world = callee->world();
    auto [cond, a, b] = split<3>(arg);

    if (cond->isa<Bot>()) return world.bot(type, dbg);
    if (auto lit = cond->isa<Lit>()) return lit->get<bool>() ? a : b;
    if (a == b) return a;
    if (is_not(cond)) std::swap(a, b);

    return world.raw_app(callee, {a, b}, dbg);
}

const Def* normalize_sizeof(const Def*, const Def* callee, const Def* type, const Def* dbg) {
    auto& world = callee->world();

    const Def* arg = nullptr;
    if (false) {}
    else if (auto int_ = isa<Tag::Int >(type)) arg = int_->arg();
    else if (auto real = isa<Tag::Real>(type)) arg = real->arg();

    if (auto width = isa_lit<nat_t>(arg)) return world.lit_nat(*width / 8, dbg);
    return nullptr;
}

/*
 * instantiate templates
 */

#define CODE(T, o) template const Def* normalize_ ## T<T::o>(const Def*, const Def*, const Def*, const Def*);
THORIN_W_OP (CODE)
THORIN_Z_OP (CODE)
THORIN_I_OP (CODE)
THORIN_R_OP (CODE)
THORIN_I_CMP(CODE)
THORIN_R_CMP(CODE)
THORIN_CONV (CODE)
#undef CODE

}
