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

    uint8_t log2(size_t value)
    {
        uint8_t pow2 = 0;
        for (; value > 0; value = value >> 1, pow2++)
        {
        }
        return pow2 - 1;
    }

    // TODO: Document approach

    constexpr uint32_t MaxBuddyBlockCount = 4096; // Maximum block size is 4096 * SmallestBuddyChunkSize (4096 * 1024 = 4MB)
    constexpr size_t SmallestBuddyBlockSize = SmallMemoryBoundary * 2;
    constexpr size_t BuddyChunkSize = MaxBuddyBlockCount * SmallestBuddyBlockSize;
    constexpr size_t MediumMemoryBoundary = MaxBuddyBlockCount * SmallestBuddyBlockSize / 2; // We don't want to be able to allocate a whole buddy chunk.
    constexpr uint32_t BuddyChunkCount = 1024; // arbitrary. Is roughly 4GB with MaxBuddyBlockCount of 4096 and SmallestBuddyChunkSize of 1024

    struct BuddyBlock
    {
        uint16_t Index = 0; // Our index in terms of our layer's size. It can go up to MaxBuddyBlockCount
        uint8_t Layer = 0;
    };

    struct BuddyChunk
    {
        void *MemoryPages = nullptr;
        BuddyBlock AllocatedBlocks[MaxBuddyBlockCount];
        BuddyBlock FreeBlocks[MaxBuddyBlockCount];
        uint32_t AllocatedBlockCount = 0;
        uint32_t FreeBlockCount = 0;
    };
    BuddyChunk* BuddyChunks = nullptr;

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
            if (BuddyChunks == nullptr)
            {
                uint32_t memoryPageCount = static_cast<uint32_t>(sizeof(BuddyChunk) * BuddyChunkCount / IB::memoryPageSize());
                BuddyChunks = reinterpret_cast<BuddyChunk*>(IB::reserveMemoryPages(memoryPageCount));
                IB::commitMemoryPages(BuddyChunks, memoryPageCount);
            }

            for (uint32_t buddyChunkIndex = 0; buddyChunkIndex < BuddyChunkCount; buddyChunkIndex++)
            {
                if (BuddyChunks[buddyChunkIndex].MemoryPages == nullptr)
                {
                    BuddyBlock initialBlock{};
                    initialBlock.Index = 0;
                    initialBlock.Layer = log2(BuddyChunkSize) - log2(SmallestBuddyBlockSize);

                    BuddyChunks[buddyChunkIndex].FreeBlocks[0] = initialBlock;
                    BuddyChunks[buddyChunkIndex].FreeBlockCount = 1;

                    BuddyChunks[buddyChunkIndex].MemoryPages = IB::reserveMemoryPages(static_cast<uint32_t>(BuddyChunkSize / IB::memoryPageSize()));
                }

                uint8_t requestedLayer = log2(blockSize) - log2(SmallestBuddyBlockSize);

                uint32_t closestBlockIndex = UINT32_MAX;
                uint8_t closestBlockLayer = UINT8_MAX;

                BuddyBlock *freeBlocks = BuddyChunks[buddyChunkIndex].FreeBlocks;
                for (uint32_t i = 0; i < BuddyChunks[buddyChunkIndex].FreeBlockCount; i++)
                {
                    if (freeBlocks[i].Layer >= requestedLayer && freeBlocks[i].Layer < closestBlockLayer)
                    {
                        closestBlockLayer = freeBlocks[i].Layer;
                        closestBlockIndex = i;
                    }
                }

                if (closestBlockIndex != UINT32_MAX)
                {
                    // Recursively split our block until it's the right size
                    uint32_t currentBlockIndex = closestBlockIndex;
                    BuddyBlock currentBlock = freeBlocks[currentBlockIndex];

                    while (currentBlock.Layer > requestedLayer)
                    {
                        // Remove our block
                        freeBlocks[currentBlockIndex] = freeBlocks[BuddyChunks[buddyChunkIndex].FreeBlockCount - 1];
                        BuddyChunks[buddyChunkIndex].FreeBlockCount--;

                        BuddyBlock nextBlock = BuddyBlock{};
                        nextBlock.Layer = currentBlock.Layer - 1;
                        nextBlock.Index = currentBlock.Index * 2;
                        freeBlocks[BuddyChunks[buddyChunkIndex].FreeBlockCount] = nextBlock;

                        nextBlock.Index = currentBlock.Index * 2 + 1;
                        freeBlocks[BuddyChunks[buddyChunkIndex].FreeBlockCount + 1] = nextBlock;

                        currentBlockIndex = BuddyChunks[buddyChunkIndex].FreeBlockCount;
                        currentBlock = freeBlocks[BuddyChunks[buddyChunkIndex].FreeBlockCount];
                        BuddyChunks[buddyChunkIndex].FreeBlockCount += 2;
                    }
                    assert(currentBlock.Layer == requestedLayer);

                    // Remove our final block
                    freeBlocks[currentBlockIndex] = freeBlocks[BuddyChunks[buddyChunkIndex].FreeBlockCount - 1];
                    BuddyChunks[buddyChunkIndex].FreeBlockCount--;

                    BuddyChunks[buddyChunkIndex].AllocatedBlocks[BuddyChunks[buddyChunkIndex].AllocatedBlockCount] = currentBlock;
                    BuddyChunks[buddyChunkIndex].AllocatedBlockCount++;

                    ptrdiff_t memoryOffset = blockSize * currentBlock.Index;
                    uintptr_t memoryAddress = reinterpret_cast<uintptr_t>(BuddyChunks[buddyChunkIndex].MemoryPages) + memoryOffset;

                    uintptr_t alignedMemoryAddress = memoryAddress / IB::memoryPageSize() * IB::memoryPageSize();
                    uint32_t memoryPageCount = static_cast<uint32_t>(blockSize / IB::memoryPageSize()) + (blockSize % IB::memoryPageSize() != 0 ? 1 : 0);

                    IB::commitMemoryPages(reinterpret_cast<void*>(alignedMemoryAddress), memoryPageCount);
                    return reinterpret_cast<void*>(memoryAddress);
                }

                // Continue looping if we didn't find a slot in this buddy chunk.
            }
        }
        else
        {
            return IB::mapLargeMemoryBlock(blockSize);
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
                uintptr_t pageStart = reinterpret_cast<uintptr_t>(BuddyChunks[memoryPageIndex].MemoryPages);
                ptrdiff_t offsetFromStart = reinterpret_cast<uintptr_t>(memory) - pageStart;

                uint32_t blockIndex = UINT32_MAX;
                for (uint32_t i = 0; i < BuddyChunks[memoryPageIndex].AllocatedBlockCount; i++)
                {
                    BuddyBlock currentBlock = BuddyChunks[memoryPageIndex].AllocatedBlocks[i];

                    size_t blockSize = 1ull << (currentBlock.Layer + log2(SmallestBuddyBlockSize));
                    ptrdiff_t memoryOffset = blockSize * currentBlock.Index;

                    if (memoryOffset == offsetFromStart)
                    {
                        blockIndex = i;
                        break;
                    }
                }

                if (blockIndex != UINT32_MAX)
                {
                    BuddyBlock currentBlock = BuddyChunks[memoryPageIndex].AllocatedBlocks[blockIndex];

                    BuddyChunks[memoryPageIndex].AllocatedBlocks[blockIndex] = BuddyChunks[memoryPageIndex].AllocatedBlocks[BuddyChunks[memoryPageIndex].AllocatedBlockCount - 1];
                    BuddyChunks[memoryPageIndex].AllocatedBlockCount--;

                    uint32_t currentFreeBlockIndex = BuddyChunks[memoryPageIndex].FreeBlockCount;
                    BuddyChunks[memoryPageIndex].FreeBlocks[BuddyChunks[memoryPageIndex].FreeBlockCount] = currentBlock;
                    BuddyChunks[memoryPageIndex].FreeBlockCount++;

                    if (currentBlock.Layer >= log2(IB::memoryPageSize()) - log2(SmallestBuddyBlockSize))
                    {
                        size_t blockSize = 1ull << (currentBlock.Layer + log2(SmallestBuddyBlockSize));
                        ptrdiff_t memoryOffset = blockSize * currentBlock.Index;
                        uintptr_t memoryAddress = reinterpret_cast<uintptr_t>(BuddyChunks[memoryPageIndex].MemoryPages) + memoryOffset;

                        uintptr_t alignedMemoryAddress = memoryAddress / IB::memoryPageSize() * IB::memoryPageSize();
                        uint32_t memoryPageCount = static_cast<uint32_t>(blockSize / IB::memoryPageSize()) + (blockSize % IB::memoryPageSize() != 0 ? 1 : 0);

                        IB::decommitMemoryPages(reinterpret_cast<void*>(alignedMemoryAddress), memoryPageCount);
                    }

                    // Coallesce our buddies back into bigger blocks
                    bool blockFound = true;
                    while(blockFound)
                    {
                        blockFound = false;

                        // Don't test the block we've just added, it's always at the end of our list
                        uint32_t freeBlockEnd = BuddyChunks[memoryPageIndex].FreeBlockCount - 1;

                        uint16_t evenIndex = currentBlock.Index & ~1;
                        for (uint32_t i = 0; i < freeBlockEnd; i++)
                        {
                            BuddyBlock otherBlock = BuddyChunks[memoryPageIndex].FreeBlocks[i];
                            uint16_t otherEvenIndex = otherBlock.Index & ~1;
                            if (otherEvenIndex == evenIndex && otherBlock.Layer == currentBlock.Layer)
                            {
                                BuddyChunks[memoryPageIndex].FreeBlocks[i] = BuddyChunks[memoryPageIndex].FreeBlocks[BuddyChunks[memoryPageIndex].FreeBlockCount - 1];
                                BuddyChunks[memoryPageIndex].FreeBlocks[currentFreeBlockIndex] = BuddyChunks[memoryPageIndex].FreeBlocks[BuddyChunks[memoryPageIndex].FreeBlockCount - 2];
                                BuddyChunks[memoryPageIndex].FreeBlockCount -= 2;

                                BuddyBlock parentBlock{};
                                parentBlock.Index = evenIndex / 2;
                                parentBlock.Layer = currentBlock.Layer + 1;

                                currentBlock = parentBlock;
                                currentFreeBlockIndex = BuddyChunks[memoryPageIndex].FreeBlockCount;
                                BuddyChunks[memoryPageIndex].FreeBlocks[BuddyChunks[memoryPageIndex].FreeBlockCount] = parentBlock;
                                BuddyChunks[memoryPageIndex].FreeBlockCount++;

                                if (currentBlock.Layer >= log2(IB::memoryPageSize()) - log2(SmallestBuddyBlockSize))
                                {
                                    size_t blockSize = 1ull << (currentBlock.Layer + log2(SmallestBuddyBlockSize));
                                    ptrdiff_t memoryOffset = blockSize * currentBlock.Index;
                                    uintptr_t memoryAddress = reinterpret_cast<uintptr_t>(BuddyChunks[memoryPageIndex].MemoryPages) + memoryOffset;

                                    uintptr_t alignedMemoryAddress = memoryAddress / IB::memoryPageSize() * IB::memoryPageSize();
                                    uint32_t memoryPageCount = static_cast<uint32_t>(blockSize / IB::memoryPageSize()) + (blockSize % IB::memoryPageSize() != 0 ? 1 : 0);

                                    IB::decommitMemoryPages(reinterpret_cast<void*>(alignedMemoryAddress), memoryPageCount);
                                }

                                blockFound = true;
                            }
                        }
                    };
                }

                return;
            }
        }

        // Large memory allocations
        {
            IB::unmapLargeMemoryBlock(memory);
        }
    }

} // namespace IB
