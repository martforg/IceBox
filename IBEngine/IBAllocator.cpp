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

    // Make our memory thread local to support multi-threaded access to the page tables.
    // Note that this is not necessarily the best approach to making our memory allocation
    // but is definitely the easiest.
    // If problems arise in the future, we might want to re-examine how we can make the
    // allocator threadsafe.
    thread_local PageTable SmallMemoryPageTables[SmallMemoryBoundary] = {};
    const size_t SmallMemoryRange = IB::memoryPageSize() * 8 * IB::memoryPageSize();

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

    uint32_t log2(size_t value)
    {
        uint32_t pow2 = 0;
        for (; value > 0; value = value >> 1, pow2++)
        {
        }
        return pow2 - 1;
    }

    // TODO: Document approach

    // Medium memory boundary is defined by how many blocks can be defined in a header
    // Assuming a 4kb memory page and a minimum block size of 512 (small memory boundary)
    // Then because we're going to be using a budy allocator, we want to define a hiearchy
    // And each bit in our header will represent an element in our hierarchy
    // x x x x
    //  x   x
    //    x
    // Now to determine how many blocks of 1024 our hierarchy can hold, we need
    // to determine how many allocation bits we need to allocate for our header.
    // Because the number of allocation bits required by a base number of digits is 2*n-1 (n+n/2+n/4+n/8+...)
    // we can see that
    // PageSize*8=2*n-1
    // remove the -1, we don't mind if we're using up an extra bit and it keeps our math in powers of 2
    // Solve for n
    // PageSize*8/2 = n
    // n=PageSize/2*8
    // We only want to allocate PageSize/2 number of blocks instead of PageSize*8/2 since we would have to traverse
    // A whole page simply to mark our blocks as allocated. Instead we only traverse PageSize/8 (256 bytes for a 4kb page) bytes
    
    // If ever our buddy chunks are too small and we're making too
    // many large allocations, we could add a second hierarchy of buddy chunks
    constexpr size_t SmallestBuddyChunkSize = SmallMemoryBoundary * 2;
    const size_t BuddyChunkSize = IB::memoryPageSize() / 2 * SmallestBuddyChunkSize;
    const uint32_t BuddyLevelCount = log2(IB::memoryPageSize()) - 1;

    const size_t MediumMemoryBoundary = BuddyChunkSize / 2; // We don't want to allocate a whole buddy chunk
    constexpr uint32_t BuddyChunkCount = 1024; // arbitrary

    struct BuddyChunk
    {
        void *Header = nullptr;
        void *MemoryPages = nullptr;
    };
    BuddyChunk BuddyChunks[BuddyChunkCount] = {};

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
            for (uint32_t buddyChunkIndex = 0; buddyChunkIndex < BuddyChunkCount; buddyChunkIndex++)
            {
                if (BuddyChunks[buddyChunkIndex].Header == nullptr)
                {
                    BuddyChunks[buddyChunkIndex].Header = IB::reserveMemoryPages(1);
                    IB::commitMemoryPages(BuddyChunks[buddyChunkIndex].Header, 1);

                    BuddyChunks[buddyChunkIndex].MemoryPages = IB::reserveMemoryPages(static_cast<uint32_t>(BuddyChunkSize / IB::memoryPageSize()));
                }

                uint64_t *headerBits = reinterpret_cast<uint64_t *>(BuddyChunks[buddyChunkIndex].Header);

                // Calculate our next largest power of 2 block
                uint64_t buddySize = 1ull << log2(blockSize);

                uint32_t buddyLevel = BuddyLevelCount - (log2(blockSize) - log2(SmallMemoryBoundary * 2));
                // To get to our level bit, add all our previous levels
                // Ex: For level 5 from the top
                // 1+2+4+8 = 15
                // So 2^level-1
                uint64_t levelOffset = (1ull << buddyLevel) - 1;
                uint64_t buddyCounts = (1ull << buddyLevel);

                uint64_t *levelHeaderBits = headerBits + levelOffset / 64;
                uint64_t levelBitOffset = levelOffset % 64;

                uint64_t freeSlot = NoSlot;
                {
                    // prologue to get our bits aligned on a 64 bit boundary
                    uint64_t buddyCountsMask = (buddyCounts > 64 ? 0xFFFFFFFFFFFFFFFF : (1ull << buddyCounts) - 1);
                    uint64_t buddyMask = buddyCountsMask << levelBitOffset;

                    uint64_t value = *levelHeaderBits;
                    if ((value & buddyMask) != buddyMask)
                    {
                        freeSlot = firstClearedBitIndex(value);
                    }
                }

                if (freeSlot == NoSlot)
                {
                    // We didn't find our value in the prologue
                    // Look through the rest of our level
                    freeSlot = (64 - levelBitOffset) + findClearedSlot(levelHeaderBits + 1, buddyCounts - (64 - levelBitOffset));
                }

                if (freeSlot != NoSlot)
                {
                    uint64_t slotOffset = levelOffset + freeSlot;
                    uint64_t *slotBits = headerBits + slotOffset / 64;
                    *slotBits = *slotBits | (1ull << (slotOffset % 64));

                    uint8_t *memory = reinterpret_cast<uint8_t *>(BuddyChunks[buddyChunkIndex].MemoryPages);
                    memory += freeSlot * buddySize;

                    if (blockSize == IB::memoryPageSize())
                    {
                        IB::commitMemoryPages(memory, 1);
                    }

                    // This approach might be slow. Keep an eye on it.
                    // Mark our parent bits as allocated
                    uint64_t parentSlot = freeSlot;
                    for (uint32_t i = buddyLevel; i > 0; i--)
                    {
                        parentSlot = parentSlot / 2;

                        uint32_t parentLevel = i - 1;
                        uint32_t parentLevelOffset = (1ull << parentLevel) - 1;

                        uint64_t parentSlotOffset = parentLevelOffset + parentSlot;
                        uint64_t *parentLevelBits = headerBits + parentSlotOffset / 64;

                        *parentLevelBits = *parentLevelBits | (1ull << (parentSlotOffset % 64));

                        size_t parentBlockSize = 1ull << ((BuddyLevelCount - parentLevel) + log2(SmallMemoryBoundary * 2));
                        if (parentBlockSize == IB::memoryPageSize())
                        {
                            uint8_t *parentMemory = reinterpret_cast<uint8_t *>(BuddyChunks[buddyChunkIndex].MemoryPages);
                            parentMemory += parentSlot * parentBlockSize;
                            IB::commitMemoryPages(parentMemory, IB::memoryPageSize());
                        }
                    }

                    // Mark our child bits as allocated
                    uint64_t childSlot = freeSlot;
                    uint64_t childBitCount = 1;
                    for (uint32_t i = buddyLevel + 1; i <= BuddyLevelCount; i++)
                    {
                        childSlot = childSlot * 2;
                        childBitCount = childBitCount * 2;

                        uint64_t childLevel = i;
                        uint64_t childLevelOffset = (1ull << childLevel) - 1;

                        for (uint32_t childIndex = 0; childIndex < childBitCount; childIndex++)
                        {
                            uint64_t childSlotOffset = childLevelOffset + childSlot + childIndex;
                            uint64_t *childLevelBits = headerBits + childSlotOffset / 64;

                            *childLevelBits = *childLevelBits | (1ull << (childSlotOffset % 64));

                            size_t childBlockSize = 1ull << ((BuddyLevelCount - childLevel) + log2(SmallMemoryBoundary * 2));
                            if (childBlockSize == IB::memoryPageSize())
                            {
                                uint8_t *childMemory = reinterpret_cast<uint8_t *>(BuddyChunks[buddyChunkIndex].MemoryPages);
                                childMemory += childSlot * childBlockSize;
                                IB::commitMemoryPages(childMemory, IB::memoryPageSize());
                            }
                        }
                    }

                    return memory;
                }

                // Continue looping if we didn't find a slot in this buddy chunk.
            }
        }

        // TODO: Large memory allocation
        return nullptr;
    }

    void memoryFree(void *memory)
    {
        // Small memory allocations
        {
            uint32_t memoryPageIndex = UINT32_MAX;
            for (uint32_t i = 0; i < SmallMemoryBoundary; i++)
            {
                uintptr_t memoryAddress = reinterpret_cast<uintptr_t>(memory);
                uintptr_t memoryStart = reinterpret_cast<uintptr_t>(SmallMemoryPageTables[i].MemoryPages);
                uintptr_t memoryEnd = memoryStart + SmallMemoryRange;

                if (memoryAddress >= memoryStart && memoryAddress < memoryEnd)
                {
                    memoryPageIndex = i;
                    break;
                }
            }

            // Small memory allocation
            if (memoryPageIndex != UINT32_MAX)
            {
                size_t blockSize = memoryPageIndex + 1;
                uint64_t blockCount = (IB::memoryPageSize() * 8) / (1 + blockSize * 8);

                uintptr_t memoryAddress = reinterpret_cast<uintptr_t>(memory);
                uintptr_t pageStart = reinterpret_cast<uintptr_t>(SmallMemoryPageTables[memoryPageIndex].MemoryPages);

                ptrdiff_t offsetFromStart = memoryAddress - pageStart;
                ptrdiff_t pageIndex = offsetFromStart / IB::memoryPageSize();

                void *page = reinterpret_cast<void *>(pageStart + pageIndex * IB::memoryPageSize());
                void *firstSlot = getPageSlot(page, blockSize, blockCount, 0);
                uintptr_t indexInPage = (memoryAddress - reinterpret_cast<uintptr_t>(firstSlot)) / blockSize;
                clearSlot(page, indexInPage);
                if (areAllSlotsClear(page, blockCount))
                {
                    IB::decommitMemoryPages(page, 1);
                }

                clearSlot(SmallMemoryPageTables[memoryPageIndex].Header, pageIndex);
                return;
            }
        }

        // Medium memory allocation
        {
            uint32_t memoryPageIndex = UINT32_MAX;
            for (uint32_t i = 0; i < BuddyChunkCount; i++)
            {
                uintptr_t memoryAddress = reinterpret_cast<uintptr_t>(memory);
                uintptr_t memoryStart = reinterpret_cast<uintptr_t>(BuddyChunks[i].MemoryPages);
                uintptr_t memoryEnd = memoryStart + BuddyChunkSize;

                if (memoryAddress >= memoryStart && memoryAddress < memoryEnd)
                {
                    memoryPageIndex = i;
                    break;
                }
            }

            if (memoryPageIndex != UINT32_MAX)
            {
                uintptr_t memoryAddress = reinterpret_cast<uintptr_t>(memory);
                uintptr_t pageStart = reinterpret_cast<uintptr_t>(BuddyChunks[memoryPageIndex].MemoryPages);

                ptrdiff_t offsetFromStart = memoryAddress - pageStart;

                // Find our smallest aligned block, that's our allocation size
                // TODO: How do we find what size we've allocated?
                size_t blockSize = SmallMemoryBoundary * 2;
                for (; blockSize < MediumMemoryBoundary; blockSize = blockSize * 2)
                {
                    if (offsetFromStart % blockSize == 0)
                    {
                        break;
                    }
                }
                assert(blockSize != MediumMemoryBoundary);

                uint32_t buddyLevel = BuddyLevelCount - (log2(blockSize) - log2(SmallMemoryBoundary * 2));
                uint64_t slotIndex = offsetFromStart / blockSize;
                uint64_t levelOffset = (1ull << buddyLevel) - 1;

                uint64_t *headerBits = reinterpret_cast<uint64_t *>(BuddyChunks[memoryPageIndex].Header);
                uint64_t slotOffset = levelOffset + slotIndex;
                uint64_t *slotBits = headerBits + slotOffset / 64;
                *slotBits = *slotBits & ~(1ull << (slotOffset % 64));

                if (blockSize == IB::memoryPageSize())
                {
                    IB::decommitMemoryPages(memory, 1);
                }

                // Mark our child bits as freed
                uint64_t childSlot = slotIndex;
                uint64_t childBitCount = 1;
                for (uint32_t i = buddyLevel + 1; i < BuddyLevelCount; i++)
                {
                    childSlot = childSlot * 2;
                    childBitCount = childBitCount * 2;

                    uint64_t childLevel = i;
                    uint64_t childLevelOffset = (1ull << childLevel) - 1;

                    for (uint32_t childIndex = 0; childIndex < childBitCount; childIndex++)
                    {
                        uint64_t childSlotOffset = childLevelOffset + childSlot + childIndex;
                        uint64_t *childLevelBits = headerBits + childSlotOffset / 64;

                        *childLevelBits = *childLevelBits & ~(1ull << (childSlotOffset % 64));

                        size_t childBlockSize = 1ull << ((BuddyLevelCount - childLevel) + log2(SmallMemoryBoundary * 2));
                        if (childBlockSize == IB::memoryPageSize())
                        {
                            uint8_t *childMemory = reinterpret_cast<uint8_t *>(BuddyChunks[memoryPageIndex].MemoryPages);
                            childMemory += childSlot * childBlockSize;
                            IB::decommitMemoryPages(childMemory, IB::memoryPageSize());
                        }
                    }
                }

                // Mark our parent bits as freed
                uint64_t currentSlot = slotIndex;
                for (uint32_t i = buddyLevel; i > 0; i--)
                {
                    uint32_t buddySlot0 = currentSlot & ~1;
                    uint32_t buddySlot1 = buddySlot0 + 1;

                    uint64_t currentLevelOffset = (1ull << i) - 1;

                    uint64_t buddySlotOffset0 = currentLevelOffset + buddySlot0;
                    uint64_t *buddyLevelBits0 = headerBits + buddySlotOffset0 / 64;

                    uint64_t buddySlotOffset1 = currentLevelOffset + buddySlot1;
                    uint64_t *buddyLevelBits1 = headerBits + buddySlotOffset1 / 64;

                    bool isAllocated0 = *buddyLevelBits0 & (1ull << (buddySlotOffset0 % 64));
                    bool isAllocated1 = *buddyLevelBits1 & (1ull << (buddySlotOffset1 % 64));
                    if (!isAllocated0 && !isAllocated1)
                    {
                        uint64_t parentSlot = currentSlot / 2;

                        uint32_t parentLevel = i - 1;
                        uint64_t parentLevelOffset = (1ull << parentLevel) - 1;

                        uint64_t parentSlotOffset = parentLevelOffset + parentSlot;
                        uint64_t *parentLevelBits = headerBits + parentSlotOffset / 64;

                        *parentLevelBits = *parentLevelBits & ~(1ull << (parentSlotOffset % 64));
                        size_t parentBlockSize = 1ull << ((BuddyLevelCount - parentLevel) + log2(SmallMemoryBoundary * 2));
                        if (parentBlockSize == IB::memoryPageSize())
                        {
                            uint8_t *parentMemory = reinterpret_cast<uint8_t *>(BuddyChunks[memoryPageIndex].MemoryPages);
                            parentMemory += parentSlot * parentBlockSize;
                            IB::decommitMemoryPages(parentMemory, IB::memoryPageSize());
                        }
                    }
                    else
                    {
                        break;
                    }

                    currentSlot = currentSlot / 2;
                }
            }
        }
    }

} // namespace IB
