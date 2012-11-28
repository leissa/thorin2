#include "anydsl2/primop.h"

#include "anydsl2/literal.h"
#include "anydsl2/type.h"
#include "anydsl2/world.h"
#include "anydsl2/util/array.h"
#include "anydsl2/util/hash.h"

namespace anydsl2 {

//------------------------------------------------------------------------------

bool equal_op(const DefTuple0& tuple, const PrimOp* def) {
    return equal_kind_type(tuple, 0, def);
}
bool equal_op(const DefTuple1& tuple, const PrimOp* def) {
    return equal_kind_type(tuple, 1, def) && tuple.get<2>() == def->op(0);
}
bool equal_op(const DefTuple2& tuple, const PrimOp* def) {
    return equal_kind_type(tuple, 2, def) 
        && tuple.get<2>() == def->op(0) && tuple.get<3>() == def->op(1);
}
bool equal_op(const DefTuple3& tuple, const PrimOp* def) {
    return equal_kind_type(tuple, 3, def) 
        && tuple.get<2>() == def->op(0) && tuple.get<3>() == def->op(1) && tuple.get<4>() == def->op(2);
}
bool equal_op(const DefTupleN& tuple, const PrimOp* def) {
    return equal_kind_type(tuple, tuple.get<2>().size(), def) && tuple.get<2>() == def->ops();
}

//------------------------------------------------------------------------------

RelOp::RelOp(RelOpKind kind, const Def* lhs, const Def* rhs, const std::string& name)
    : BinOp((NodeKind) kind, lhs->world().type_u1(), lhs, rhs, name)
{}

//------------------------------------------------------------------------------

Select::Select(const Def* cond, const Def* t, const Def* f, const std::string& name)
    : PrimOp(Node_Select, 3, t->type(), name)
{
    set_op(0, cond);
    set_op(1, t);
    set_op(2, f);
    assert(cond->type() == world().type_u1() && "condition must be of u1 type");
    assert(t->type() == f->type() && "types of both values must be equal");
}

//------------------------------------------------------------------------------

TupleOp::TupleOp(NodeKind kind, size_t size, const Type* type, const Def* tuple, const Def* index, const std::string& name)
    : PrimOp(kind, size, type, name)
{
    set_op(0, tuple);
    set_op(1, index);
}

//------------------------------------------------------------------------------

Extract::Extract(const Def* tuple, const Def* index, const std::string& name)
    : TupleOp(Node_Extract, 2, tuple->type()->as<Sigma>()->elem_via_lit(index), tuple, index, name)
{}

//------------------------------------------------------------------------------

Insert::Insert(const Def* tuple, const Def* index, const Def* value, const std::string& name)
    : TupleOp(Node_Insert, 3, tuple->type(), tuple, index, name)
{
    set_op(2, value);
    assert(tuple->type()->as<Sigma>()->elem_via_lit(index) == value->type() && "type error");
}

//------------------------------------------------------------------------------

Tuple::Tuple(World& world, ArrayRef<const Def*> args, const std::string& name)
    : PrimOp(Node_Tuple, args.size(), (const Type*) 0, name)
{
    if (ops().empty())
        set_type(world.sigma0());
    else {
        Array<const Type*> types(ops().size());
        size_t x = 0;
        for_all (arg, args) {
            set_op(x, args[x]);
            types[x++] = arg->type();
        }

        set_type(world.sigma(types));
    }
}

//------------------------------------------------------------------------------

} // namespace anydsl2
