#include "Allocator.h"

#include "Platform/IBPlatform.h"

#include <assert.h>

namespace
{
    constexpr uint32_t SmallMemoryBoundary = 512;
    constexpr uint64_t NoSlot = 0xFFFFFFFFFFFFFFFF;

    // TODO: Document approach

    // BlockCount * 1/8 + BlockCount * BlockSize = PageSize
    // (BlockCount)(1/8 + BlockSize) = PageSize
    // BlockCount = PageSize*8/(1 + 8*BlockSize)

    struct PageTable
    {
        void *Header = nullptr;
        void *MemoryPages = nullptr;
    };

    struct PageRange
    {
        uintptr_t Start;
        uintptr_t End;
    };

    PageTable SmallMemoryPageTables[SmallMemoryBoundary] = {};
    PageRange SmallMemoryPageRanges[SmallMemoryBoundary] = {};

    bool areAllSlotsSet(void *memory, uint64_t bitCount)
    {
        bool fullyAllocated = true;

        uint64_t *memoryIter = reinterpret_cast<uint64_t*>(memory);
        for (; static_cast<int32_t>(bitCount) > 0; memoryIter++, bitCount -= 64)
        {
            uint64_t value = *memoryIter;
            uint64_t testBitMask = ~(0xFFFFFFFFFFFFFFFF << (bitCount < 64 ? (bitCount % 64) : 0)); // Create a bitmask of all the bits we want to test
            if ((value & testBitMask) != testBitMask)                                              // If any of our bits were cleared
            {
                fullyAllocated = false;
                break;
            }
        }

        return fullyAllocated;
    }

    uint64_t findClearedSlot(void *memory, uint64_t bitCount)
    {
        uint64_t freeSlot = NoSlot;

        uint64_t *memoryIter = reinterpret_cast<uint64_t *>(memory);
        for (; static_cast<int32_t>(bitCount) > 0; memoryIter++, bitCount -= 64)
        {
            uint64_t value = *memoryIter;
            uint64_t testBitMask = ~(0xFFFFFFFFFFFFFFFF << (bitCount < 64 ? (bitCount % 64) : 0)); // Create a bitmask of all the bits we want to test
            if ((value & testBitMask) != testBitMask)                                              // If any of our bits were cleared
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
                value = ((value >> 1) & 0x5555555555555555) + (value & 0x5555555555555555);
                value = ((value >> 2) & 0x3333333333333333) + (value & 0x3333333333333333);
                value = ((value >> 4) & 0x0F0F0F0F0F0F0F0F) + (value & 0x0F0F0F0F0F0F0F0F);
                value = ((value >> 8) & 0x00FF00FF00FF00FF) + (value & 0x00FF00FF00FF00FF);
                value = ((value >> 16) & 0x0000FFFF0000FFFF) + (value & 0x0000FFFF0000FFFF);
                value = ((value >> 32) & 0x00000000FFFFFFFF) + (value & 0x00000000FFFFFFFF);

                ptrdiff_t chunkIndex = (memoryIter - reinterpret_cast<uint64_t*>(memory));
                freeSlot = chunkIndex * 64 + value - 1;
                break;
            }
        }

        return freeSlot;
    }

    void setSlot(void *memory, uint64_t index)
    {
        uint64_t *memoryIter = reinterpret_cast<uint64_t *>(memory);
        memoryIter = memoryIter + index / 64;
        *memoryIter |= 1ull << (index % 64);
    }

    void *getPageSlot(void *page, size_t blockSize, uint64_t blockCount, uint64_t slotIndex)
    {
        uintptr_t pageIter = reinterpret_cast<uintptr_t>(page);
        pageIter += blockCount / 8 + (blockCount % 8 > 0 ? 1 : 0);

        // Make sure we're aligned
        pageIter = pageIter + ((pageIter % blockSize) > 0 ? (blockSize - pageIter % blockSize) : 0);
        pageIter = pageIter + slotIndex * blockSize;

        assert(pageIter < reinterpret_cast<uintptr_t>(page) + IB::memoryPageSize());
        return reinterpret_cast<void *>(pageIter);
    }
} // namespace

namespace IB
{
    void *memoryAllocate(size_t size, size_t alignment)
    {
        assert(size != 0);
        if (size <= SmallMemoryBoundary && alignment <= SmallMemoryBoundary)
        {
            // If blockSize is larger than size, then we have internal fragmentation.
            // TODO: Log internal fragmentation
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
            assert(blockSize <= SmallMemoryBoundary);

            size_t tableIndex = blockSize - 1;
            // If our table hasn't been initialized, allocate a page for it
            if (SmallMemoryPageTables[tableIndex].MemoryPages == nullptr)
            {
                SmallMemoryPageTables[tableIndex].Header = IB::reserveMemoryPages(1);
                IB::commitMemoryPages(SmallMemoryPageTables[tableIndex].Header, 1);

                SmallMemoryPageTables[tableIndex].MemoryPages = IB::reserveMemoryPages(IB::memoryPageSize() * 8);

                uintptr_t start = reinterpret_cast<uintptr_t>(SmallMemoryPageTables[tableIndex].MemoryPages);
                uintptr_t end = start + IB::memoryPageSize() * 8 * IB::memoryPageSize();
                SmallMemoryPageRanges[tableIndex] = PageRange{ start, end };
            }

            // Find our free page address
            void *page;
            uint64_t pageCount = (IB::memoryPageSize() * 8);
            uint64_t pageIndex = findClearedSlot(SmallMemoryPageTables[tableIndex].Header, pageCount);
            assert(pageIndex != NoSlot); // We're out of memory pages for this size class! Improve this algorithm to support the use case.

            {
                uintptr_t pageAddress = reinterpret_cast<uintptr_t>(SmallMemoryPageTables[tableIndex].MemoryPages);
                pageAddress += pageAddress + IB::memoryPageSize() * pageIndex;
                page = reinterpret_cast<void*>(pageAddress);
                IB::commitMemoryPages(page, 1);
            }

            // Find our memory address
            void *memory;
            {
                uint64_t blockCount = (IB::memoryPageSize() * 8) / (1 + blockSize / 8);
                uint64_t freeSlot = findClearedSlot(page, blockCount);
                assert(freeSlot != NoSlot); // Our page's "fully allocated" bit was cleared.

                setSlot(page, freeSlot);
                if (areAllSlotsSet(page, blockCount))
                {
                    setSlot(SmallMemoryPageTables[tableIndex].Header, pageIndex);
                }

                memory = getPageSlot(page, blockSize, blockCount, freeSlot);
            }

            return memory;
        }

        // TODO: Large memory allocation
        return nullptr;
    }

    void memoryFree(void *memory)
    {
        uintptr_t memoryAddress = reinterpret_cast<uintptr_t>(memory);
        uint32_t memoryPageIndex = SmallMemoryBoundary;
        for (uint32_t i = 0; i < SmallMemoryBoundary; i++)
        {
            if (memoryAddress >= SmallMemoryPageRanges[i].Start && memoryAddress < SmallMemoryPageRanges[i].End)
            {
                memoryPageIndex = i;
                break;
            }
        }

        // Small memory allocation
        if (memoryPageIndex != SmallMemoryBoundary)
        {

        }
    }

} // namespace IB
