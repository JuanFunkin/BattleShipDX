#include "enhancements.h"
#include <libultraship/bridge/consolevariablebridge.h>

namespace ssb64 {
    namespace enhancements {

        const char* NeutralSpawnsCVarName() {
            return "gEnhancements.NeutralSpawns";
        }

    } // namespace enhancements
} // namespace ssb64

extern "C" int np_setting_override(int bit, int local);
extern "C" int port_enhancement_neutral_spawns(void) {
    return np_setting_override(1, CVarGetInteger(ssb64::enhancements::NeutralSpawnsCVarName(), 0) != 0) != 0;
}
