#include "thorin/analyses/cfg.h"

#include <fstream>
#include <stack>

#include "thorin/primop.h"
#include "thorin/analyses/domfrontier.h"
#include "thorin/analyses/domtree.h"
#include "thorin/analyses/looptree.h"
#include "thorin/analyses/scope.h"
#include "thorin/be/ycomp.h"
#include "thorin/util/log.h"
#include "thorin/util/queue.h"

namespace thorin {

//------------------------------------------------------------------------------

uint32_t CFNodeBase::id_counter_ = 1;

typedef thorin::HashSet<const CFNodeBase*> CFNodeSet;

/// Any jumps targeting a @p Lambda or @p Param outside the @p CFA's underlying @p Scope target this node.
class OutNode : public RealCFNode {
public:
    OutNode(const CFNode* context, Def def)
        : RealCFNode(def)
        , context_(context)
    {
        assert(def->isa<Param>() || def->isa<Lambda>());
    }

    const CFNode* context() const { return context_; }
    const HashSet<const OutNode*>& ancestors() const { return ancestors_; }
    virtual std::ostream& stream(std::ostream&) const override;

private:
    const CFNode* context_;
    mutable HashSet<const OutNode*> ancestors_;

    friend class CFABuilder;
};

class SymNode : public CFNodeBase {
protected:
    SymNode(Def def)
        : CFNodeBase(def)
    {}
};

class SymDefNode : public SymNode {
public:
    SymDefNode(Def def)
        : SymNode(def)
    {
        assert(def->isa<Param>() || def->isa<Lambda>());
    }

    virtual std::ostream& stream(std::ostream&) const override;

private:
    Def def_;
};

class SymOutNode : public SymNode {
public:
    SymOutNode(const OutNode* out_node)
        : SymNode(out_node->def())
        , out_node_(out_node)
    {}

    const OutNode* out_node() const { return out_node_; }
    virtual std::ostream& stream(std::ostream&) const override;

private:
    const OutNode* out_node_;
};

void CFNode::link(const CFNode* other) const {
    this ->succs_.emplace(other);
    other->preds_.emplace(this);
}

std::ostream& CFNode::stream(std::ostream& out) const { return streamf(out, "%", lambda()); }
std::ostream& OutNode::stream(std::ostream& out) const { return streamf(out, "[Out: % (%)]", def(), context()); }
std::ostream& SymDefNode::stream(std::ostream& out) const { return streamf(out, "[Sym: %]", def()); }
std::ostream& SymOutNode::stream(std::ostream& out) const { return streamf(out, "[Sym: %]", out_node()); }

//------------------------------------------------------------------------------

class CFABuilder : public YComp {
public:
    CFABuilder(CFA& cfa)
        : YComp(cfa.scope(), "cfa")
        , cfa_(cfa)
        , lambda2param2nodes_(cfa.scope(), std::vector<CFNodeSet>(0))
        , entry_(in_node(scope().entry()))
        , exit_ (in_node(scope().exit()))
    {
        ILOG("*** CFA: %", scope().entry());
        ILOG_SCOPE(propagate_higher_order_values());
        ILOG_SCOPE(run_cfa());
        ILOG_SCOPE(build_cfg());
        ILOG_SCOPE(unreachable_node_elimination());
        ILOG_SCOPE(link_to_exit());
        ILOG_SCOPE(transitive_cfg());
#ifndef NDEBUG
        ILOG_SCOPE(verify());
#endif
    }

    ~CFABuilder() {
        for (const auto& p : out_nodes_) {
            for (const auto& q : p.second)
                delete q.second;
        }

        for (auto p : def2sym_) delete p.second;
        for (auto p : out2sym_) delete p.second;
    }

    void propagate_higher_order_values();
    void run_cfa();
    void build_cfg();
    void unreachable_node_elimination();
    void link_to_exit();
    void transitive_cfg();
    void verify();
    virtual void stream_ycomp(std::ostream& out) const override;

    const CFA& cfa() const { return cfa_; }
    const CFNode* entry() const { return entry_; }
    const CFNode* exit() const { return exit_; }

    CFNodeSet nodes(const CFNode*, size_t i);
    CFNodeSet to_nodes(const CFNode* in) { return in->lambda()->empty() ? CFNodeSet() : nodes(in, 0); }
    Array<CFNodeSet> arg_nodes(const CFNode*);
    bool contains(Lambda* lambda) { return scope().inner_contains(lambda); }
    bool contains(const Param* param) { return contains(param->lambda()); }

    const CFNode* in_node(Lambda* lambda) {
        assert(scope().outer_contains(lambda));
        if (auto in = find(cfa().nodes(), lambda))
            return in;
        auto in = cfa_.nodes_[lambda] = new CFNode(lambda);
        lambda2param2nodes_[lambda].resize(lambda->num_params()); // make room for params
        return in;
    }

    const OutNode* out_node(const CFNode* in, Def def) {
        if (auto out = find(out_nodes_[in], def))
            return out;
        return out_nodes_[in][def] = new OutNode(in, def);
    }

    const OutNode* out_node(const CFNode* in, const OutNode* ancestor) {
        auto out = out_node(in, ancestor->def());
        out->ancestors_.insert(ancestor);
        return out;
    }

    const OutNode* out_node(const CFNode* in, const SymNode* sym) {
        if (auto sym_def = sym->isa<SymDefNode>())
            return out_node(in, sym_def->def());
        return out_node(in, sym->as<SymOutNode>()->out_node());
    }

    const SymNode* sym_node(Def def) {
        if (auto sym = find(def2sym_, def))
            return sym;
        return def2sym_[def] = new SymDefNode(def);
    }

    const SymNode* sym_node(const OutNode* out) {
        if (auto sym = find(out2sym_, out))
            return sym;
        return out2sym_[out] = new SymOutNode(out);
    }

    CFNodeSet& param2nodes(const Param* param) {
        in_node(param->lambda()); // alloc CFNode and make room in lambda2param2nodes_
        return lambda2param2nodes_[param->lambda()][param->index()];
    }

    void link(const RealCFNode* src, const RealCFNode* dst) {
        DLOG("% -> %", src, dst);

        assert(src->f_index_ == CFNode::Reachable || src->f_index_ == CFNode::Done);
        dst->f_index_ = src->f_index_;

        const auto& p = succs_[src].insert(dst);
        const auto& q = preds_[dst].insert(src);

        // recursively link ancestors
        if (p.second) {
            assert_unused(q.second);
            if (auto out = dst->isa<OutNode>()) {
                for (auto ancestor : out->ancestors())
                    link(out, ancestor);
            }
        }
    }

private:
    CFA& cfa_;
    Scope::Map<std::vector<CFNodeSet>> lambda2param2nodes_; ///< Maps param in scope to CFNodeSet.
    DefMap<DefSet> def2set_;
    HashMap<const RealCFNode*, HashSet<const RealCFNode*>> succs_;
    HashMap<const RealCFNode*, HashSet<const RealCFNode*>> preds_;
    const CFNode* entry_;
    const CFNode* exit_;
    size_t num_out_nodes_ = 0;
    mutable HashMap<const CFNode*, DefMap<const OutNode*>> out_nodes_;
    mutable DefMap<const SymNode*> def2sym_;
    mutable HashMap<const OutNode*, const SymNode*> out2sym_;
};

void CFABuilder::propagate_higher_order_values() {
    std::stack<Def> stack;

    auto push = [&] (Def def) -> bool {
        if (def->order() > 0) {
            const auto& p = def2set_.emplace(def, DefSet());
            if (p.second) { // if first insert
                stack.push(def);
                return true;
            }
        }
        return false;
    };

    for (auto lambda : scope()) {
        for (auto op : lambda->ops()) {
            push(op);

            while (!stack.empty()) {
                auto def = stack.top();
                assert(def->order() > 0);
                auto& set = def2set_[def];

                if (def->isa<Param>() || def->isa<Lambda>()) {
                    set.insert(def);
                    stack.pop();
                } else {
                    if (auto load = def->isa<Load>()) {
                        if (load->type()->order() >= 1)
                            WLOG("higher-order load not yet supported");
                    }
                    bool todo = false;
                    for (auto op : def->as<PrimOp>()->ops())
                        todo |= push(op);
                    if (!todo) {
                        for (auto op : def->as<PrimOp>()->ops())
                            set.insert_range(def2set_[op]);
                        stack.pop();
                    }
                }
            }
        }
    }
}

CFNodeSet CFABuilder::nodes(const CFNode* in, size_t i) {
    CFNodeSet result;
    auto cur_lambda = in->lambda();
    auto op = cur_lambda->op(i);

    if (op->order() > 0) {
        auto iter = def2set_.find(cur_lambda->op(i));
        assert(iter != def2set_.end());

        for (auto def : iter->second) {
            assert(def->order() > 0);

            if (auto lambda = def->isa_lambda()) {
                if (contains(lambda))
                    result.insert(in_node(lambda));
                else if (i == 0)
                    result.insert(out_node(in, lambda));
                else
                    result.insert(sym_node(lambda));
            } else {
                auto param = def->as<Param>();
                if (contains(param)) {
                    const auto& set = param2nodes(param);
                    for (auto n : set) {
                        if (auto sym = n->isa<SymNode>()) {
                            if (i == 0) {
                                result.insert(out_node(in, sym));
                                continue;
                            }
                        } else if (auto out = n->isa<OutNode>()) {
                            if (i == 0)
                                result.insert(out_node(in, out)); // create a new context
                            else
                                result.insert(sym_node(out));
                            continue;
                        }

                        result.insert(n);
                    }
                } else {
                    if (i == 0)
                        result.insert(out_node(in, param));
                    else
                        result.insert(sym_node(param));
                }
            }
        }
    }

    return result;
}

Array<CFNodeSet> CFABuilder::arg_nodes(const CFNode* in) {
    Array<CFNodeSet> result(in->lambda()->num_args());
    for (size_t i = 0; i != result.size(); ++i)
        result[i] = nodes(in, i+1); // shift by one due to args/ops discrepancy
    return result;
}

void CFABuilder::run_cfa() {
    std::queue<Lambda*> queue;

    auto enqueue = [&] (const CFNode* in) {
        queue.push(in->lambda());
        in->f_index_ = CFNode::Unfresh;
    };

    enqueue(entry());

    while (!queue.empty()) {
        auto cur_lambda = pop(queue);
        auto cur_in = in_node(cur_lambda);
        auto args = arg_nodes(cur_in);

        for (auto n : to_nodes(cur_in)) {
            if (n->def()->type() != cur_lambda->to()->type())
                continue;

            if (auto in = n->isa<CFNode>()) {
                bool todo = in->f_index_ == CFNode::Fresh;
                for (size_t i = 0; i != cur_lambda->num_args(); ++i) {
                    if (in->lambda()->param(i)->order() > 0)
                        todo |= lambda2param2nodes_[in->lambda()][i].insert_range(args[i]);
                }
                if (todo)
                    enqueue(in);

            } else {
                auto out = n->as<OutNode>();
                assert(in_node(cur_lambda) == out->context() && "OutNode's context does not match");

                for (const auto& nodes : args) {
                    for (auto n : nodes) {
                        if (auto in = n->isa<CFNode>()) {
                            bool todo = in->f_index_ == CFNode::Fresh;
                            for (size_t i = 0; i != in->lambda()->num_params(); ++i) {
                                if (in->lambda()->param(i)->order() > 0)
                                    todo |= lambda2param2nodes_[in->lambda()][i].insert(out).second;
                            }
                            if (todo)
                                enqueue(in);
                        }
                    }
                }
            }
        }
    }
}

void CFABuilder::build_cfg() {
    std::queue<const CFNode*> queue;

    auto enqueue = [&] (const CFNode* in) {
        if (in->f_index_ != CFNode::Reachable) {
            queue.push(in);
        }
    };

    queue.push(entry());
    entry()->f_index_ = exit()->f_index_ = CFNode::Reachable;

    while (!queue.empty()) {
        auto cur_in = pop(queue);
        for (auto n : to_nodes(cur_in)) {
            if (auto in = n->isa<CFNode>()) {
                enqueue(in);
                link(cur_in, in);
            } else {
                auto out = n->as<OutNode>();
                link(cur_in, out);
                for (const auto& nodes : arg_nodes(cur_in)) {
                    for (auto n : nodes) {
                        if (auto in = n->isa<CFNode>()) {
                            enqueue(in);
                            link(out, in);
                        }
                    }
                }
            }
        }
    }
}

void CFABuilder::unreachable_node_elimination() {
    for (auto in : cfa().nodes()) {
        if (in->f_index_ == CFNode::Reachable) {
            ++cfa_.size_;

            auto& out_nodes = out_nodes_[in];
            for (auto i = out_nodes.begin(); i != out_nodes.end();) {
                auto out = i->second;
                if (out->f_index_ == CFNode::Reachable) {
                    ++i;
                    ++num_out_nodes_;
                } else {
                    DLOG("removing: %", out);
                    i = out_nodes.erase(i);
                    delete out;
                }
            }
        } else {
#ifndef NDEBUG
            for (const auto& p : out_nodes_[in])
                assert(p.second->f_index_ != CFNode::Reachable);
#endif
            DLOG("removing: %", in);
            cfa_.nodes_[in->lambda()] = nullptr;
            delete in;
        }
    }
}

void CFABuilder::link_to_exit() {
    auto backwards_reachable = [&] (const RealCFNode* n) {
        std::queue<const RealCFNode*> queue;

        auto enqueue = [&] (const RealCFNode* n) {
            if (n->b_index_ != CFNode::Done) {
                n->b_index_ = CFNode::Done;
                queue.push(n);
            }
        };

        enqueue(n);

        while (!queue.empty()) {
            for (auto pred : preds_[pop(queue)])
                enqueue(pred);
        }
    };

    std::vector<const RealCFNode*> stack;

    auto push = [&] (const RealCFNode* n) {
        if (n->f_index_ == CFNode::Reachable) {
            n->f_index_ = CFNode::OnStackTodo;
            stack.push_back(n);
        }
    };

    auto backtrack = [&] () {
        static size_t mark = 0;
        std::stack<const RealCFNode*> backtrack_stack;
        const RealCFNode* candidate = nullptr;

        auto push = [&] (const RealCFNode* n) {
            assert(int(n->b_index_) >= -1 || n->b_index_ == CFNode::Done);

            if ((n->f_index_ == CFNode::OnStackTodo || n->f_index_ == CFNode::OnStackReady) && n->b_index_ != mark) {
                n->b_index_ = mark;
                backtrack_stack.push(n);
                return true;
            }
            return false;
        };

        backtrack_stack.push(stack.back());

        while (!backtrack_stack.empty()) {
            auto n = backtrack_stack.top();

            bool todo = false;
            for (auto succ : succs_[n])
                todo |= push(succ);

            if (!todo) {
                if (n->f_index_ == CFNode::OnStackTodo) {
                    candidate = n;
                    DLOG("candidate: %", candidate);
                }
                backtrack_stack.pop();
            }
        }

        ++mark;

        if (candidate) { // reorder stack
            DLOG("reorder for candidate: %", candidate);
            auto i = std::find(stack.begin(), stack.end(), candidate);
            assert(i != stack.end());
            std::move(i+1, stack.end(), i);
            stack.back() = candidate;
            return true;
        }
        return false;
    };

    backwards_reachable(exit());
    push(entry());

    while (!stack.empty()) {
        auto n = stack.back();

        if (n->f_index_ == CFNode::OnStackTodo) {
            n->f_index_ = CFNode::OnStackReady;
            for (auto succ : succs_[n])
                push(succ);
        } else {
            if (n->b_index_ != CFNode::Done) {
                if (!backtrack()) {
                    n->f_index_ = CFNode::Done;
                    stack.pop_back();
                    DLOG("unreachble from exit: %", n);
                    link(n, exit());
                    backwards_reachable(n);
                }
            } else {
                n->f_index_ = CFNode::Done;
                stack.pop_back();
            }
        }
    }
}

void CFABuilder::transitive_cfg() {
    std::queue<const RealCFNode*> queue;

    auto link_to_succs = [&] (const CFNode* src) {
        auto enqueue = [&] (const RealCFNode* n) {
            for (auto succ : succs_.find(n)->second)
                queue.push(succ);
        };

        enqueue(src);

        while (!queue.empty()) {
            auto n = pop(queue);
            if (auto dst = n->isa<CFNode>())
                src->link(dst);
            else
                enqueue(n);
        }
    };

    for (const auto& p : succs_) {
        if (auto in = p.first->isa<CFNode>())
            link_to_succs(in);
    }
}

void CFABuilder::verify() {
    bool error = false;
    for (auto in : cfa().nodes()) {
        if (in != entry() && in->preds_.size() == 0) {
            WLOG("missing predecessors: %", in->lambda());
            error = true;
        }
    }

    if (error) {
        ycomp();
        abort();
    }
}

void CFABuilder::stream_ycomp(std::ostream& out) const {
    std::vector<const RealCFNode*> nodes(cfa().nodes().begin(), cfa().nodes().end());
    for (const auto& p : out_nodes_) {
        for (const auto& q : p.second)
            nodes.push_back(q.second);
    }

    auto succs = [&] (const RealCFNode* n) {
        auto i = succs_.find(n);
        return i != succs_.end() ? i->second : HashSet<const RealCFNode*>();
    };

    thorin::ycomp(out, YCompOrientation::TopToBottom, scope(), range(nodes), succs);
}

//------------------------------------------------------------------------------

CFA::CFA(const Scope& scope)
    : scope_(scope)
    , nodes_(scope)
{
    CFABuilder cfa(*this);
}

CFA::~CFA() {
    for (auto n : nodes_.array()) delete n;
}

const F_CFG& CFA::f_cfg() const { return lazy_init(this, f_cfg_); }
const B_CFG& CFA::b_cfg() const { return lazy_init(this, b_cfg_); }

//------------------------------------------------------------------------------

template<bool forward>
CFG<forward>::CFG(const CFA& cfa)
    : YComp(cfa.scope(), forward ? "f_cfg" : "b_cfg")
    , cfa_(cfa)
    , rpo_(*this)
{
#ifndef NDEBUG
    assert(post_order_visit(entry(), size()) == 0);
#else
    post_order_visit(entry(), size());
#endif
}

template<bool forward>
size_t CFG<forward>::post_order_visit(const CFNode* n, size_t i) {
    auto& n_index = forward ? n->f_index_ : n->b_index_;
    assert(n_index == CFNode::Done);
    n_index = CFNode::Visited;

    for (auto succ : succs(n)) {
        if (index(succ) == CFNode::Done)
            i = post_order_visit(succ, i);
    }

    n_index = i-1;
    rpo_[n] = n;
    return n_index;
}

template<bool forward>
void CFG<forward>::stream_ycomp(std::ostream& out) const {
    thorin::ycomp(out, YCompOrientation::TopToBottom, scope(), range(reverse_post_order()),
        [&] (const CFNode* n) { return range(succs(n)); }
    );
}

template<bool forward> const DomTreeBase<forward>& CFG<forward>::domtree() const { return lazy_init(this, domtree_); }
template<bool forward> const LoopTree<forward>& CFG<forward>::looptree() const { return lazy_init(this, looptree_); }
template<bool forward> const DomFrontierBase<forward>& CFG<forward>::domfrontier() const { return lazy_init(this, domfrontier_); }

template class CFG<true>;
template class CFG<false>;

//------------------------------------------------------------------------------

}
