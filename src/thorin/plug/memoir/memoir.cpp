#include "thorin/plug/memoir/memoir.h"

#include <thorin/plugin.h>

#include <thorin/pass/pass.h>

using namespace thorin;

/// Registers Pass%es in the different optimization Phase%s as well as normalizers for the Axiom%s.
extern "C" THORIN_EXPORT Plugin thorin_get_plugin() {
    return {"memoir", [](Normalizers& normalizers) { plug::memoir::register_normalizers(normalizers); }, nullptr, nullptr};
}
