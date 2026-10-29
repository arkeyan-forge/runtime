// Declarations that the mingw-w64 headers (llvm-mingw 20260922) do not provide yet.
// IIDs and vtable orders come from the Windows metadata as published in the windows-rs crate (MIT/Apache-2.0,
// windows 0.58.0: src/Windows/System/mod.rs, src/Windows/UI/Core/mod.rs). The EventHandler<BackRequestedEventArgs>
// IID is the WinRT parameterized-interface IID (SHA-1 of its signature), checked against a known IID first.
#pragma once

#include <windows.h>
#include <inspectable.h>
#include <eventtoken.h>

namespace ABI::Windows::System
{
    MIDL_INTERFACE("5c6c279c-d7ca-4779-9188-4057219ce64c")
    IMemoryManagerStatics : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE get_AppMemoryUsage(UINT64 * value) = 0;
        virtual HRESULT STDMETHODCALLTYPE get_AppMemoryUsageLimit(UINT64 * value) = 0;
        // Later members (usage level and change events) are not used.
    };
}
__CRT_UUID_DECL(ABI::Windows::System::IMemoryManagerStatics, 0x5c6c279c, 0xd7ca, 0x4779, 0x91, 0x88, 0x40, 0x57, 0x21,
    0x9c, 0xe6, 0x4c)

namespace ABI::Windows::UI::Core
{
    MIDL_INTERFACE("d603d28a-e411-4a4e-ba41-6a327a8675bc")
    IBackRequestedEventArgs : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE get_Handled(boolean * value) = 0;
        virtual HRESULT STDMETHODCALLTYPE put_Handled(boolean value) = 0;
    };

    // Windows.Foundation.EventHandler<Windows.UI.Core.BackRequestedEventArgs>
    MIDL_INTERFACE("ca821060-002b-526d-8122-982630d7cdbe")
    IBackRequestedEventHandler : public IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable * sender, IBackRequestedEventArgs * args) = 0;
    };

    MIDL_INTERFACE("93023118-cf50-42a6-9706-69107fa122e1")
    ISystemNavigationManager : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE add_BackRequested(
            IBackRequestedEventHandler * handler, EventRegistrationToken * token) = 0;
        virtual HRESULT STDMETHODCALLTYPE remove_BackRequested(EventRegistrationToken token) = 0;
    };

    MIDL_INTERFACE("dc52b5ce-bee0-4305-8c54-68228ed683b5")
    ISystemNavigationManagerStatics : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE GetForCurrentView(ISystemNavigationManager * *value) = 0;
    };
}
__CRT_UUID_DECL(ABI::Windows::UI::Core::IBackRequestedEventArgs, 0xd603d28a, 0xe411, 0x4a4e, 0xba, 0x41, 0x6a, 0x32,
    0x7a, 0x86, 0x75, 0xbc)
__CRT_UUID_DECL(ABI::Windows::UI::Core::IBackRequestedEventHandler, 0xca821060, 0x002b, 0x526d, 0x81, 0x22, 0x98, 0x26,
    0x30, 0xd7, 0xcd, 0xbe)
__CRT_UUID_DECL(ABI::Windows::UI::Core::ISystemNavigationManager, 0x93023118, 0xcf50, 0x42a6, 0x97, 0x06, 0x69, 0x10,
    0x7f, 0xa1, 0x22, 0xe1)
__CRT_UUID_DECL(ABI::Windows::UI::Core::ISystemNavigationManagerStatics, 0xdc52b5ce, 0xbee0, 0x4305, 0x8c, 0x54, 0x68,
    0x22, 0x8e, 0xd6, 0x83, 0xb5)

// fileapifromapp.h (api-ms-win-core-file-fromapp-l1-1-0.dll, Windows 10 1803+; the functions are in libwindowsapp.a,
// only the header is missing). These variants let an app with the broadFileSystemAccess capability reach paths
// outside its container (e.g. USB drives) through the file broker.
extern "C"
{
    WINBASEAPI HANDLE WINAPI CreateFile2FromAppW(LPCWSTR fileName, DWORD desiredAccess, DWORD shareMode,
        DWORD creationDisposition, LPCREATEFILE2_EXTENDED_PARAMETERS extendedParameters);
    WINBASEAPI HANDLE WINAPI FindFirstFileExFromAppW(LPCWSTR fileName, FINDEX_INFO_LEVELS infoLevelId,
        LPVOID findFileData, FINDEX_SEARCH_OPS searchOp, LPVOID searchFilter, DWORD additionalFlags);
    WINBASEAPI BOOL WINAPI GetFileAttributesExFromAppW(
        LPCWSTR fileName, GET_FILEEX_INFO_LEVELS infoLevelId, LPVOID fileInformation);
}
