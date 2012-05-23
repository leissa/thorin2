#ifndef ANYDSL_PRIMOP_H
#define ANYDSL_PRIMOP_H

#include <boost/array.hpp>

#include "anydsl/air/enums.h"
#include "anydsl/air/def.h"
#include "anydsl/air/use.h"

namespace anydsl {

class PrimLit;

//------------------------------------------------------------------------------

class PrimOp : public Value {
protected:

    PrimOp(IndexKind index, const Type* type)
        : Value(index, type)
    {}

public:

    PrimOpKind primOpKind() const { return (PrimOpKind) index(); }
};

//------------------------------------------------------------------------------

class BinOp : public PrimOp {
protected:

    BinOp(IndexKind index, const Type* type, Def* ldef, Def* rdef)
        : PrimOp(index, type)
        , luse(this, ldef)
        , ruse(this, rdef)
    {
        anydsl_assert(ldef->type() == rdef->type(), "types are not equal");
    }

    static ValueNumber VN(IndexKind kind, Def* ldef, Def* rdef) {
        return ValueNumber(kind, uintptr_t(ldef), uintptr_t(rdef));
    }

public:

    typedef boost::array<Use*, 2> LRUse;
    typedef boost::array<const Use*, 2> ConstLRUse;

    LRUse lruse() { return (LRUse){{ &luse, &ruse }}; }
    ConstLRUse lruse() const { return (ConstLRUse){{ &ruse, &ruse }}; }

public:

    Use luse;
    Use ruse;
};

//------------------------------------------------------------------------------

class ArithOp : public BinOp {
private:

    ArithOp(const ValueNumber& vn)
        : BinOp((IndexKind) vn.index, 
                ((Def*) vn.op1)->type(), 
                (Def*) vn.op1, 
                (Def*) vn.op2)
    {}

    static ValueNumber VN(ArithOpKind kind, Def* ldef, Def* rdef) {
        return BinOp::VN((IndexKind) kind, ldef, rdef);
    }

public:

    ArithOpKind kind() { return (ArithOpKind) index(); }

    friend class World;
};

//------------------------------------------------------------------------------

class RelOp : public BinOp {
private:

    RelOp(const ValueNumber& vn);

    static ValueNumber VN(RelOpKind kind, Def* ldef, Def* rdef) {
        return ValueNumber((IndexKind) kind, uintptr_t(ldef), uintptr_t(rdef));
    }

public:

    RelOpKind kind() { return (RelOpKind) index(); }

    friend class World;
};

//------------------------------------------------------------------------------

class Select : public PrimOp {
private:

    Select(const ValueNumber& vn);

    static ValueNumber VN(Def* cond, Def* t, Def* f) {
        return ValueNumber(Index_Select, uintptr_t(cond), uintptr_t(t), uintptr_t(f));
    }

public:

    Use cond;
    Use tuse;
    Use fuse;

    RelOpKind kind() { return (RelOpKind) index(); }

    friend class World;
};

//------------------------------------------------------------------------------

class Proj : public PrimOp {
private:

    Proj(const ValueNumber& vn);

    static ValueNumber VN(Def* tuple, PrimLit* elem) {
        return ValueNumber(Index_Proj, uintptr_t(tuple), uintptr_t(elem));
    }
    
    Use tuple;
    Use elem;

    friend class World;
};

//------------------------------------------------------------------------------

class Tuple : public PrimOp {
private:

    Tuple(const ValueNumber& vn);

#if 0
    template <class C>
    static ValueNumber VN(C container) {
        ValueNumber vn(container.size());
        FOREACH(i, container)

    }
#endif
};

//------------------------------------------------------------------------------

} // namespace anydsl

#endif // ANYDSL_PRIMOP_H
