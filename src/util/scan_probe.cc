#include "util/scan_probe.h"

#ifdef STRATA_SCAN_PROBE
namespace strata {
namespace probe {

ScanCounters& counters() {
    static ScanCounters c;
    return c;
}

} // namespace probe
} // namespace strata
#endif
