#pragma once

#include <Bytes.h>

namespace RNS { namespace Cryptography {

	const Bytes bz2_decompress(const Bytes& data);
	const Bytes bz2_compress(const Bytes& data);

} }
