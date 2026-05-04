// Compatibility header — when HDLC.h (or other pyxis headers) does
// `#include "Bytes.h"`, this is found first via -I tests/native, supplying
// the shim instead of the real microReticulum Bytes.h. See bytes_shim.h.
#pragma once
#include "bytes_shim.h"
