// WinRT declarations for the real portals (host/uwp/portal_backends.cpp) that the mingw-w64 headers (llvm-mingw
// 20260922) lack: Windows.Devices.HumanInterfaceDevice, and the GIP half of Windows.Gaming.Input.Custom.
// IIDs and vtable orders were read from Windows.winmd (the Windows metadata, as in winrt_extra.h). The IIDs of the
// two parameterized interfaces are the WinRT parameterized-interface IIDs (SHA-1 of their signature), the algorithm
// checked against the IAsyncOperation<UINT32> IID that mingw-w64 declares.
#pragma once

#include <windows.h>
#include <inspectable.h>
#include <eventtoken.h>
#include <windows.foundation.h>
#include <windows.gaming.input.custom.h>
#include <windows.storage.h>
#include <windows.storage.streams.h>

namespace ABI::Windows::Devices::HumanInterfaceDevice
{
    MIDL_INTERFACE("c35d0e50-f7e7-4e8d-b23e-cabbe56b90e9")
    IHidInputReport : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE get_Id(UINT16* value) = 0;
        virtual HRESULT STDMETHODCALLTYPE get_Data(Storage::Streams::IBuffer** value) = 0;
        // Later members (controls) are not used.
    };

    MIDL_INTERFACE("62cb2544-c896-4463-93c1-df9db053c450")
    IHidOutputReport : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE get_Id(UINT16* value) = 0;
        virtual HRESULT STDMETHODCALLTYPE get_Data(Storage::Streams::IBuffer** value) = 0;
        virtual HRESULT STDMETHODCALLTYPE put_Data(Storage::Streams::IBuffer* value) = 0;
        // Later members (controls) are not used.
    };

    MIDL_INTERFACE("7059c5cb-59b2-4dc2-985c-0adc6136fa2d")
    IHidInputReportReceivedEventArgs : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE get_Report(IHidInputReport** value) = 0;
    };

    struct IHidDevice;

    // Windows.Foundation.TypedEventHandler<HidDevice, HidInputReportReceivedEventArgs>
    MIDL_INTERFACE("31e757c8-8f6a-540b-938b-aba79b6f03ec")
    IHidInputReportReceivedHandler : public IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE Invoke(IHidDevice* sender, IHidInputReportReceivedEventArgs* args) = 0;
    };

    MIDL_INTERFACE("5f8a14e7-2200-432e-95da-d09b87d574a8")
    IHidDevice : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE get_VendorId(UINT16* value) = 0;
        virtual HRESULT STDMETHODCALLTYPE get_ProductId(UINT16* value) = 0;
        virtual HRESULT STDMETHODCALLTYPE get_Version(UINT16* value) = 0;
        virtual HRESULT STDMETHODCALLTYPE get_UsagePage(UINT16* value) = 0;
        virtual HRESULT STDMETHODCALLTYPE get_UsageId(UINT16* value) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetInputReportAsync(IInspectable** operation) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetInputReportByIdAsync(UINT16 reportId, IInspectable** operation) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetFeatureReportAsync(IInspectable** operation) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetFeatureReportByIdAsync(UINT16 reportId, IInspectable** operation) = 0;
        virtual HRESULT STDMETHODCALLTYPE CreateOutputReport(IHidOutputReport** report) = 0;
        virtual HRESULT STDMETHODCALLTYPE CreateOutputReportById(UINT16 reportId, IHidOutputReport** report) = 0;
        virtual HRESULT STDMETHODCALLTYPE CreateFeatureReport(IInspectable** report) = 0;
        virtual HRESULT STDMETHODCALLTYPE CreateFeatureReportById(UINT16 reportId, IInspectable** report) = 0;
        virtual HRESULT STDMETHODCALLTYPE SendOutputReportAsync(IHidOutputReport* report,
            Foundation::IAsyncOperation<UINT32>** operation) = 0;
        virtual HRESULT STDMETHODCALLTYPE SendFeatureReportAsync(IInspectable* report, IInspectable** operation) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetBooleanControlDescriptions(INT32 reportType, UINT16 usagePage, UINT16 usageId,
            IInspectable** value) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetNumericControlDescriptions(INT32 reportType, UINT16 usagePage, UINT16 usageId,
            IInspectable** value) = 0;
        virtual HRESULT STDMETHODCALLTYPE add_InputReportReceived(IHidInputReportReceivedHandler* handler,
            EventRegistrationToken* token) = 0;
        virtual HRESULT STDMETHODCALLTYPE remove_InputReportReceived(EventRegistrationToken token) = 0;
    };

    // Windows.Foundation.IAsyncOperation<HidDevice>
    MIDL_INTERFACE("a76a4fbf-5177-5256-84a8-b31a8dcf1048")
    IAsyncOperationHidDevice : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
        virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetResults(IHidDevice** results) = 0;
    };

    MIDL_INTERFACE("9e5981e4-9856-418c-9f73-77de0cd85754")
    IHidDeviceStatics : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE GetDeviceSelector(UINT16 usagePage, UINT16 usageId, HSTRING* selector) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorVidPid(UINT16 usagePage, UINT16 usageId, UINT16 vendorId,
            UINT16 productId, HSTRING* selector) = 0;
        virtual HRESULT STDMETHODCALLTYPE FromIdAsync(HSTRING deviceId, Storage::FileAccessMode accessMode,
            IAsyncOperationHidDevice** operation) = 0;
    };
}
__CRT_UUID_DECL(ABI::Windows::Devices::HumanInterfaceDevice::IHidInputReport, 0xc35d0e50, 0xf7e7, 0x4e8d, 0xb2, 0x3e, 0xca, 0xbb, 0xe5, 0x6b, 0x90, 0xe9)
__CRT_UUID_DECL(ABI::Windows::Devices::HumanInterfaceDevice::IHidOutputReport, 0x62cb2544, 0xc896, 0x4463, 0x93, 0xc1, 0xdf, 0x9d, 0xb0, 0x53, 0xc4, 0x50)
__CRT_UUID_DECL(ABI::Windows::Devices::HumanInterfaceDevice::IHidInputReportReceivedEventArgs, 0x7059c5cb, 0x59b2, 0x4dc2, 0x98, 0x5c, 0x0a, 0xdc, 0x61, 0x36, 0xfa, 0x2d)
__CRT_UUID_DECL(ABI::Windows::Devices::HumanInterfaceDevice::IHidInputReportReceivedHandler, 0x31e757c8, 0x8f6a, 0x540b, 0x93, 0x8b, 0xab, 0xa7, 0x9b, 0x6f, 0x03, 0xec)
__CRT_UUID_DECL(ABI::Windows::Devices::HumanInterfaceDevice::IHidDevice, 0x5f8a14e7, 0x2200, 0x432e, 0x95, 0xda, 0xd0, 0x9b, 0x87, 0xd5, 0x74, 0xa8)
__CRT_UUID_DECL(ABI::Windows::Devices::HumanInterfaceDevice::IAsyncOperationHidDevice, 0xa76a4fbf, 0x5177, 0x5256, 0x84, 0xa8, 0xb3, 0x1a, 0x8d, 0xcf, 0x10, 0x48)
__CRT_UUID_DECL(ABI::Windows::Devices::HumanInterfaceDevice::IHidDeviceStatics, 0x9e5981e4, 0x9856, 0x418c, 0x9f, 0x73, 0x77, 0xde, 0x0c, 0xd8, 0x57, 0x54)

namespace ABI::Windows::Gaming::Input::Custom
{
    enum GipMessageClass : INT32
    {
        GipMessageClass_Command = 0,
        GipMessageClass_LowLatency = 1,
        GipMessageClass_StandardLatency = 2,
    };

    // Arrays are passed as (length, pointer), the WinRT ABI for u8[].
    MIDL_INTERFACE("dbcf1e19-1af5-45a8-bf02-a0ee50c823fc")
    IGipGameControllerProvider : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE SendMessage(GipMessageClass messageClass, BYTE messageId, UINT32 size,
            BYTE* message) = 0;
        virtual HRESULT STDMETHODCALLTYPE SendReceiveMessage(GipMessageClass messageClass, BYTE messageId,
            UINT32 requestSize, BYTE* request, UINT32 responseSize, BYTE* response) = 0;
        // UpdateFirmwareAsync is not used.
    };

    MIDL_INTERFACE("a2108abf-09f1-43bc-a140-80f899ec36fb")
    IGipGameControllerInputSink : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE OnKeyReceived(UINT64 timestamp, BYTE keyCode, boolean isPressed) = 0;
        virtual HRESULT STDMETHODCALLTYPE OnMessageReceived(UINT64 timestamp, GipMessageClass messageClass, BYTE messageId,
            BYTE sequenceId, UINT32 size, BYTE* message) = 0;
    };
}
__CRT_UUID_DECL(ABI::Windows::Gaming::Input::Custom::IGipGameControllerProvider, 0xdbcf1e19, 0x1af5, 0x45a8, 0xbf, 0x02, 0xa0, 0xee, 0x50, 0xc8, 0x23, 0xfc)
__CRT_UUID_DECL(ABI::Windows::Gaming::Input::Custom::IGipGameControllerInputSink, 0xa2108abf, 0x09f1, 0x43bc, 0xa1, 0x40, 0x80, 0xf8, 0x99, 0xec, 0x36, 0xfb)
