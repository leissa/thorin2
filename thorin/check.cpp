#include "thorin/check.h"

#include "thorin/world.h"

namespace thorin {

/*
 * Infer
 */

const Def* refer(const Def* def) { return def ? Infer::find(def) : nullptr; }

const Def* Infer::find(const Def* def) {
    // find root
    auto res = def;
    for (auto infer = res->isa_nom<Infer>(); infer && infer->op(); infer = res->isa_nom<Infer>()) res = infer->op();

    // path compression: set all Infers along the chain to res
    for (auto infer = def->isa_nom<Infer>(); infer && infer->op(); infer = def->isa_nom<Infer>()) {
        def = infer->op();
        infer->set(res);
    }

    assert((!res->isa<Infer>() || res != res->op(0)) && "an Infer shouldn't point to itself");

    // If we have an Infer as operand, try to get rid of it now.
    // TODO why does this not work?
    // if (res->isa_structural() && res->has_dep(Dep::Infer)) {
    if (res->isa<Tuple>() || res->isa<Type>()) {
        auto new_type = refer(res->type());
        bool update   = new_type != res->type();

        auto new_ops = DefArray(res->num_ops(), [res, &update](size_t i) {
            auto r = refer(res->op(i));
            update |= r != res->op(i);
            return r;
        });

        if (update) return res->rebuild(res->world(), new_type, new_ops, res->dbg());
    }

    return res;
}

const Def* Infer::inflate(Ref ty, Defs elems_t) {
    auto& w = world();
    if (!is_set()) {
        DefArray infer_ops(elems_t.size(), [&](size_t i) { return w.nom_infer(elems_t[i], dbg()); });
        set(w.tuple(infer_ops, dbg()));
        if (auto t = type()->isa_nom<Infer>(); t && !t->is_set() && ty) t->set(ty);
    }

    return op();
}

// TODO try to merge with above
const Def* Infer::inflate(Ref ty, u64 n, Ref elem_t) {
    auto& w = world();
    if (!is_set()) {
        DefArray infer_ops(n, [&](size_t) { return w.nom_infer(elem_t, dbg()); });
        set(w.tuple(infer_ops, dbg()));
        if (auto t = type()->isa_nom<Infer>(); t && !t->is_set() && ty) t->set(ty);
    }

    return op();
}

/*
 * Checker
 */

bool Checker::equiv(Ref r1, Ref r2, Ref dbg) {
    auto d1 = *r1; // find
    auto d2 = *r2; // find

    if (d1 == d2) return true;
    if (!d1 || !d2) return false;

    auto i1 = d1->isa_nom<Infer>();
    auto i2 = d2->isa_nom<Infer>();

    if ((!i1 && !d1->is_set()) || (!i2 && !d2->is_set())) return false;

    if (i1 && i2) {
        // union by rank
        if (i1->rank() < i2->rank()) std::swap(i1, i2); // make sure i1 is heavier or equal
        i2->set(i1);                                    // make i1 new root
        if (i1->rank() == i2->rank()) ++i1->rank();
        return true;
    } else if (i1) {
        i1->set(d2);
        return true;
    } else if (i2) {
        i2->set(d1);
        return true;
    }

    assert(!i1 && !i2);
    if (d1->gid() > d2->gid()) std::swap(d1, d2); // normalize

    if (auto [it, ins] = equiv_.emplace(std::pair(d1, d2), Equiv::Unknown); !ins) {
        switch (it->second) {
            case Equiv::Distinct: return false;
            case Equiv::Unknown:
            case Equiv::Equiv: return true;
            default: unreachable();
        }
    }

    bool res                  = equiv_internal(d1, d2, dbg);
    equiv_[std::pair(d1, d2)] = res ? Equiv::Equiv : Equiv::Distinct;
    return res;
}

bool Checker::equiv_internal(Ref d1, Ref d2, Ref dbg) {
    if (!equiv(d1->type(), d2->type(), dbg)) return false;
    if (d1->isa<Top>() || d2->isa<Top>()) return equiv(d1->type(), d2->type(), dbg);

    if (auto n1 = d1->isa_nom()) {
        if (auto n2 = d2->isa_nom()) vars_.emplace_back(n1, n2);
    }

    if (d1->isa<Sigma, Arr>()) {
        if (!equiv(d1->arity(), d2->arity(), dbg)) return false;

        if (auto a = isa_lit(d1->arity())) {
            for (size_t i = 0; i != a; ++i) {
                if (!equiv(d1->proj(*a, i), d2->proj(*a, i), dbg)) return false;
            }
            return true;
        }
    }

    if (d1->node() != d2->node() || d1->flags() != d2->flags() || d1->num_ops() != d2->num_ops()) return false;

    if (auto var = d1->isa<Var>()) { // vars are equal if they appeared under the same binder
        for (auto [n1, n2] : vars_) {
            if (var->nom() == n1) return d2->as<Var>()->nom() == n2;
        }
        // TODO what if Var is free?
        return false;
    }

    return std::ranges::equal(d1->ops(), d2->ops(), [&](auto op1, auto op2) { return equiv(op1, op2, dbg); });
}

bool Checker::assignable(Ref type, Ref val, Ref dbg /*= {}*/) {
    auto val_ty = refer(val->type());
    if (type == val_ty) return true;

    auto infer = val->isa_nom<Infer>();
    if (auto sigma = type->isa<Sigma>()) {
        if (!infer && !equiv(type->arity(), val_ty->arity(), dbg)) return false;

        size_t a = sigma->num_ops();
        auto red = sigma->reduce(val);

        if (infer) infer->inflate(sigma, red);

        for (size_t i = 0; i != a; ++i) {
            if (!assignable(red[i], val->proj(a, i, dbg), dbg)) return false;
        }

        return true;
    } else if (auto arr = type->isa<Arr>()) {
        if (!infer && !equiv(type->arity(), val_ty->arity(), dbg)) return false;

        if (auto a = isa_lit(arr->arity())) {
            if (infer) infer->inflate(arr, *a, arr->body());

            for (size_t i = 0; i != *a; ++i) {
                if (!assignable(arr->proj(*a, i), val->proj(*a, i, dbg), dbg)) return false;
            }

            return true;
        }
    } else if (auto vel = val->isa<Vel>()) {
        if (assignable(type, vel->value(), dbg)) return true;
    }

    return equiv(type, val_ty, dbg);
}

const Def* Checker::is_uniform(Defs defs, Ref dbg) {
    assert(!defs.empty());
    auto first = defs.front();
    auto ops   = defs.skip_front();
    return std::ranges::all_of(ops, [&](auto op) { return equiv(first, op, dbg); }) ? first : nullptr;
}

/*
 * infer
 */

const Def* Pi::infer(const Def* dom, const Def* codom) {
    auto& w = dom->world();
    return w.umax<Sort::Kind>({dom->unfold_type(), codom->unfold_type()});
}

/*
 * check
 */

void Arr::check() {
    auto& w = world();
    auto t  = body()->unfold_type();

    if (!w.checker().equiv(t, type(), type()->dbg()))
        err(dbg(), "declared sort '{}' of array does not match inferred one '{}'", type(), t);
}

void Sigma::check() {
    // TODO
}

void Lam::check() {
    auto& w = world();
    return; // TODO
    if (!w.checker().equiv(filter()->type(), w.type_bool(), filter()->dbg()))
        err(filter()->dbg(), "filter '{}' of lambda is of type '{}' but must be of type '.Bool'", filter(),
            filter()->type());
    if (!w.checker().equiv(body()->type(), codom(), body()->dbg()))
        err(body()->dbg(), "body '{}' of lambda is of type '{}' but its codomain is of type '{}'", body(),
            body()->type(), codom());
}

void Pi::check() {
    auto& w = world();
    auto t  = infer(dom(), codom());

    if (!w.checker().equiv(t, type(), type()->dbg()))
        err(dbg(), "declared sort '{}' of function type does not match inferred one '{}'", type(), t);
}

} // namespace thorin
