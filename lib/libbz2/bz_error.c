// bz_internal_error implementation for BZ_NO_STDIO builds
// This function is called when bzip2 detects an internal error

#include "bzlib.h"

void bz_internal_error(int errcode) {
    // In embedded systems, we let the error propagate through return codes
    // The calling code checks return values and handles errors appropriately
    (void)errcode;  // Unused parameter
}
