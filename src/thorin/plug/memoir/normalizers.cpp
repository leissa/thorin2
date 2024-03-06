#include "thorin/world.h"

#include "thorin/plug/memoir/memoir.h"

namespace thorin::plug::memoir {

Ref normalize_assoc(Ref type, Ref callee, Ref arg) {
    auto& world = type->world();
    return world.raw_app(type, callee, arg);
}

Ref normalize_read(Ref type, Ref callee, Ref arg) {
    auto& world = type->world();
    auto [c, k] = arg->projs<2>();

    if (auto assoc = match<memoir::assoc>(c)) {
        if (auto tuple = assoc->arg()->isa<Tuple>()) {
            for (auto kv : tuple->ops()) {
                if (kv->proj(2, 0) == k) return kv->proj(2, 1);
            }
        }
    }
    return world.raw_app(type, callee, arg);
}

Ref normalize_write(Ref type, Ref callee, Ref arg) {
    auto& world    = type->world();
    auto [c, k, v] = arg->projs<3>();

    if (auto assoc = match<memoir::assoc>(c)) {
        if (auto tuple = assoc->arg()->isa<Tuple>()) {
            auto n  = assoc->decurry()->arg();
            auto KV = assoc->decurry()->decurry()->arg();
            if (auto l = Lit::isa(n)) {
                DefVec new_ops;
                bool updated = false;
                for (size_t i = 0, e = *l; i != e; ++i) {
                    auto kv = tuple->proj(e, i);
                    if (kv->proj(2, 0) == k) {
                        updated = true;
                        new_ops.emplace_back(world.tuple({k, v}));
                    } else {
                        new_ops.emplace_back(tuple->proj(e, i));
                    }
                }
                if (!updated) new_ops.emplace_back(world.tuple({k, v}));
                return world.call<memoir::assoc>(KV, new_ops.size(), Defs(new_ops));
            }
        }
    }

    return world.raw_app(type, callee, arg);
}

Ref normalize_has(Ref type, Ref callee, Ref arg) {
    auto& world = type->world();
    auto [c, k] = arg->projs<2>();
    if (auto assoc = match<memoir::assoc>(c)) {
        if (auto tuple = assoc->arg()->isa<Tuple>()) {
            for (auto kv : tuple->ops()) {
                if (kv->proj(2, 0) == k) return world.lit_tt();
            }
        }
    }
    return world.raw_app(type, callee, arg);
}

Ref normalize_size(Ref type, Ref callee, Ref arg) {
    auto& world = type->world();
    if (auto assoc = match<memoir::assoc>(arg)) return assoc->decurry()->arg();
    return world.raw_app(type, callee, arg);
}

Ref normalize_keys(Ref type, Ref callee, Ref arg) {
    auto& world = type->world();
    // TODO
    return world.raw_app(type, callee, arg);
}

THORIN_memoir_NORMALIZER_IMPL

} // namespace thorin::plug::memoir
