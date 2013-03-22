#include "anydsl2/world.h"

#include <cmath>
#include <algorithm>
#include <queue>
#include <iostream>
#include <boost/unordered_map.hpp>

#include "anydsl2/def.h"
#include "anydsl2/primop.h"
#include "anydsl2/lambda.h"
#include "anydsl2/literal.h"
#include "anydsl2/memop.h"
#include "anydsl2/type.h"
#include "anydsl2/analyses/domtree.h"
#include "anydsl2/analyses/rootlambdas.h"
#include "anydsl2/analyses/scope.h"
#include "anydsl2/analyses/verifier.h"
#include "anydsl2/transform/cfg_builder.h"
#include "anydsl2/transform/inliner.h"
#include "anydsl2/transform/merge_lambdas.h"
#include "anydsl2/util/array.h"
#include "anydsl2/util/for_all.h"
#include "anydsl2/util/hash.h"

#define ANYDSL2_NO_U_TYPE \
    case PrimType_u1: \
    case PrimType_u8: \
    case PrimType_u16: \
    case PrimType_u32: \
    case PrimType_u64: ANYDSL2_UNREACHABLE;

#define ANYDSL2_NO_F_TYPE \
    case PrimType_f32: \
    case PrimType_f64: ANYDSL2_UNREACHABLE;

#if (defined(__clang__) || defined(__GNUC__)) && (defined(__x86_64__) || defined(__i386__))
#define ANYDSL2_BREAK asm("int3");
#else
#define ANYDSL2_BREAK { int* __p__ = 0; *__p__ = 42; }
#endif

#ifndef NDEBUG
#define ANYDSL2_CHECK_BREAK \
    if (breakpoints_.find(gid_) != breakpoints_.end()) \
        ANYDSL2_BREAK;
#else
#define ANYDSL2_CHECK_BREAK {}
#endif

namespace anydsl2 {

/*
 * constructor and destructor
 */

World::World()
    : primops_(1031)
    , types_(1031)
    , gid_(0)
    , pass_counter_(1)
    , sigma0_ (keep(new Sigma(*this, TypeTupleN(Node_Sigma, ArrayRef<const Type*>()))))
    , pi0_    (keep(new Pi   (*this, TypeTupleN(Node_Pi,    ArrayRef<const Type*>()))))
    , mem_    (keep(new Mem  (*this)))
    , frame_  (keep(new Frame(*this)))
#define ANYDSL2_UF_TYPE(T) ,T##_(keep(new PrimType(*this, TypeTuple0(PrimType_##T))))
#include "anydsl2/tables/primtypetable.h"
{}

World::~World() {
    for_all (primop, primops_) delete primop;
    for_all (type,   types_  ) delete type;
    for_all (lambda, lambdas_) delete lambda;
}

const Type* World::keep_nocast(const Type* type) {
    std::pair<TypeSet::iterator, bool> tp = types_.insert(type);
    assert(tp.second);
    typekeeper(type);
    return type;
}

/*
 * types
 */

const Sigma* World::sigma(ArrayRef<const Type*> elems) {
    return unify<TypeTupleN, Sigma>(TypeTupleN(Node_Sigma, elems));
}

Sigma* World::named_sigma(size_t size, const std::string& name) {
    Sigma* s = new Sigma(*this, size, name);
    assert(types_.find(s) == types_.end() && "must not be inside");
    types_.insert(s).first;
    return s;
}

const Pi* World::pi(ArrayRef<const Type*> elems) {
    return unify<TypeTupleN, Pi>(TypeTupleN(Node_Pi, elems));
}

const Generic* World::generic(size_t index) {
    return unify<GenericTuple, Generic>(GenericTuple(Node_Generic, index));
}

const Opaque* World::opaque(ArrayRef<const Type*> types, ArrayRef<uint32_t> flags) {
    return unify<OpaqueTuple, Opaque>(OpaqueTuple(Node_Opaque, types, flags));
}

const Ptr* World::ptr(const Type* ref) { return unify<TypeTuple1, Ptr>(TypeTuple1(Node_Ptr, ref)); }

/*
 * literals
 */

const PrimLit* World::literal(PrimTypeKind kind, Box box) {
    return cse<PrimLitTuple, PrimLit>(PrimLitTuple(Node_PrimLit, type(kind), box), "");
}

const PrimLit* World::literal(PrimTypeKind kind, int value) {
    switch (kind) {
#define ANYDSL2_U_TYPE(T) case PrimType_##T: return literal(T(value));
#define ANYDSL2_F_TYPE(T) ANYDSL2_U_TYPE(T)
#include "anydsl2/tables/primtypetable.h"
        default: ANYDSL2_UNREACHABLE;
    }
}

const PrimLit* World::literal(const Type* type, int value) { return literal(type->as<PrimType>()->primtype_kind(), value); }
const Any*     World::any    (const Type* type) { return cse<DefTuple0, Any   >(DefTuple0(Node_Any,    type), ""); }
const Bottom*  World::bottom (const Type* type) { return cse<DefTuple0, Bottom>(DefTuple0(Node_Bottom, type), ""); }
const PrimLit* World::zero   (const Type* type) { return zero  (type->as<PrimType>()->primtype_kind()); }
const PrimLit* World::one    (const Type* type) { return one   (type->as<PrimType>()->primtype_kind()); }
const PrimLit* World::allset (const Type* type) { return allset(type->as<PrimType>()->primtype_kind()); }

/*
 * create
 */

const Def* World::binop(int kind, const Def* lhs, const Def* rhs, const std::string& name) {
    if (is_arithop(kind))
        return arithop((ArithOpKind) kind, lhs, rhs);

    assert(is_relop(kind) && "must be a RelOp");
    return relop((RelOpKind) kind, lhs, rhs);
}

const Def* World::tuple(ArrayRef<const Def*> args, const std::string& name) {
    Array<const Type*> elems(args.size());

    for_all2 (&elem, elems, arg, args)
        elem = arg->type();

    return cse<DefTupleN, Tuple>(DefTupleN(Node_Tuple, sigma(elems), args), name);
}

const Def* World::arithop(ArithOpKind kind, const Def* a, const Def* b, const std::string& name) {
    PrimTypeKind type = a->type()->as<PrimType>()->primtype_kind();

    // bottom op bottom -> bottom
    if (a->isa<Bottom>() || b->isa<Bottom>())
        return bottom(type);

    const PrimLit* llit = a->isa<PrimLit>();
    const PrimLit* rlit = b->isa<PrimLit>();

    if (llit && rlit) {
        Box l = llit->box();
        Box r = rlit->box();

        switch (kind) {
            case ArithOp_add:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() + r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_sub:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() - r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_mul:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() * r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_udiv:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) \
                    case PrimType_##T: \
                        return rlit->is_zero() \
                             ? (const Def*) bottom(type) \
                             : (const Def*) literal(type, Box(T(l.get_##T() / r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_sdiv:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) \
                    case PrimType_##T: { \
                        typedef make_signed<T>::type S; \
                        return literal(type, Box(bcast<T , S>(bcast<S, T >(l.get_##T()) / bcast<S, T >(r.get_##T())))); \
                    }
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_urem:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) \
                    case PrimType_##T: \
                        return rlit->is_zero() \
                             ? (const Def*) bottom(type) \
                             : (const Def*) literal(type, Box(T(l.get_##T() % r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_srem:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) \
                    case PrimType_##T: { \
                        typedef make_signed<T>::type S; \
                        return literal(type, Box(bcast<T , S>(bcast<S, T >(l.get_##T()) % bcast<S, T >(r.get_##T())))); \
                    }
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_and:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() & r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_or:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() | r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_xor:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() ^ r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_shl:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() << r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_lshr:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() >> r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_ashr:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) \
                    case PrimType_##T: { \
                        typedef make_signed<T>::type S; \
                        return literal(type, Box(bcast<T , S>(bcast<S, T >(l.get_##T()) >> bcast<S, T >(r.get_##T())))); \
                    }
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case ArithOp_fadd:
                switch (type) {
#define ANYDSL2_JUST_F_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() + r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_U_TYPE;
                }
            case ArithOp_fsub:
                switch (type) {
#define ANYDSL2_JUST_F_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() - r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_U_TYPE;
                }
            case ArithOp_fmul:
                switch (type) {
#define ANYDSL2_JUST_F_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() * r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_U_TYPE;
                }
            case ArithOp_fdiv:
                switch (type) {
#define ANYDSL2_JUST_F_TYPE(T) case PrimType_##T: return literal(type, Box(T(l.get_##T() / r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_U_TYPE;
                }
            case ArithOp_frem:
                switch (type) {
#define ANYDSL2_JUST_F_TYPE(T) case PrimType_##T: return literal(type, Box(std::fmod(l.get_##T(), r.get_##T())));
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_U_TYPE;
                }
        }
    }

    if (a == b) {
        switch (kind) {
            case ArithOp_add:  return arithop_mul(literal(type, 2), a);

            case ArithOp_sub:
            case ArithOp_srem:
            case ArithOp_urem:
            case ArithOp_xor:  return zero(type);

            case ArithOp_sdiv:
            case ArithOp_udiv: return one(type);

            case ArithOp_and:
            case ArithOp_or:   return a;

            default: break;
        }
    }

    if (rlit) {
        if (rlit->is_zero()) {
            switch (kind) {
                case ArithOp_sdiv:
                case ArithOp_udiv:
                case ArithOp_srem:
                case ArithOp_urem: return bottom(type);

                case ArithOp_shl:
                case ArithOp_ashr:
                case ArithOp_lshr: return a;

                default: break;
            }
        } else if (rlit->is_one()) {
            switch (kind) {
                case ArithOp_sdiv:
                case ArithOp_udiv: return a;

                case ArithOp_srem:
                case ArithOp_urem: return zero(type);

                default: break;
            }
        } else if (rlit->primlit_value<uint64_t>() >= uint64_t(num_bits(type))) {
            switch (kind) {
                case ArithOp_shl:
                case ArithOp_ashr:
                case ArithOp_lshr: return bottom(type);

                default: break;
            }
        }
    }

    // do we have ~~x?
    if (kind == ArithOp_xor && a->is_allset() && b->is_not())
        return b->as<ArithOp>()->rhs();

    // normalize: a - b = a + -b
    if ((kind == ArithOp_sub || kind == ArithOp_fsub) && !a->is_minus_zero()) { 
        rlit = (b = arithop_minus(b))->isa<PrimLit>();
        kind = kind == ArithOp_sub ? ArithOp_add : ArithOp_fadd;
    }

    // normalize: swap literal to the left
    if (is_commutative(kind) && rlit) {
        std::swap(a, b);
        std::swap(llit, rlit);
    }

    if (llit) {
        if (llit->is_zero()) {
            switch (kind) {
                case ArithOp_mul:
                case ArithOp_sdiv:
                case ArithOp_udiv:
                case ArithOp_srem:
                case ArithOp_urem:
                case ArithOp_and:
                case ArithOp_shl:
                case ArithOp_lshr:
                case ArithOp_ashr: return zero(type);

                case ArithOp_add: 
                case ArithOp_or:
                case ArithOp_xor:  return b;

                default: break;
            }
        } else if (llit->is_one()) {
            switch (kind) {
                case ArithOp_mul: return b;
                default: break;
            }
        } else if (llit->is_allset()) {
            switch (kind) {
                case ArithOp_and: return b;
                case ArithOp_or:  return llit; // allset
                default: break;
            }
        }
    }

    // normalize: try to reorder same ops to have the literal on the left-most side
    if (is_associative(kind)) {
        const ArithOp* a_same = a->isa<ArithOp>() && a->as<ArithOp>()->arithop_kind() == kind ? a->as<ArithOp>() : 0;
        const ArithOp* b_same = b->isa<ArithOp>() && b->as<ArithOp>()->arithop_kind() == kind ? b->as<ArithOp>() : 0;
        const PrimLit* a_lhs_lit = a_same && a_same->lhs()->isa<PrimLit>() ? a_same->lhs()->as<PrimLit>() : 0;
        const PrimLit* b_lhs_lit = b_same && b_same->lhs()->isa<PrimLit>() ? b_same->lhs()->as<PrimLit>() : 0;

        if (is_commutative(kind)) {
            if (a_lhs_lit && b_lhs_lit)
                return binop(kind, binop(kind, a_lhs_lit, b_lhs_lit), binop(kind, a_same->rhs(), b_same->rhs()));
            if (llit && b_lhs_lit)
                return binop(kind, binop(kind, llit, b_lhs_lit), b_same->rhs());
            if (b_lhs_lit)
                return binop(kind, b_lhs_lit, binop(kind, a, b_same->rhs()));
        }
        if (a_lhs_lit)
            return binop(kind, a_lhs_lit, binop(kind, a_same->rhs(), b));
    }

    return cse<DefTuple2, ArithOp>(DefTuple2(kind, a->type(), a, b), name);
}

const Def* World::arithop_not(const Def* def) { return arithop_xor(allset(def->type()), def); }

const Def* World::arithop_minus(const Def* def) {
    const Def* zero;
    switch (PrimTypeKind kind = def->type()->as<PrimType>()->primtype_kind()) {
        case PrimType_f32: zero = literal_f32(-0.f); break;
        case PrimType_f64: zero = literal_f64(-0.0); break;
        default: assert(is_int(kind)); zero = this->zero(kind);
    }
    return arithop_sub(zero, def);
}

const Def* World::relop(RelOpKind kind, const Def* a, const Def* b, const std::string& name) {
    if (a->isa<Bottom>() || b->isa<Bottom>())
        return bottom(type_u1());

    RelOpKind oldkind = kind;
    switch (kind) {
        case RelOp_cmp_ugt:  kind = RelOp_cmp_ult; break;
        case RelOp_cmp_uge:  kind = RelOp_cmp_ule; break;
        case RelOp_cmp_sgt:  kind = RelOp_cmp_slt; break;
        case RelOp_cmp_sge:  kind = RelOp_cmp_sle; break;
        case RelOp_fcmp_ogt: kind = RelOp_fcmp_olt; break;
        case RelOp_fcmp_oge: kind = RelOp_fcmp_ole; break;
        case RelOp_fcmp_ugt: kind = RelOp_fcmp_ult; break;
        case RelOp_fcmp_uge: kind = RelOp_fcmp_ule; break;
        default: break;
    }

    if (oldkind != kind)
        std::swap(a, b);

    const PrimLit* llit = a->isa<PrimLit>();
    const PrimLit* rlit = b->isa<PrimLit>();

    if (llit && rlit) {
        Box l = llit->box();
        Box r = rlit->box();
        PrimTypeKind type = llit->primtype_kind();

        switch (kind) {
            case RelOp_cmp_eq:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal_u1(l.get_##T() == r.get_##T());
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case RelOp_cmp_ne:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal_u1(l.get_##T() != r.get_##T());
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case RelOp_cmp_ult:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal_u1(l.get_##T() <  r.get_##T());
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case RelOp_cmp_ule:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) case PrimType_##T: return literal_u1(l.get_##T() <= r.get_##T());
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case RelOp_cmp_slt:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) \
                    case PrimType_##T: { \
                        typedef make_signed< T >::type S; \
                        return literal_u1(bcast<S, T>(l.get_##T()) <  bcast<S, T>(r.get_##T())); \
                    }
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case RelOp_cmp_sle:
                switch (type) {
#define ANYDSL2_JUST_U_TYPE(T) \
                    case PrimType_##T: { \
                        typedef make_signed< T >::type S; \
                        return literal_u1(bcast<S, T>(l.get_##T()) <= bcast<S, T>(r.get_##T())); \
                    }
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_F_TYPE;
                }
            case RelOp_fcmp_oeq:
                switch (type) {
#define ANYDSL2_JUST_F_TYPE(T) case PrimType_##T: return literal_u1(l.get_##T() == r.get_##T());
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_U_TYPE;
                }
            case RelOp_fcmp_one:
                switch (type) {
#define ANYDSL2_JUST_F_TYPE(T) case PrimType_##T: return literal_u1(l.get_##T() != r.get_##T());
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_U_TYPE;
                }
            case RelOp_fcmp_olt:
                switch (type) {
#define ANYDSL2_JUST_F_TYPE(T) case PrimType_##T: return literal_u1(l.get_##T() <  r.get_##T());
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_U_TYPE;
                }
            case RelOp_fcmp_ole:
                switch (type) {
#define ANYDSL2_JUST_F_TYPE(T) case PrimType_##T: return literal_u1(l.get_##T() <= r.get_##T());
#include "anydsl2/tables/primtypetable.h"
                    ANYDSL2_NO_U_TYPE;
                }
            default:
                ANYDSL2_UNREACHABLE;
        }
    }

    if (a == b) {
        switch (kind) {
            case RelOp_cmp_ult:
            case RelOp_cmp_slt: 
            case RelOp_cmp_eq:  return zero(u1_);
            case RelOp_cmp_ule:
            case RelOp_cmp_sle:
            case RelOp_cmp_ne:  return one(u1_);
            default: break;
        }
    }

    return cse<DefTuple2, RelOp>(DefTuple2(kind, type_u1(), a, b), name);
}

const Def* World::convop(ConvOpKind kind, const Def* from, const Type* to, const std::string& name) {
    if (from->isa<Bottom>())
        return bottom(to);

#if 0
    if (const PrimLit* lit = from->isa<PrimLit>())
        Box box = lit->box();
        PrimTypeKind type = lit->primtype_kind();

        // TODO folding
    }
#endif

    return cse<DefTuple1, ConvOp>(DefTuple1(kind, to, from), name);
}

const Def* World::extract(const Def* tuple, u32 index, const std::string& name) {
    return extract(tuple, literal_u32(index), name);
}

const Def* World::extract(const Def* agg, const Def* index, const std::string& name) {
    if (agg->isa<Bottom>())
        return bottom(agg->type()->as<Sigma>()->elem_via_lit(index));

    if (const Tuple* tuple = agg->isa<Tuple>())
        return tuple->op_via_lit(index);

    if (const Insert* insert = agg->isa<Insert>()) {
        if (index == insert->index())
            return insert->value();
        else
            return extract(insert->tuple(), index);
    }

    const Type* type = agg->type()->as<Sigma>()->elem_via_lit(index);
    return cse<DefTuple2, Extract>(DefTuple2(Node_Extract, type, agg, index), name);
}

const Def* World::insert(const Def* tuple, u32 index, const Def* value, const std::string& name) {
    return insert(tuple, literal_u32(index), value, name);
}

const Def* World::insert(const Def* agg, const Def* index, const Def* value, const std::string& name) {
    if (agg->isa<Bottom>() || value->isa<Bottom>())
        return bottom(agg->type());

    if (const Tuple* tup = agg->isa<Tuple>()) {
        Array<const Def*> args(tup->size());
        std::copy(agg->ops().begin(), agg->ops().end(), args.begin());
        args[index->primlit_value<size_t>()] = value;

        return tuple(args);
    }

    return cse<DefTuple3, Insert>(DefTuple3(Node_Extract, agg->type(), agg, index, value), name);
}

const Load* World::load(const Def* m, const Def* ptr, const std::string& name) {
    return cse<DefTuple2, Load>(DefTuple2(Node_Load, sigma2(mem(), ptr->type()->as<Ptr>()->ref()), m, ptr), name);
}
const Store* World::store(const Def* m, const Def* ptr, const Def* val, const std::string& name) {
    return cse<DefTuple3, Store>(DefTuple3(Node_Store, mem(), m, ptr, val), name);
}
const Enter* World::enter(const Def* m, const std::string& name) {
    return cse<DefTuple1, Enter>(DefTuple1(Node_Enter, sigma2(mem(), frame()), m), name);
}
const Leave* World::leave(const Def* m, const Def* frame, const std::string& name) {
    assert(frame->type()->isa<Frame>());
    return cse<DefTuple2, Leave>(DefTuple2(Node_Leave, mem(), m, frame), name);
}
const LEA* World::lea(const Def* ptr, const Def* index, const std::string& name) {
    const Type* type = this->ptr(ptr->type()->as<Ptr>()->ref()->as<Sigma>()->elem_via_lit(index));
    return cse<DefTuple2, LEA>(DefTuple2(Node_LEA, type, ptr, index), name);
}

const Slot* World::slot(const Type* type, size_t index, const Def* frame, const std::string& name) {
    return cse<SlotTuple, Slot>(SlotTuple(Node_Slot, type->to_ptr(), index, frame), name);
}

const CCall* World::c_call(const std::string& callee, const Def* m, ArrayRef<const Def*> args,
                           const Type* rettype, bool vararg, const std::string& name) {
    const Type* type = rettype && !rettype->isa<Mem>() ? (const Type*) sigma2(mem(), rettype) : (const Type*) mem();
    return cse<CCallTuple, CCall>(CCallTuple(Node_CCall, type, callee, m, args, vararg), name);
}

const Def* World::select(const Def* cond, const Def* a, const Def* b, const std::string& name) {
    if (cond->isa<Bottom>() || a->isa<Bottom>() || b->isa<Bottom>())
        return bottom(a->type());

    if (const PrimLit* lit = cond->isa<PrimLit>())
        return lit->box().get_u1().get() ? a : b;

    if (cond->is_not()) {
        cond = cond->as<ArithOp>()->rhs();
        std::swap(a, b);
    }

    return cse<DefTuple3, Select>(DefTuple3(Node_Select, a->type(), cond, a, b), name);
}

const TypeKeeper* World::typekeeper(const Type* type, const std::string& name) {
    return cse<DefTuple0, TypeKeeper>(DefTuple0(Node_TypeKeeper, type), name)->as<TypeKeeper>();
}

Lambda* World::lambda(const Pi* pi, LambdaAttr attr, const std::string& name) {
    ANYDSL2_CHECK_BREAK
    Lambda* l = new Lambda(gid_++, pi, attr, true, name);
    lambdas_.insert(l);

    size_t i = 0;
    for_all (elem, pi->elems())
        l->params_.push_back(param(elem, l, i++));

    return l;
}

Lambda* World::basicblock(const std::string& name) {
    ANYDSL2_CHECK_BREAK
    Lambda* bb = new Lambda(gid_++, pi0(), LambdaAttr(0), false, name);
    lambdas_.insert(bb);
    return bb;
}

const Def* World::rebuild(const PrimOp* in, ArrayRef<const Def*> ops) {
    int kind = in->kind();
    const Type* type = in->type();
    const std::string& name = in->name;

    if (is_arithop(kind)) { assert(ops.size() == 2); return arithop((ArithOpKind) kind, ops[0], ops[1], name); }
    if (is_relop  (kind)) { assert(ops.size() == 2); return relop(  (RelOpKind  ) kind, ops[0], ops[1], name); }
    if (is_convop (kind)) { assert(ops.size() == 1); return convop( (ConvOpKind ) kind, ops[0],   type, name); }

    switch (kind) {
        case Node_Enter:   assert(ops.size() == 1); return enter(  ops[0], name);
        case Node_Extract: assert(ops.size() == 2); return extract(ops[0], ops[1], name);
        case Node_Insert:  assert(ops.size() == 3); return insert( ops[0], ops[1], ops[2], name);
        case Node_Leave:   assert(ops.size() == 2); return leave(  ops[0], ops[1], name);
        case Node_Load:    assert(ops.size() == 2); return load(   ops[0], ops[1], name);
        case Node_CCall: {
            assert(ops.size() >= 1);
            const CCall* ocall = in->as<CCall>();
            // extract the internal type of this call or avoid an explicit return type
            const Type* ctype = ocall->returns_void() ? (const Type*)0 :
                ocall->type()->as<Sigma>()->elem(1);
            return c_call(ocall->callee(), ops[0], ops.slice_back(1), ctype, ocall->vararg(), ocall->name);
        }
        case Node_Select:  assert(ops.size() == 3); return select( ops[0], ops[1], ops[2], name);
        case Node_Slot:    assert(ops.size() == 1); return slot(   type->as<Ptr>()->ref(), in->as<Slot>()->index(), ops[0], name);
        case Node_Store:   assert(ops.size() == 3); return store(  ops[0], ops[1], ops[2], name);
        case Node_Tuple:                            return tuple(  ops, name);
        case Node_Bottom:  assert(ops.empty());     return bottom(type);
        case Node_Any:     assert(ops.empty());     return any(type);
        case Node_PrimLit: assert(ops.empty());     return literal((PrimTypeKind) kind, in->as<PrimLit>()->box());
        default: ANYDSL2_UNREACHABLE;
    }
}

const Param* World::param(const Type* type, Lambda* lambda, size_t index, const std::string& name) {
    ANYDSL2_CHECK_BREAK
    return new Param(gid_++, type, lambda, index, name);
}

/*
 * cse + unify
 */

void World::cse_break(const PrimOp* primop) {
#ifndef NDEBUG
    if (breakpoints_.find(primop->gid()) != breakpoints_.end()) ANYDSL2_BREAK
#endif
}

template<class T, class U>
const U* World::unify(const T& tuple) {
    TypeSet::iterator i = types_.find(tuple, std::ptr_fun<const T&, size_t>(hash_tuple),
                                             std::ptr_fun<const T&, const Node*, bool>(smart_eq<T, U>));
    if (i != types_.end()) return (*i)->as<U>();

    std::pair<TypeSet::iterator, bool> p = types_.insert(new U(*this, tuple));
    assert(p.second && "hash/equal broken");
    return (*p.first)->as<U>();
}

/*
 * optimizations
 */

template<class S>
void World::wipe_out(const size_t pass, S& set) {
    for (typename S::iterator i = set.begin(); i != set.end();) {
        typename S::iterator j = i++;
        const Def* def = *j;
        if (!def->is_visited(pass)) {
            delete def;
            set.erase(j);
        }
    }
}

template<class S>
void World::unregister_uses(const size_t pass, S& set) {
    for (typename S::iterator i = set.begin(), e = set.end(); i != e; ++i) {
        const Def* def = *i;
        if (!def->is_visited(pass)) {
            for (size_t i = 0, e = def->size(); i != e; ++i) {
                //if (!def->op(i)->is_visited(pass))
                    def->unregister_use(i);
            }
        }
    }
}

void World::unreachable_code_elimination() {
    const size_t pass = new_pass();

    for_all (lambda, lambdas())
        if (lambda->attr().is_extern())
            uce_insert(pass, lambda);

    for_all (lambda, lambdas()) {
        if (!lambda->is_visited(pass))
            lambda->destroy_body();
    }
}

void World::uce_insert(const size_t pass, Lambda* lambda) {
    if (lambda->visit(pass)) return;

    for_all (succ, lambda->succs())
        uce_insert(pass, succ);
}

void World::dead_code_elimination() {
    const size_t pass = new_pass();

    for_all (primop, primops()) {
        if (const TypeKeeper* tk = primop->isa<TypeKeeper>())
            dce_insert(pass, tk);
    }

    for_all (lambda, lambdas()) {
        if (lambda->attr().is_extern()) {
            for_all (param, lambda->params()) {
                if (param->order() >= 1) {
                    for_all (use, param->uses()) {
                        if (Lambda* caller = use->isa_lambda())
                            dce_insert(pass, caller);
                    }
                }
            }
        }
    }

    for_all (lambda, lambdas()) {
        if (!lambda->is_visited(pass))
            lambda->destroy_body();
        else {
            for (size_t i = 0, e = lambda->num_args(); i != e; ++i) {
                const Def* arg = lambda->arg(i);
                if (!arg->is_visited(pass)) {
                    const Bottom* bot = bottom(arg->type());
                    bot->visit(pass);
                    lambda->update_arg(i, bot);
                }
            }
        }
    }

    unregister_uses(pass, primops_);
    unregister_uses(pass, lambdas_);
    wipe_out(pass, primops_);
    wipe_out(pass, lambdas_);
}

void World::dce_insert(const size_t pass, const Def* def) {
#ifndef NDEBUG
    if (const PrimOp* primop = def->isa<PrimOp>()) assert(primops_.find(primop)          != primops_.end());
    if (      Lambda* lambda = def->isa_lambda() ) assert(lambdas_.find(lambda)          != lambdas_.end());
    if (const Param*  param  = def->isa<Param>() ) assert(lambdas_.find(param->lambda()) != lambdas_.end());
#endif

    if (def->visit(pass)) return;

    if (const PrimOp* primop = def->isa<PrimOp>()) {
        for_all (op, primop->ops())
            dce_insert(pass, op);
    } else if (const Param* param = def->isa<Param>()) {
        for_all (peek, param->peek())
            dce_insert(pass, peek.def());
    } else {
        Lambda* lambda = def->as_lambda();
        for_all (pred, lambda->preds()) // insert control-dependent lambdas
            dce_insert(pass, pred);
        if (!lambda->empty()) {
            dce_insert(pass, lambda->to());
            if (lambda->to()->isa<Param>()) {
                for_all (arg, lambda->args())
                    dce_insert(pass, arg);
            }
        }
    }
}

void World::unused_type_elimination() {
    const size_t pass = new_pass();

    for_all (primop, primops())
        ute_insert(pass, primop->type());

    for_all (lambda, lambdas()) {
        ute_insert(pass, lambda->type());
        for_all (param, lambda->params())
            ute_insert(pass, param->type());
    }

    for (TypeSet::iterator i = types_.begin(); i != types_.end();) {
        const Type* type = *i;
        if (type->is_visited(pass))
            ++i;
        else {
            delete type;
            i = types_.erase(i);
        }
    }
}

void World::ute_insert(const size_t pass, const Type* type) {
    assert(types_.find(type) != types_.end() && "not in map");

    if (type->visit(pass)) return;

    for_all (elem, type->elems())
        ute_insert(pass, elem);
}

void World::cleanup() {
    unreachable_code_elimination();
    dead_code_elimination();
    unused_type_elimination();
}

void World::opt() {
    cleanup();
    assert(verify(*this)); cfg_transform(*this); cleanup();
    assert(verify(*this)); inliner(*this);       cleanup();
    assert(verify(*this)); merge_lambdas(*this); cleanup();
}

PrimOp* World::release(const PrimOp* primop) {
    PrimOpSet::iterator i = primops_.find(primop);
    assert(i != primops_.end() && "must be found");
    assert(primop == *i);
    primops_.erase(i);

    return const_cast<PrimOp*>(primop);
}

void World::reinsert(const PrimOp* primop) {
    assert(primops_.find(primop) == primops_.end() && "must not be found");
    primops_.insert(primop);
}

/*
 * other
 */

void World::dump(bool fancy) {
    if (fancy) {
        LambdaSet roots = find_root_lambdas(lambdas());

        for_all (root, roots) {
            Scope scope(root);
            for_all (lambda, scope.rpo())
                lambda->dump_body(fancy, scope.domtree().depth(lambda));
        }
    } else {
        for_all (lambda, lambdas())
            lambda->dump_body(false, 0);
    }
    std::cout << std::endl;
}

} // namespace anydsl2
