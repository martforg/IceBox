#pragma once

#include "IBEngineAPI.h"

#include <stdint.h>

namespace IB
{
    IB_API void *memoryAllocate(size_t size, size_t alignment);
    IB_API void memoryFree(void *memory);
} // namespace IB
