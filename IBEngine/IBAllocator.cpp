#include "IBAllocator.h"

#include "Platform/IBPlatform.h"

#include <assert.h>

namespace
{
    // Small Memory Allocations

    constexpr size_t SmallMemoryBoundary = 512;
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

    // Make our memory thread local to support multi-threaded access to the page tables.
    // Note that this is not necessarily the best approach to making our memory allocation
    // but is definitely the easiest.
    // If problems arise in the future, we might want to re-examine how we can make the
    // allocator threadsafe.
    thread_local PageTable SmallMemoryPageTables[SmallMemoryBoundary] = {};
    thread_local PageRange SmallMemoryPageRanges[SmallMemoryBoundary] = {};

    bool areAllSlotsSet(void *memory, uint64_t bitCount)
    {
        bool fullyAllocated = true;

        uint64_t *memoryIter = reinterpret_cast<uint64_t *>(memory);
        for (; static_cast<int32_t>(bitCount) > 0; memoryIter++, bitCount -= 64)
        {
            uint64_t value = *memoryIter;
            uint64_t testBitMask = ~(bitCount < 64 ? (0xFFFFFFFFFFFFFFFF << (bitCount % 64)) : 0); // Create a bitmask of all the bits we want to test
            // If any of our bits were cleared
            if ((value & testBitMask) != testBitMask)
            {
                fullyAllocated = false;
                break;
            }
        }

        return fullyAllocated;
    }

    bool areAllSlotsClear(void *memory, uint64_t bitCount)
    {
        bool fullyFree = true;

        uint64_t *memoryIter = reinterpret_cast<uint64_t *>(memory);
        for (; static_cast<int32_t>(bitCount) > 0; memoryIter++, bitCount -= 64)
        {
            uint64_t value = *memoryIter;
            uint64_t testBitMask = ~(bitCount < 64 ? (0xFFFFFFFFFFFFFFFF << (bitCount % 64)) : 0); // Create a bitmask of all the bits we want to test
            // If any of our bits were set
            if ((value & testBitMask) != 0)
            {
                fullyFree = false;
                break;
            }
        }

        return fullyFree;
    }

    uint64_t firstClearedBitIndex(uint64_t value)
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

        return value - 1;
    }

    uint64_t findClearedSlot(void *memory, uint64_t bitCount)
    {
        uint64_t freeSlot = NoSlot;

        uint64_t *memoryIter = reinterpret_cast<uint64_t *>(memory);
        for (; static_cast<int32_t>(bitCount) > 0; memoryIter++, bitCount -= 64)
        {
            uint64_t value = *memoryIter;
            uint64_t testBitMask = ~(bitCount < 64 ? (0xFFFFFFFFFFFFFFFF << (bitCount % 64)) : 0); // Create a bitmask of all the bits we want to test
            if ((value & testBitMask) != testBitMask)                                              // If any of our bits were cleared
            {
                uint64_t bitIndex = firstClearedBitIndex(value);

                ptrdiff_t chunkIndex = (memoryIter - reinterpret_cast<uint64_t *>(memory));
                freeSlot = chunkIndex * 64 + bitIndex;
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

    void clearSlot(void *memory, uint64_t index)
    {
        uint64_t *memoryIter = reinterpret_cast<uint64_t *>(memory);
        memoryIter = memoryIter + index / 64;
        *memoryIter &= ~(1ull << (index % 64));
    }

    void *getPageSlot(void *page, size_t blockSize, uint64_t blockCount, uint64_t slotIndex)
    {
        uintptr_t pageIter = reinterpret_cast<uintptr_t>(page);
        pageIter += blockCount / 8 + (blockCount % 8 > 0 ? 1 : 0);

        // Make sure we're aligned
        uintptr_t firstSlot = pageIter + ((pageIter % blockSize) > 0 ? (blockSize - pageIter % blockSize) : 0);
        pageIter = firstSlot + slotIndex * blockSize;

        assert(pageIter < reinterpret_cast<uintptr_t>(page) + IB::memoryPageSize());
        return reinterpret_cast<void *>(pageIter);
    }

    // Medium Memory Allocations

    // TODO: Document approach

    // Medium memory boundary is defined by how many blocks can be defined in a header
    // Assuming a 4kb memory page and a minimum block size of 512 (small memory boundary)
    // Then because we're going to be using a budy allocator, we want to define a hiearchy
    // And each bit in our header will represent an element in our hierarchy
    // x x x x
    //  x   x
    //    x
    // Now to determine how many blocks of 512 our hierarchy can hold, we need
    // to determine how many allocation bits we need to allocate for our header.
    // Because the number of allocation bits required by a base number of digits is 2*n-1 (n+n/2+n/4+n/8+...)
    // we can see that
    // 4kb=2*n-1
    // remove the -1, we don't mind if we're using up an extra bit and it keeps our math in powers of 2
    // Solve for n
    // 4kb/2 = n
    // n=2048
    // n*512 = 1048576 or 1MB
    constexpr size_t MediumMemoryBoundary = 1024 * 1024 / 2;
    // If ever our 1MB buddy chunks are too small and we're making too
    // many large allocations, we could add a second level of buddy chunks with
    // 1MB sized base blocks allowing us to have buddy chunks of up to 2GB
    constexpr size_t BuddyChunkSize = 1024 * 1024; // 1MB buddy chunks
    constexpr uint32_t BuddyChunkCount = 1024;     // At most 1GB of memory for medium allocations

    struct BuddyChunk
    {
        void *Header = nullptr;
        void *MemoryPages = nullptr;
    };
    BuddyChunk BuddyChunks[BuddyChunkCount] = {};

    uint32_t log2(size_t value)
    {
        uint32_t pow2 = 0;
        for (; value > 0; value = value >> 1, pow2++)
        {
        }
        return pow2 - 1;
    }

} // namespace

namespace IB
{
    void *memoryAllocate(size_t size, size_t alignment)
    {
        assert(size != 0);
        size_t blockSize = size;
        if (size != alignment && size % alignment != 0)
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
        // By this point, if blockSize is larger than size, then we have internal fragmentation.
        // TODO: Log internal fragmentation

        if (blockSize <= SmallMemoryBoundary)
        {
            size_t tableIndex = blockSize - 1;
            // If our table hasn't been initialized, allocate a page for it
            if (SmallMemoryPageTables[tableIndex].MemoryPages == nullptr)
            {
                SmallMemoryPageTables[tableIndex].Header = IB::reserveMemoryPages(1);
                IB::commitMemoryPages(SmallMemoryPageTables[tableIndex].Header, 1);

                SmallMemoryPageTables[tableIndex].MemoryPages = IB::reserveMemoryPages(IB::memoryPageSize() * 8);

                uintptr_t start = reinterpret_cast<uintptr_t>(SmallMemoryPageTables[tableIndex].MemoryPages);
                uintptr_t end = start + IB::memoryPageSize() * 8 * IB::memoryPageSize();
                SmallMemoryPageRanges[tableIndex] = PageRange{start, end};
            }

            // Find our free page address
            void *page;
            uint64_t pageCount = (IB::memoryPageSize() * 8);
            uint64_t pageIndex = findClearedSlot(SmallMemoryPageTables[tableIndex].Header, pageCount);
            assert(pageIndex != NoSlot); // We're out of memory pages for this size class! Improve this algorithm to support the use case.

            {
                uintptr_t pageAddress = reinterpret_cast<uintptr_t>(SmallMemoryPageTables[tableIndex].MemoryPages);
                pageAddress = pageAddress + IB::memoryPageSize() * pageIndex;
                page = reinterpret_cast<void *>(pageAddress);
                IB::commitMemoryPages(page, 1);
            }

            // Find our memory address
            void *memory;
            {
                uint64_t blockCount = (IB::memoryPageSize() * 8) / (1 + blockSize * 8);
                uint64_t freeSlot = findClearedSlot(page, blockCount);
                assert(freeSlot != NoSlot); // Our page's "fully allocated" bit was cleared, how come we have no space?

                setSlot(page, freeSlot);
                if (areAllSlotsSet(page, blockCount))
                {
                    setSlot(SmallMemoryPageTables[tableIndex].Header, pageIndex);
                }

                memory = getPageSlot(page, blockSize, blockCount, freeSlot);
            }

            return memory;
        }
        else if (blockSize <= MediumMemoryBoundary)
        {
            for (uint32_t i = 0; i < BuddyChunkCount; i++)
            {
                if (BuddyChunks[i].Header == nullptr)
                {
                    BuddyChunks[i].Header = IB::reserveMemoryPages(1);
                    IB::commitMemoryPages(BuddyChunks[i].Header);

                    BuddyChunks[i].MemoryPages = IB::reserveMemoryPages(BuddyChunkSize / IB::memoryPageSize());
                }

                uint64_t *headerBits = reinterpret_cast<uint64_t *>(BuddyChunks[i].Header);

                // Buddy level count is log2(memoryPageSize/2), use log2(memoryPageSize)-1 identity instead
                uint32_t buddyLevelCount = log2(IB::memoryPageSize()) - 1;
                uint32_t buddyLevel = buddyLevelCount - (log2(blockSize) - log2(SmallMemoryBoundary));
                // To get to our level bit, add all our previous levels
                // Ex: For level 5 from the top
                // 1+2+4+8 = 15
                // So 2^level-1
                uint32_t levelOffset = (1 << buddyLevel) - 1;
                uint32_t buddyCounts = (1 << buddyLevel);

                headerBits += levelOffset / 64;
                levelOffset = levelOffset % 64;

                uint64_t freeSlot = NoSlot;
                {
                    // prologue to get our bits aligned on a 64 bit boundary
                    uint64_t buddyCountsMask = (buddyCounts > 64 ? 0xFFFFFFFFFFFFFFFF : (1ull << buddyCounts) - 1);
                    uint64_t buddyMask = buddyCountsMask << levelOffset;

                    uint64_t value = *headerBits;
                    if ((value & buddyMask) != buddyMask)
                    {
                        freeSlot = firstClearedBitIndex(value);
                    }
                }

                if (freeSlot == NoSlot)
                {
                    // We didn't find our value in the prologue
                    // Look through the rest of our level
                    freeSlot = findClearedSlot(headerBits + 1, buddyCounts - (64 - levelOffset));
                }

                if (freeSlot != NoSlot)
                {
                    // TODO: Allocate, we need to make sure to set the bits of our child indices
                    // as well as our parent indices
                    break;
                }

                // Continue looping if we didn't find a slot in this buddy chunk.
            }
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
            size_t blockSize = memoryPageIndex + 1;
            uint64_t blockCount = (IB::memoryPageSize() * 8) / (1 + blockSize * 8);

            ptrdiff_t offsetFromStart = memoryAddress - SmallMemoryPageRanges[memoryPageIndex].Start;
            ptrdiff_t pageIndex = offsetFromStart / IB::memoryPageSize();

            void *page = reinterpret_cast<void *>(SmallMemoryPageRanges[memoryPageIndex].Start + pageIndex * IB::memoryPageSize());
            void *firstSlot = getPageSlot(page, blockSize, blockCount, 0);
            uintptr_t indexInPage = (memoryAddress - reinterpret_cast<uintptr_t>(firstSlot)) / blockSize;
            clearSlot(page, indexInPage);
            if (areAllSlotsClear(page, blockCount))
            {
                IB::decommitMemoryPages(page, 1);
            }

            clearSlot(SmallMemoryPageTables[memoryPageIndex].Header, pageIndex);
        }
    }

} // namespace IB
