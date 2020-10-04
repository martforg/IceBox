#include "IBPlatform.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sysinfoapi.h>

#include <assert.h>
#include <stdint.h>

namespace
{
    struct ActiveWindow
    {
        HWND WindowHandle = NULL;

        void (*OnCloseRequested)(void *) = nullptr;
        void *State = nullptr;
    };

    constexpr uint32_t MaxActiveWindows = 10;
    thread_local ActiveWindow ActiveWindows[MaxActiveWindows] = {};

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        ActiveWindow *activeWindow = nullptr;
        for (uint32_t i = 0; i < MaxActiveWindows; i++)
        {
            if (ActiveWindows[i].WindowHandle == hwnd)
            {
                activeWindow = &ActiveWindows[i];
                break;
            }
        }

        switch (msg)
        {
        case WM_CLOSE:
            if (activeWindow != nullptr && activeWindow->OnCloseRequested != nullptr)
            {
                activeWindow->OnCloseRequested(activeWindow->State);
            }
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }

        return 0;
    }

    IB::WindowHandle createWindowWin32(IB::WindowDesc desc, HWND parentWindowHandle, DWORD style)
    {
        HINSTANCE hinstance = GetModuleHandle(NULL);

        WNDCLASS wndClass = {};
        wndClass.lpfnWndProc = WndProc;
        wndClass.hInstance = hinstance;
        wndClass.lpszClassName = desc.Name;
        ATOM classAtom = RegisterClass(&wndClass);
        assert(classAtom != 0);

        RECT rect = {0, 0, desc.Width, desc.Height};
        BOOL result = AdjustWindowRect(&rect, style, FALSE);
        assert(result == TRUE);

        HWND hwnd = CreateWindowEx(
            0,
            desc.Name,
            desc.Name,
            style,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
            parentWindowHandle,
            NULL,
            hinstance,
            NULL);
        assert(hwnd != NULL);

        result = ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);

        uint32_t i = 0;
        for (; i < MaxActiveWindows; i++)
        {
            if (ActiveWindows[i].WindowHandle == NULL)
            {
                ActiveWindows[i].WindowHandle = hwnd;
                ActiveWindows[i].State = desc.CallbackState;
                ActiveWindows[i].OnCloseRequested = desc.OnCloseRequested;
                break;
            }
        }
        assert(i < MaxActiveWindows);

        return IB::WindowHandle{i};
    }

    struct ActiveFileMapping
    {
        HANDLE Handle = NULL;
        void *Mapping = nullptr;
    };

    constexpr uint32_t MaxFileMappingCount = 1024;
    thread_local ActiveFileMapping ActiveFileMappings[MaxFileMappingCount];
} // namespace

namespace IB
{
    WindowHandle createWindow(WindowDesc desc)
    {
        return createWindowWin32(desc, nullptr, WS_OVERLAPPEDWINDOW);
    }

    void destroyWindow(WindowHandle window)
    {
        DestroyWindow(ActiveWindows[window.value].WindowHandle);
        ActiveWindows[window.value] = {};
    }

    bool consumeMessageQueue(PlatformMessage *message)
    {
        MSG msg;

        bool hasMessage = PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
        if (hasMessage)
        {
            if (msg.message == WM_QUIT)
            {
                *message = PlatformMessage::Quit;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        return hasMessage;
    }

    void sendQuitMessage()
    {
        PostQuitMessage(0);
    }

    uint32_t memoryPageSize()
    {
        SYSTEM_INFO systemInfo;
        GetSystemInfo(&systemInfo);

        return systemInfo.dwPageSize;
    }

    void *reserveMemoryPages(uint32_t pageCount)
    {
        LPVOID address = VirtualAlloc(NULL, memoryPageSize() * pageCount, MEM_RESERVE, PAGE_NOACCESS);
        assert(address != NULL);
        return address;
    }

    void commitMemoryPages(void *pages, uint32_t pageCount)
    {
        assert(reinterpret_cast<uintptr_t>(pages) % memoryPageSize() == 0);

        VirtualAlloc(pages, memoryPageSize() * pageCount, MEM_COMMIT, PAGE_READWRITE);
    }

    void decommitMemoryPages(void *pages, uint32_t pageCount)
    {
        assert(reinterpret_cast<uintptr_t>(pages) % memoryPageSize() == 0);

        BOOL result = VirtualFree(pages, memoryPageSize() * pageCount, MEM_DECOMMIT);
        assert(result == TRUE);
    }

    void freeMemoryPages(void *pages, uint32_t pageCount)
    {
        assert(reinterpret_cast<uintptr_t>(pages) % memoryPageSize() == 0);

        BOOL result = VirtualFree(pages, memoryPageSize() * pageCount, MEM_RELEASE);
        assert(result == TRUE);
    }

    IB_API void *mapLargeMemoryBlock(size_t size)
    {
        HANDLE fileMapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xFFFFFFFF), NULL);
        void *map = MapViewOfFile(fileMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);

        for (uint32_t i = 0; i < MaxFileMappingCount; i++)
        {
            if (ActiveFileMappings[i].Handle == NULL)
            {
                ActiveFileMappings[i].Handle = fileMapping;
                ActiveFileMappings[i].Mapping = map;
                break;
            }
        }

        return map;
    }

    IB_API void unmapLargeMemoryBlock(void *memory)
    {
        for (uint32_t i = 0; i < MaxFileMappingCount; i++)
        {
            if (ActiveFileMappings[i].Mapping == memory)
            {
                UnmapViewOfFile(memory);
                CloseHandle(ActiveFileMappings[i].Handle);
                ActiveFileMappings[i] = {};
                break;
            }
        }
    }
} // namespace IB

// Bridge
extern "C"
{
    IB_API void *IB_createWindow(void *parentWindowHandle, const char *name, int width, int height)
    {
        IB::WindowDesc desc = {};
        desc.Name = name;
        desc.Width = width;
        desc.Height = height;
        IB::WindowHandle handle = createWindowWin32(desc, reinterpret_cast<HWND>(parentWindowHandle), DS_CONTROL | WS_CHILD);
        return ActiveWindows[handle.value].WindowHandle;
    }

    IB_API void IB_destroyWindow(void *windowHandle)
    {
        DestroyWindow(reinterpret_cast<HWND>(windowHandle));

        for (uint32_t i = 0; i < MaxActiveWindows; i++)
        {
            if (ActiveWindows[i].WindowHandle == windowHandle)
            {
                ActiveWindows[i] = {};
                break;
            }
        }
    }
}
