//
// Copyright (C) Microsoft Corporation. All rights reserved.
//
#pragma once
#include <unknwn.h>
#include <windows.h>
#include <propvarutil.h>
#include <devpropdef.h>
#include "devpkey.h"
#include "cfgmgr32.h"

#include <ole2.h>  // include unknown.h this must come before winrt header
#include <initguid.h>
#include <Ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <nserror.h>
#include <winmeta.h>
#include <d3d9types.h>

#include <mfvirtualcamera.h>
#include <strsafe.h>
#include <string>

#define RESULT_DIAGNOSTICS_LEVEL 4 // include function name

#include <wil\cppwinrt.h> // must be before the first C++ WinRT header, ref:https://github.com/Microsoft/wil/wiki/Error-handling-helpers
#include <wil\result.h>
#include <wil\com.h>

#include "EventHandler.h"
#include "SimpleFrameGenerator.h"
#include "SimpleMediaSource.h"
#include "SimpleMediaStream.h"
#include "HWMediaSource.h"
#include "HWMediaStream.h"
#include "AugmentedMediaSource.h"
#include "AugmentedMediaStream.h"
#include "VirtualCameraMediaSource.h"
#include "VirtualCameraMediaSourceActivate.h"

#include "winrt\Windows.ApplicationModel.h"

#pragma comment(lib, "mfuuid")
#pragma comment(lib, "mf")
#pragma comment(lib, "windowsapp")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "Mfsensorgroup")

inline void DebugPrint(LPCWSTR szFormat, ...)
{
    WCHAR szBuffer[MAX_PATH] = { 0 };

    va_list pArgs;
    va_start(pArgs, szFormat);
    StringCbVPrintf(szBuffer, sizeof(szBuffer), szFormat, pArgs);
    va_end(pArgs);
    OutputDebugStringW(szBuffer);
}

#define DEBUG_MSG(msg,...) \
{\
    DebugPrint(L"[%s@%d] ", TEXT(__FUNCTION__), __LINE__);\
    DebugPrint(msg, __VA_ARGS__);\
    DebugPrint(L"\n");\
}\

inline void WriteMediaSourceDiagnostic(_In_opt_z_ PCWSTR path, _In_z_ PCWSTR format, ...)
{
    WCHAR message[768] = {};
    va_list args;
    va_start(args, format);
    StringCbVPrintfW(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    WCHAR record[1024] = {};
    StringCbPrintfW(
        record,
        sizeof(record),
        L"[%04u-%02u-%02uT%02u:%02u:%02u.%03u] %s\r\n",
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds),
        message);
    OutputDebugStringW(record);

    if (path == nullptr || path[0] == L'\0')
    {
        return;
    }

    HANDLE file = CreateFileW(
        path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    const int characterCount = lstrlenW(record);
    const int byteCount = WideCharToMultiByte(CP_UTF8, 0, record, characterCount, nullptr, 0, nullptr, nullptr);
    if (byteCount > 0 && byteCount <= 4096)
    {
        char utf8[4096] = {};
        const int converted = WideCharToMultiByte(
            CP_UTF8,
            0,
            record,
            characterCount,
            utf8,
            byteCount,
            nullptr,
            nullptr);
        if (converted == byteCount)
        {
            DWORD bytesWritten = 0;
            if (WriteFile(file, utf8, static_cast<DWORD>(byteCount), &bytesWritten, nullptr) && bytesWritten == static_cast<DWORD>(byteCount))
            {
                FlushFileBuffers(file);
            }
        }
    }
    CloseHandle(file);
}

inline HRESULT LogMediaSourceHRESULT(_In_opt_z_ PCWSTR path, _In_z_ PCWSTR stage, HRESULT result)
{
    if (FAILED(result))
    {
        WriteMediaSourceDiagnostic(
            path,
            L"media_source_error stage=%s hresult=0x%08X",
            stage,
            static_cast<unsigned>(result));
    }
    return result;
}

namespace wilEx
{
    //template <typename T>
    //using make_unique_cotaskmem_array = unique_any_array_ptr<typename details::element_traits<T>::type>;

    template<typename T>
    wil::unique_cotaskmem_array_ptr<T> make_unique_cotaskmem_array(size_t numOfElements)
    {
        wil::unique_cotaskmem_array_ptr<T> arr;
        size_t cb = sizeof(wil::details::element_traits<T>::type) * numOfElements;
        void* ptr = ::CoTaskMemAlloc(cb);
        if (ptr != nullptr)
        {
            ZeroMemory(ptr, cb);
            arr.reset(reinterpret_cast<typename wil::details::element_traits<T>::type*>(ptr), numOfElements);
        }
        return arr;
    }
};

namespace winrt
{
    template<> bool is_guid_of<IMFMediaSourceEx>(guid const& id) noexcept;

    template<> bool is_guid_of<IMFMediaStream2>(guid const& id) noexcept;

    template<> bool is_guid_of<IMFActivate>(guid const& id) noexcept;
};

#define CHECKHR_GOTO( _hr, _lbl ) { hr = _hr; if( FAILED( hr ) ){ DEBUG_MSG(L"hr=0x%08x", _hr); goto _lbl; } }
#define CHECKNULL_GOTO( _ptr, _hr, _lbl ) { if(_ptr == nullptr) {hr = _hr; if( FAILED( hr ) ){ DEBUG_MSG(L"hr=0x%08x", _hr); goto _lbl; } } }