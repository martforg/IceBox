#pragma once

#include "IBEngineAPI.h"

#include <stdint.h>

namespace IB
{
    void *memoryAllocate(size_t size, size_t alignment);
    void memoryFree(void *memory);
} // namespace IB
