#include "Allocator.h"

#include <assert.h>

namespace
{
    constexpr uint32_t SmallMemoryBoundary = 512;
    constexpr uint32_t SmallMemoryPageCount = SmallMemoryBoundary / 4 + 2; // Multiples of 4 + 1 byte and 2 bytes
    constexpr uint64_t NoSlot = 0xFFFFFFFFFFFFFFFF;
    constexpr uint64_t HeaderSize = 8;
    static_assert(HeaderSize == sizeof(uintptr_t));

    // Pages are stored in this format:
    // Pointer To Next Page
    // Allocation Bits
    // Data Block Memory

    // BlockCount * 1/8 + BlockCount * BlockSize = PageSize - HeaderSize
    // (BlockCount)(1/8 + BlockSize) = PageSize - HeaderSize
    // BlockCount = (PageSize - HeaderSize)*8/(1 + 8*BlockSize)
    void* SmallMemoryPages[SmallMemoryPageCount] = {};

    uint32_t getMemoryIndex(size_t size)
    {
        if (size < 4)
        {
            assert(size == 1 || size == 2);
            return size - 1;
        }
        else
        {
            // Index is size / 4 + 1 if mod 3 != 0 + 1 to get up to index 2 (0, 1 taken by 1 and 2 byte allocs)
            return (size >> 2) + ((size & 3) > 0 ? 1 : 0) + 1;
        }
    }

    uint64_t findFreeSlot(void* page, size_t blockSize)
    {
        uint64_t freeSlot = NoSlot;

        uint64_t* pageIter = reinterpret_cast<uint64_t*>(page);
        pageIter++; // Ignore our header pointer

        uint32_t blockCount = ((IB::memoryPageSize() - HeaderSize) >> 3) / (1 + (blockSize >> 3));
        uint64_t pageEnd = pageIter + (blockCount >> 6);
        for (; pageIter != pageEnd; pagerIter++)
        {
            uint64_t value = *pageIter;
            uint64_t testBitMask = ~(0xFFFFFFFFFFFFFFFF << (blockCount & 63)); // Create a bitmask of all the bits we want to test
            if ((value & testBitMask) != testBitMask) // If any of our bits were cleared
            {
                // Mask out all our set bits and the first non-set bit
                // (0010 + 1) ^ 0010 = 0011 ^ 0010 = 0001
                // (0011 + 1) ^ 0011 = 0100 ^ 0011 = 0111
                // (1011 + 1) ^ 1011 = 1100 ^ 1011 = 0111
                // (0111 + 1) ^ 0111 = 1000 ^ 0111 = 1111
                value = value ^ (value + 1);

                // At this point, the number of bits set is equal to the index of our first cleared bit + 1
                // Calculate the number of set bits and subtract 1 to get it's index

                // Count the number of set bits with a parallel add
                // We first start with the first pair of bits
                // 01 10 11 00
                // We add up the pairs
                // 01 01 10 00
                // Then we add them up the pairs in a quad
                // 0101 1000
                // 0010 0010
                // Then we add up the quads in octets
                // 00100010
                // 00000100 = 4
                value = ((value >> 1)  & 0x5555555555555555) + (value & 0x5555555555555555);
                value = ((value >> 2)  & 0x3333333333333333) + (value & 0x3333333333333333);
                value = ((value >> 4)  & 0x0F0F0F0F0F0F0F0F) + (value & 0x0F0F0F0F0F0F0F0F);
                value = ((value >> 8)  & 0x00FF00FF00FF00FF) + (value & 0x00FF00FF00FF00FF);
                value = ((value >> 16) & 0x0000FFFF0000FFFF) + (value & 0x0000FFFF0000FFFF);
                value = ((value >> 32) & 0x00000000FFFFFFFF) + (value & 0x00000000FFFFFFFF);

                freeSlot = value - 1;
            }
        }

        return freeSlot;
    }

    void* getSlotMemory(void* page, size_t blockSize, uint64_t slotIndex)
    {
        uint32_t blockCount = ((IB::memoryPageSize() - HeaderSize) >> 3) / (1 + (blockSize >> 3));
        uintptr_t pageIter = reinterpret_cast<uintptr_t>(page);
        pageIter += HeaderSize + blockCount * blockSize;
        pageIter = pageIter + ((pageIter % blockSize) > 0 ? (blockSize - pageIter % blockSize) : 0);

        assert(pageIter < reinterpret_cast<uintptr_t>(page) + IB::memoryPageSize());
        return reinterpret_cast<void*>(pageIter);
    }
}

namespace IB
{
    void *memoryAllocate(size_t size, size_t alignment)
    {
        assert(size != 0);
        if (size < SmallMemoryBoundary && alignment < SmallMemoryBoundary)
        {
            size_t blockSize = size;
            if (size != alignment && size % alignment == 0)
            {
                if (size > alignment)
                {
                    blockSize = ((size / alignment) + 1) * alignment;
                }
                else
                {
                    blockSize = alignment;
                }
            }

            // If blockSize is larger than size, then we have internal fragmentation.
            // TODO: Log internal fragmentation
            uint32_t pageIndex = getMemoryIndex(blockSize);
            if (SmallMemoryPages[pageIndex] == nullptr)
            {
                SmallMemoryPages[pageIndex] = IB::reserveMemoryPage();
                IB::commitMemoryPage(SmallMemoryPages[pageIndex]);
            }

            // TODO: If we find that we spend a lot of time here. We can make sure that pages with
            // free memory are bumped to the front of the list.
            // TODO: We can also store our pointer chain in an auxiliary table instead of getting the pointers from each page.
            void* page = SmallMemoryPages[pageIndex];
            uint64_t freeSlot = findFreeSlot(page, blockSize);
            while (freeSlot == NoSlot)
            {
                void** nextPage = reinterpret_cast<void**>(page);
                if (*nextPage == nullptr)
                {
                    *nextPage = IB::reserveMemoryPage();
                    IB::commitMemoryPage(*nextPage);
                    freeSlot = 0;
                    page = *nextPage;
                }
                else
                {
                    // Try the next page
                    page = *nextPage;
                    freeSlot = findFreeSlot(page, blockSize);
                }
            }

            return getSlotMemory(page, blockSize, freeSlot);
        }
    }

    void memoryFree(void *memory)
    {

    }

} // namespace IB
