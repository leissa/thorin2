#include "thorin/check.h"
#include "thorin/transform/closure_conv.h"
#include "thorin/analyses/scope.h"

namespace thorin {

/* auxillary functions */

/* annotations */

CA operator&(CA a, CA b) {
    if ((uint8_t) b < (uint8_t) a)
        std::swap(a, b);
    if (a == CA::bot || a == b)
        return b;
    if (a == CA::proc && b == CA::proc_e)
        return CA::proc_e;
    return CA::unknown;
}

std::tuple<const Def*, CA> ca_isa_mark(const Def* def) {
    if (auto query = isa<Tag::CA>(def)) {
        return {query->arg(), query.flags()};
    }
    return {nullptr, CA::bot};
}

// Adjust the index of an argument to account for the env param
size_t shift_env(size_t i) {
    return (i < CLOSURE_ENV_PARAM) ? i : i - 1_u64;
}

// Same but skip the env param
size_t skip_env(size_t i) {
    return (i < CLOSURE_ENV_PARAM) ? i : i + 1_u64;
}

const Def* closure_insert_env(size_t i, const Def* env, std::function<const Def* (size_t)> f) {
    return (i == CLOSURE_ENV_PARAM) ? env : f(shift_env(i));
}

const Def* closure_remove_env(size_t i, std::function<const Def* (size_t)> f) {
    return f(skip_env(i));
}

static const Def* ctype(World& w, const Pi* pi, const Def* env_type = nullptr, 
        std::function<const Def*(const Def*)> rw_args = [](auto d) { return d; }) {
    if (!env_type) {
        auto sigma = w.nom_sigma(w.kind(), 3_u64, w.dbg("closure_type"));
        sigma->set(0_u64, w.kind());
        sigma->set(1_u64, ctype(w, pi, sigma->var(0_u64), rw_args));
        sigma->set(2_u64, sigma->var(0_u64));
        return sigma;
    } else {
        auto dom = w.sigma(DefArray(pi->num_doms() + 1, [&](auto i) {
            return closure_insert_env(i, env_type, [&](auto i) { return rw_args(pi->dom(i)); });
        }));
        assert(dom->type() == w.kind());
        auto new_pi = w.cn(dom);
        return new_pi;
    }
}

Sigma* ctype(const Pi* pi) {
    return ctype(pi->world(), pi, nullptr)->as_nom<Sigma>();
}

const Pi* ctype_to_pi(const Def* ct, const Def* new_env_type) {
    assert(isa_ctype(ct));
    auto& w = ct->world();
    auto pi = ct->op(1_u64)->isa<Pi>();
    assert(pi);
    if (!new_env_type)
        return w.cn(w.tuple(DefArray(pi->num_doms() - 1, [&](auto i) {
            return pi->dom(skip_env(i));
        })));
    else
        return w.cn(w.sigma(DefArray(pi->num_doms(), [&](auto i) {
            return (i == CLOSURE_ENV_PARAM) ? new_env_type : pi->dom(i);
        })));
}

const Sigma* isa_ctype(const Def* def) {
    auto& w = def->world();
    auto sig = def->isa_nom<Sigma>();
    if (!sig || sig->num_ops() < 3 || sig->op(0_u64) != w.kind())
        return nullptr;
    auto var = sig->var(0_u64);
    if (sig->op(2_u64) != var)
        return nullptr;
    auto pi = sig->op(1_u64)->isa<Pi>();
    return (pi && pi->is_cn() && pi->num_ops() > 1_u64 && pi->dom(CLOSURE_ENV_PARAM) == var)
        ? sig : nullptr;
}

const Def* pack_closure_dbg(const Def* env, const Def* lam, const Def* dbg, const Def* ct) {
    assert(env && lam);
    assert(!ct || isa_ctype(ct));
    auto pi = lam->type()->isa<Pi>();
    assert(pi && env->type() == pi->dom(CLOSURE_ENV_PARAM));
    ct = (ct) ? ct : ctype(pi);
    auto& w = env->world();
    return w.tuple(ct, {env->type(), lam, env}, dbg)->isa<Tuple>();
}

std::tuple<const Def*, const Def*, const Def*> unpack_closure(const Def* c) {
    assert(c && isa_ctype(c->type()));
    auto& w = c->world();
    auto env_type = c->proj(0_u64);
    auto pi = ctype_to_pi(c->type(), env_type);
    auto fn = w.extract_(pi, c, w.lit_int(3, 1));
    auto env = w.extract_(env_type, c, w.lit_int(3, 2));
    return {env_type, fn, env};
}

const Def* apply_closure(const Def* closure, const Def* args) {
    auto& w = closure->world();
    auto [_, fn, env] = unpack_closure(closure);
    auto pi = fn->type()->as<Pi>();
    return w.app(fn, DefArray(pi->num_doms(), [&](auto i) {
        return closure_insert_env(i, env, args);
    }));
}

std::tuple<Lam*, CA> ca_isa_marked_lam(const Def* def) {
    auto ca = CA::bot;
    if (auto q = isa<Tag::CA>(def)) {
        ca = q.flags();
        def = q->arg();
    }
    return {def->isa_nom<Lam>(), ca};
}

static std::tuple<const Def*, const Tuple*, CA>
isa_folded_branch(const Def* def) {
    auto ca = CA::bot;
    if (auto proj = def->isa<Extract>()) {
        if (auto tuple = proj->tuple()->isa<Tuple>()) {
            if (std::all_of(tuple->ops().begin(), tuple->ops().end(), [&](const Def* d) { 
                    auto [lam, v] = ca_isa_marked_lam(d); ca &= v; return lam != nullptr; }))
                return {proj->index(), tuple, ca};
        }
    }
    return {nullptr, nullptr, CA::bot};
}

ClosureLit isa_closure_lit(const Def* def, bool lambda_or_branch) {
    auto tpl = def->isa<Tuple>();
    if (tpl && isa_ctype(def->type())) {
        auto fnc = std::get<1_u64>(unpack_closure(tpl));
        if (auto [lam, ca] = ca_isa_marked_lam(fnc); lam)
            return ClosureLit(tpl, ca);
        if (auto [idx, lams, ca] = isa_folded_branch(fnc); idx && lams)
            return ClosureLit(tpl, ca);
        else if (!lambda_or_branch)
            return ClosureLit(tpl, CA::bot);
    }
    return ClosureLit(nullptr, CA::bot);
}

const Def* ClosureLit::env() {
    assert(def_);
    return std::get<2_u64>(unpack_closure(def_));
}

const Def* ClosureLit::fnc() {
    assert(def_);
    return std::get<1_u64>(unpack_closure(def_));
}

Lam* ClosureLit::fnc_as_lam() {
    auto [lam, _] = ca_isa_marked_lam(fnc());
    return lam;
}

std::pair<const Def*, const Tuple*> ClosureLit::fnc_as_folded() {
    auto [idx, tupel, _] = isa_folded_branch(fnc());
    return {idx, tupel};
}

const Def* ClosureLit::var(size_t i) {
    assert(i < fnc_type()->num_doms());
    if (auto lam = fnc_as_lam())
        return lam->var(i);
    auto [idx, lams] = fnc_as_folded();
    assert(idx && lams && "closure should be a lam or folded branch");
    auto& w = idx->world();
    auto tuple = w.tuple(DefArray(lams->num_ops(), [&](size_t j) {
        auto [lam, _] = ca_isa_marked_lam(lams->op(j));
        return lam->var(i);
    }));
    return w.extract(tuple, idx);
}

const Def* ClosureLit::env_var() {
    return var(CLOSURE_ENV_PARAM);
}


unsigned int ClosureLit::order() { 
    return ctype_to_pi(type())->order(); 
}

/* Closure Conversion */

void ClosureConv::run() {
    auto& w = world();
    auto externals = std::vector(w.externals().begin(), w.externals().end());
    auto subst = Def2Def();
    w.DLOG("===== ClosureConv: start =====");
    for (auto [_, ext_def]: externals) {
        rewrite(ext_def, subst);
    }
    while (!worklist_.empty()) {
        auto def = worklist_.front();
        subst = Def2Def();
        worklist_.pop();
        if (auto closure = closures_.lookup(def)) {
            rewrite_body(closure->fn, subst);
        } else {
            w.DLOG("RUN: rewrite def {}", def);
            rewrite(def, subst);
        }
    }
    w.DLOG("===== ClosureConv: done ======");
}

void ClosureConv::rewrite_body(Lam* new_lam, Def2Def& subst) {
    auto& w = world();
    auto stub = closures_.lookup(new_lam);
    assert(stub && "closure should have a stub if rewrite_body is called!");
    auto [old_fn, num_fvs, env, new_fn] = *stub;

    w.DLOG("rw body: {} [old={}, env={}]\nt", new_fn, old_fn, env);
    auto env_param = new_fn->var(CLOSURE_ENV_PARAM);
    if (num_fvs == 1) {
        subst.emplace(env, env_param);
    } else {
        for (size_t i = 0; i < num_fvs; i++) {
            auto dbg = w.dbg("fv_" + std::to_string(i));
            subst.emplace(env->op(i), env_param->proj(i, dbg));
        }
    }

    auto params =
        w.tuple(DefArray(old_fn->num_doms(), [&] (auto i) {
            return new_lam->var(skip_env(i));
        }), w.dbg("param"));
    subst.emplace(old_fn->var(), params);

    auto filter = (new_fn->filter())
        ? rewrite(new_fn->filter(), subst)
        : nullptr; // extern function

    auto body = (new_fn->body())
        ? rewrite(new_fn->body(), subst)
        : nullptr;

    new_fn->set_body(body);
    new_fn->set_filter(filter);
}

const Def* ClosureConv::rewrite(const Def* def, Def2Def& subst, CA ca) {
    switch(def->node()) {
        case Node::Kind:
        case Node::Space:
        case Node::Nat:
        case Node::Bot: // TODO This is used by the AD stuff????
        case Node::Top:
            return def;
        default:
            break;
    }

    auto& w = world();
    auto map = [&](const Def* new_def) {
        subst[def] = new_def;
        assert(subst[def] == new_def);
        return new_def;
    };

    if (auto new_def = subst.lookup(def)) {
        return *new_def;
    } else if (auto pi = def->isa<Pi>(); pi && pi->is_cn()) {
        return map(closure_type(pi, subst));
    } else if (auto lam = def->isa_nom<Lam>(); lam && lam->type()->is_cn()) {
        return map(make_closure(lam, subst, ca));
    } else if (auto [marked, v] = ca_isa_mark(def); marked) {
        return  map((v == CA::ret) ? rw_non_captured(marked, subst, v) : rewrite(marked, subst, v));
    } 

    auto new_type = rewrite(def->type(), subst);
    auto new_dbg = (def->dbg()) ? rewrite(def->dbg(), subst) : nullptr;

    if (auto nom = def->isa_nom()) {
        assert(!isa_ctype(nom));
        w.DLOG("RW: nom {}", nom);
        auto new_nom = nom->stub(w, new_type, new_dbg);
        subst.emplace(nom, new_nom);
        for (size_t i = 0; i < nom->num_ops(); i++) {
            if (def->op(i))
                new_nom->set(i, rewrite(def->op(i), subst));
        }
        if (Checker(w).equiv(nom, new_nom))
            return map(nom);
        if (auto restruct = new_nom->restructure())
            return map(restruct);
        return map(new_nom);
    } else {
        auto new_ops = DefArray(def->num_ops(), [&](auto i) {
            return rewrite(def->op(i), subst);
        });
        if (auto app = def->isa<App>(); app && new_ops[0]->type()->isa<Sigma>())
            return map(apply_closure(new_ops[0], new_ops[1]));
        else 
            return map(def->rebuild(w, new_type, new_ops, new_dbg));
    }
}

const Def* ClosureConv::closure_type(const Pi* pi, Def2Def& subst, const Def* env_type) {
    auto& w = world();
    auto rew = [&](auto d) { return rewrite(d, subst); };
    if (!env_type) {
        if (auto pct = closure_types_.lookup(pi))
            return* pct;
        auto sigma = ctype(w, pi, nullptr, rew);
        closure_types_.emplace(pi, sigma);
        w.DLOG("C-TYPE: pct {} ~~> {}", pi, sigma);
        return sigma;
    } else {
        auto new_pi = ctype(w, pi, env_type, rew);
        w.DLOG("C-TYPE: ct {}, env = {} ~~> {}", pi, env_type, new_pi);
        return new_pi;
    }
}

const Def* ClosureConv::rw_non_captured(const Def* def, Def2Def& subst, CA ca) {
    if (auto proj = def->isa<Extract>()) {
        if (auto var = proj->tuple()->isa<Var>()) {
            auto old_lam = var->nom()->as_nom<Lam>();
            auto index = as_lit(proj->index());
            auto new_lam = make_stub(old_lam, subst, true).fn;
            return new_lam->var(skip_env(index));
        }
    }
    return rewrite(def, subst, ca);
}

ClosureConv::ClosureStub ClosureConv::make_stub(Lam* old_lam, Def2Def& subst, bool convert) {
    if (auto closure = closures_.lookup(old_lam))
        return *closure;
    auto& w = world();
    size_t num_fvs;
    const Def* env;
    if (convert) {
        auto& fvs = fva_.run(old_lam);
        env = w.tuple(DefArray(fvs.begin(), fvs.end()));
        num_fvs = fvs.size();
    } else {
        env = w.tuple();
        num_fvs = 0;
    }
    auto env_type = rewrite(env->type(), subst);
    auto new_fn_type = closure_type(old_lam->type(), subst, env_type)->as<Pi>();
    auto new_lam = old_lam->stub(w, new_fn_type, w.dbg(old_lam->name()));
    new_lam->set_name(old_lam->name());
    new_lam->set_body(old_lam->body());
    new_lam->set_filter(old_lam->filter());
    if (old_lam->is_external()) {
        assert(convert && "External def must be closure converted!");
        old_lam->make_internal();
        new_lam->make_external();
    }
    w.DLOG("STUB {} ~~> ({}, {})", old_lam, env, new_lam);
    auto closure = ClosureStub{old_lam, num_fvs, env, new_lam};
    closures_.emplace(old_lam, closure);
    closures_.emplace(new_lam, closure);
    if (convert)
        worklist_.emplace(new_lam);
    else
        rewrite_body(new_lam, subst);
    return closure;
}

const Def* ClosureConv::make_closure(Lam *old_lam, Def2Def& subst, CA ca) {
    auto& w = world();
    auto [_, __, fv_env, new_lam] = make_stub(old_lam, subst, convert_lam(ca));
    auto closure_type = rewrite(old_lam->type(), subst);
    auto env = rewrite(fv_env, subst);
    auto closure = pack_closure(env, keep_annots_ ? w.ca_mark(new_lam, ca) : new_lam, closure_type);
    w.DLOG("RW: pack {} ~> {} : {}", old_lam, closure, closure_type);
    return closure;
}

/* Free variable analysis */

void FVA::split_fv(Def *nom, const Def* def, DefSet& out) {
    if (auto [marked, v] = ca_isa_mark(def); marked) {
        if (auto [var, _] = ca_isa_var<Lam>(marked); var && v == CA::ret)
            return;
        else
            def = marked;
    }
    if (def->no_dep() || def->isa<Global>() || def->isa<Axiom>() || def->isa_nom()) {
        return;
    } else if (def->dep() == Dep::Var && !def->isa<Tuple>()) {
        out.emplace(def);
    } else {
        for (auto op: def->ops())
            split_fv(nom, op, out);
    }
}

std::pair<FVA::Node*, bool> FVA::build_node(Def *nom, NodeQueue& worklist) {
    auto& w = world();
    auto [p, inserted] = lam2nodes_.emplace(nom, nullptr);
    if (!inserted)
        return {p->second.get(), false};
    // w.DLOG("FVA: create node: {}", nom);
    p->second = std::make_unique<Node>();
    auto node = p->second.get();
    node->nom = nom;
    node->pass_id = 0;
    auto scope = Scope(nom);
    node->fvs = DefSet();
    for (auto v: scope.free_defs()) {
        split_fv(nom, v, node->fvs);
    }
    node->preds = Nodes();
    node->succs = Nodes();
    bool init_node = false;
    for (auto pred: scope.free_noms()) {
        if (pred != nom) {
            auto [pnode, inserted] = build_node(pred, worklist);
            node->preds.push_back(pnode);
            pnode->succs.push_back(node);
            init_node |= inserted;
        }
    }
    if (!init_node) {
        worklist.push(node);
        // w.DLOG("FVA: init {}", nom);
    }
    return {node, true};
}

void FVA::run(NodeQueue& worklist) {
    auto& w = world();
    int iter = 0;
    while(!worklist.empty()) {
        auto node = worklist.front();
        worklist.pop();
        // w.DLOG("FA: iter {}: {}", iter, node->nom);
        if (is_done(node))
            continue;
        auto changed = is_bot(node);
        mark(node);
        for (auto p: node->preds) {
            auto& pfvs = p->fvs;
            changed |= node->fvs.insert(pfvs.begin(), pfvs.end());
            // w.DLOG("\tFV({}) ∪= FV({}) = {{{, }}}\b", node->nom, p->nom, pfvs);
        }
        if (changed) {
            for (auto s: node->succs) {
                worklist.push(s);
            }
        }
        iter++;
    }
    // w.DLOG("FVA: done");
}

DefSet& FVA::run(Lam *lam) {
    auto worklist = NodeQueue();
    auto [node, _] = build_node(lam, worklist);
    if (!is_done(node)) {
        cur_pass_id++;
        run(worklist);
    }
    return node->fvs;
}

} // namespace thorin
