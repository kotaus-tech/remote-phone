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
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern HINSTANCE g_hInst;
#pragma comment(lib, "bcrypt.lib")

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

inline std::wstring GetMediaSourceFileHash(_In_z_ PCWSTR modulePath)
{
    if (modulePath == nullptr || modulePath[0] == L'\0')
    {
        return L"unavailable";
    }

    HANDLE file = CreateFileW(
        modulePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return L"unavailable";
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring result = L"unavailable";
    try
    {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        {
            CloseHandle(file);
            return result;
        }

        ULONG objectLength = 0;
        ULONG hashLength = 0;
        ULONG bytesReturned = 0;
        if (BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength),
                sizeof(objectLength),
                &bytesReturned,
                0) < 0 ||
            BCryptGetProperty(
                algorithm,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hashLength),
                sizeof(hashLength),
                &bytesReturned,
                0) < 0 ||
            hashLength != 32)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            CloseHandle(file);
            return result;
        }

        std::vector<BYTE> hashObject(objectLength);
        if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            CloseHandle(file);
            return result;
        }

        BYTE buffer[64 * 1024] = {};
        bool readSucceeded = true;
        for (;;)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr))
            {
                readSucceeded = false;
                break;
            }
            if (bytesRead == 0)
            {
                break;
            }
            if (BCryptHashData(hash, buffer, bytesRead, 0) < 0)
            {
                readSucceeded = false;
                break;
            }
        }

        std::array<BYTE, 32> digest{};
        if (readSucceeded && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0)
        {
            constexpr wchar_t digits[] = L"0123456789ABCDEF";
            result.clear();
            result.reserve(digest.size() * 2);
            for (const BYTE value : digest)
            {
                result.push_back(digits[(value >> 4) & 0x0F]);
                result.push_back(digits[value & 0x0F]);
            }
        }
    }
    catch (...)
    {
        result = L"unavailable";
    }

    if (hash != nullptr)
    {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    CloseHandle(file);
    return result;
}

inline std::wstring GetMediaSourceModulePath()
{
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(g_hInst, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        return L"unavailable";
    }
    return std::wstring(path.data(), length);
}

inline std::wstring GetCurrentProcessImagePath()
{
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        return L"unavailable";
    }
    return std::wstring(path.data(), length);
}

inline void AppendMediaSourceDiagnosticRecord(_In_opt_z_ PCWSTR path, _In_z_ PCWSTR record)
{
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
        OPEN_ALWAYS,
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
            if (WriteFile(file, utf8, static_cast<DWORD>(byteCount), &bytesWritten, nullptr) &&
                bytesWritten == static_cast<DWORD>(byteCount))
            {
                FlushFileBuffers(file);
            }
        }
    }
    CloseHandle(file);
}

inline void WriteMediaSourceRecord(_In_opt_z_ PCWSTR path, _In_z_ PCWSTR message)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    WCHAR record[2048] = {};
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
    AppendMediaSourceDiagnosticRecord(path, record);
}

inline void EnsureMediaSourceIdentityLogged(_In_z_ PCWSTR path)
{
    static std::once_flag identityLogged;
    try
    {
        std::call_once(identityLogged, [path]()
        {
            const std::wstring modulePath = GetMediaSourceModulePath();
            const std::wstring processPath = GetCurrentProcessImagePath();
            const std::wstring processName = [] (const std::wstring& value)
            {
                const size_t separator = value.find_last_of(L"\\/");
                return separator == std::wstring::npos ? value : value.substr(separator + 1);
            }(processPath);
            const std::wstring hash = GetMediaSourceFileHash(modulePath.c_str());
            WCHAR identity[1536] = {};
            StringCbPrintfW(
                identity,
                sizeof(identity),
                L"media_source_identity version=%s sha256=%s pid=%lu process=\"%s\" process_path=\"%s\" dll_path=\"%s\"",
                VIRTUALCAMERAMEDIASOURCE_BUILD_VERSION,
                hash.c_str(),
                static_cast<unsigned long>(GetCurrentProcessId()),
                processName.c_str(),
                processPath.c_str(),
                modulePath.c_str());
            WriteMediaSourceRecord(path, identity);
        });
    }
    catch (...)
    {
        // Diagnostics must never prevent camera activation.
    }
}

inline void WriteMediaSourceDiagnostic(_In_opt_z_ PCWSTR path, _In_z_ PCWSTR format, ...)
{
    if (path != nullptr && path[0] != L'\0')
    {
        EnsureMediaSourceIdentityLogged(path);
    }

    WCHAR message[1024] = {};
    va_list args;
    va_start(args, format);
    StringCbVPrintfW(message, sizeof(message), format, args);
    va_end(args);
    WriteMediaSourceRecord(path, message);
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