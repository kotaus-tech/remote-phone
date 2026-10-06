#include <windows.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

#include "VirtualCameraMediaSource.h"

namespace {

using Microsoft::WRL::ComPtr;

void ConfigureConsole() {
    DWORD mode = 0;
    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) {
        _setmode(_fileno(stdin), _O_U16TEXT);
    }
}

void WriteWideText(DWORD standardHandle, const wchar_t* text) {
    HANDLE handle = GetStdHandle(standardHandle);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || text == nullptr) {
        return;
    }

    const int characterCount = static_cast<int>(wcslen(text));
    if (characterCount == 0) {
        return;
    }

    DWORD consoleMode = 0;
    if (GetConsoleMode(handle, &consoleMode)) {
        DWORD charactersWritten = 0;
        WriteConsoleW(handle, text, static_cast<DWORD>(characterCount), &charactersWritten, nullptr);
        return;
    }

    UINT outputCodePage = GetConsoleOutputCP();
    if (outputCodePage == 0) {
        outputCodePage = CP_UTF8;
    }
    const int byteCount = WideCharToMultiByte(outputCodePage, 0, text, characterCount, nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0) {
        return;
    }
    std::vector<char> encoded(static_cast<size_t>(byteCount));
    if (WideCharToMultiByte(outputCodePage, 0, text, characterCount, encoded.data(), byteCount, nullptr, nullptr) != byteCount) {
        return;
    }
    DWORD bytesWritten = 0;
    if (!WriteFile(handle, encoded.data(), static_cast<DWORD>(byteCount), &bytesWritten, nullptr) ||
        bytesWritten != static_cast<DWORD>(byteCount)) {
        return;
    }
}

std::wstring GetDiagnosticLogPath() {
    constexpr wchar_t kEnvironmentVariable[] = L"REMOTE_PHONE_VCAM_DIAGNOSTIC_LOG_PATH";
    const DWORD requiredCharacters = GetEnvironmentVariableW(kEnvironmentVariable, nullptr, 0);
    if (requiredCharacters == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(requiredCharacters);
    const DWORD charactersWritten = GetEnvironmentVariableW(
        kEnvironmentVariable,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (charactersWritten == 0 || charactersWritten >= static_cast<DWORD>(buffer.size())) {
        return {};
    }
    return std::wstring(buffer.data(), charactersWritten);
}

void ReportFailure(const wchar_t* action, HRESULT result) {
    wchar_t message[512] = {};
    swprintf_s(
        message,
        ARRAYSIZE(message),
        L"Ошибка: %s (код 0x%08lX).\n",
        action,
        static_cast<unsigned long>(result));
    WriteWideText(STD_ERROR_HANDLE, message);
}

class Apartment final {
public:
    HRESULT Initialize() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_initialized = SUCCEEDED(result);
        return result;
    }

    ~Apartment() {
        if (m_initialized) {
            CoUninitialize();
        }
    }

private:
    bool m_initialized = false;
};

class MediaFoundation final {
public:
    HRESULT Initialize() {
        const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        m_initialized = SUCCEEDED(result);
        return result;
    }

    ~MediaFoundation() {
        if (m_initialized) {
            MFShutdown();
        }
    }

private:
    bool m_initialized = false;
};

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    ConfigureConsole();

    const bool isolatedDiagnostic = argc == 2 && std::wcscmp(argv[1], L"--isolated-clsid") == 0;
    if (argc > 1 && !isolatedDiagnostic) {
        WriteWideText(STD_ERROR_HANDLE, L"Неизвестный параметр. Для изолированного COM-теста используйте --isolated-clsid.\n");
        return 2;
    }
    const wchar_t* cameraFriendlyName = isolatedDiagnostic
        ? L"Видоискатель — COM-изоляционный тест"
        : L"Видоискатель — тестовый источник";
    const wchar_t* sourceClsid = isolatedDiagnostic
        ? VIRTUALCAMERAMEDIASOURCE_ISOLATED_CLSID
        : VIRTUALCAMERAMEDIASOURCE_CLSID;

    Apartment apartment;
    HRESULT result = apartment.Initialize();
    if (FAILED(result)) {
        ReportFailure(L"не удалось подготовить COM", result);
        return 1;
    }

    MediaFoundation mediaFoundation;
    result = mediaFoundation.Initialize();
    if (FAILED(result)) {
        ReportFailure(L"не удалось запустить Media Foundation", result);
        return 1;
    }

    ComPtr<IMFVirtualCamera> camera;
    result = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        cameraFriendlyName,
        sourceClsid,
        nullptr,
        0,
        camera.GetAddressOf());
    if (FAILED(result)) {
        ReportFailure(L"не удалось создать тестовую виртуальную камеру", result);
        return 1;
    }

    result = camera->SetUINT32(
        VCAM_KIND,
        static_cast<UINT32>(VirtualCameraKind::Synthetic));
    if (FAILED(result)) {
        ReportFailure(L"не удалось выбрать синтетический источник", result);
        camera->Shutdown();
        return 1;
    }

    const std::wstring diagnosticLogPath = GetDiagnosticLogPath();
    if (!diagnosticLogPath.empty()) {
        result = camera->SetString(VCAM_DIAGNOSTIC_LOG_PATH, diagnosticLogPath.c_str());
        if (FAILED(result)) {
            ReportFailure(L"не удалось передать путь журнала диагностики", result);
            camera->Shutdown();
            return 1;
        }
    }

    result = camera->Start(nullptr);
    if (FAILED(result)) {
        ReportFailure(L"не удалось запустить тестовую виртуальную камеру", result);
        camera->Shutdown();
        return 1;
    }

    const wchar_t* startedMessage = isolatedDiagnostic
        ? L"Изолированная COM-тестовая камера запущена. Выберите «Видоискатель — COM-изоляционный тест» в Chrome, Edge, Discord или OBS.\n"
          L"Откройте приложения по одному; затем нажмите Ввод, чтобы остановить камеру и убрать её из списка.\n"
        : L"Тестовая камера запущена. Откройте приложение «Камера» Windows, Chrome, Edge, Discord или OBS.\n"
          L"Чтобы остановить её и убрать из списка камер, нажмите Ввод.\n";
    WriteWideText(STD_OUTPUT_HANDLE, startedMessage);
    std::wstring input;
    std::getline(std::wcin, input);

    const HRESULT stopResult = camera->Stop();
    if (FAILED(stopResult)) {
        ReportFailure(L"не удалось остановить тестовую камеру", stopResult);
    }
    const HRESULT removeResult = camera->Remove();
    if (FAILED(removeResult)) {
        ReportFailure(L"не удалось удалить тестовую камеру", removeResult);
    }
    const HRESULT shutdownResult = camera->Shutdown();
    if (FAILED(shutdownResult)) {
        ReportFailure(L"не удалось завершить работу тестовой камеры", shutdownResult);
    }
    camera.Reset();

    if (FAILED(stopResult) || FAILED(removeResult) || FAILED(shutdownResult)) {
        return 1;
    }
    WriteWideText(
        STD_OUTPUT_HANDLE,
        isolatedDiagnostic
            ? L"Изолированная COM-тестовая камера остановлена и удалена.\n"
            : L"Тестовая камера остановлена и удалена.\n");
    return 0;
}
