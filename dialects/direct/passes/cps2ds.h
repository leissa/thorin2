#pragma once

#include <thorin/def.h>
#include <thorin/pass/pass.h>

namespace thorin::direct {

/// second part of ds2cps
/// replaces all ds call sites of cps (or ds converted) functions
/// with cps calls
/// b = f args
/// becomes
/// f (args,cont)
/// cont : cn b
class CPS2DS : public RWPass<CPS2DS, Lam> {
public:
    CPS2DS(PassMan& man)
        : RWPass(man, "cps2ds") {}

    void enter() override;

private:
    Def2Def rewritten_lams;
    Def2Def rewritten_;
    Lam* curr_lam_ = nullptr;

    void rewrite_lam(Lam* lam);
    const Def* rewrite_body(const Def*);
    const Def* rewrite_body_(const Def*);
};

} // namespace thorin::direct
