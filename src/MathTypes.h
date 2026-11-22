// ManualReloadF: the game's math types. The plugin takes them from the whole of CommonLibF4; the unit tests
// (MRF_UNIT_TESTS) take only NiPoint/NiMatrix3/NiTransform, since the full headers make address-library lookups
// at start-up that need the game.
#pragma once

#if defined(MRF_UNIT_TESTS)
#    include <cmath>
#    include <cstddef>
#    include <format>
#    include <memory>
#    include "RE/NetImmerse/NiPoint.h"
#    include "RE/NetImmerse/NiMatrix3.h"
#    include "RE/NetImmerse/NiTransform.h"
#else
#    include <RE/Fallout.h>
#endif
