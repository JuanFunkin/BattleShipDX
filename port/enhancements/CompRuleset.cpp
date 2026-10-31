#include "enhancements.h"
#include <libultraship/bridge/consolevariablebridge.h>

namespace ssb64 {
    namespace enhancements {

        const char* CompRulesetCVarName() {
            return "gEnhancements.CompRuleset";
        }

    } // namespace enhancements
} // namespace ssb64

extern "C" int np_setting_override(int bit, int local);
extern "C" int port_get_comp_ruleset(void) {
    // Reads the CVar from the UI bridge.
    // The != 0 ensures it strictly returns a 1 or 0 boolean state for the C engine.
    return np_setting_override(0, CVarGetInteger(ssb64::enhancements::CompRulesetCVarName(), 0) != 0) != 0;
}
