#include <IBEngine/IBAllocator.h>
#include <assert.h>

int main()
{
    // Small allocations
    {
        void *allocations[512];
        for (uint32_t i = 0; i < 512; i++)
        {
            allocations[i] = IB::memoryAllocate(i + 1, i + 1);
            assert(reinterpret_cast<uintptr_t>(allocations[i]) % (i + 1) == 0);
        }

        for (uint32_t i = 0; i < 512; i++)
        {
            IB::memoryFree(allocations[i]);
        }

        void *smallAlignedMemory = IB::memoryAllocate(4, 16);
        assert(reinterpret_cast<uintptr_t>(smallAlignedMemory) % 16 == 0);
        IB::memoryFree(smallAlignedMemory);

        void *largeAlignedMemory = IB::memoryAllocate(24, 16);
        assert(reinterpret_cast<uintptr_t>(largeAlignedMemory) % 16 == 0);
        IB::memoryFree(largeAlignedMemory);

        void *extraAlignedMemory = IB::memoryAllocate(33, 16);
        assert(reinterpret_cast<uintptr_t>(extraAlignedMemory) % 16 == 0);
        IB::memoryFree(extraAlignedMemory);

        void *sameSize1 = IB::memoryAllocate(4, 4);
        void *sameSize2 = IB::memoryAllocate(4, 4);
        IB::memoryFree(sameSize1);
        IB::memoryFree(sameSize2);

        // Allocate a lot of small allocations
        for (uint32_t loop = 0; loop < 10; loop++)
        {
            void *manySameSize[10000];
            for (uint32_t i = 0; i < 10000; i++)
            {
                manySameSize[i] = IB::memoryAllocate(4, 4);
            }

            for (uint32_t i = 0; i < 10000; i++)
            {
                IB::memoryFree(manySameSize[i]);
            }
        }
    }
}
