#include <windows.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <iostream>
#include <string>

#include "VirtualCameraMediaSource.h"

namespace {

using Microsoft::WRL::ComPtr;

void ReportFailure(const wchar_t* action, HRESULT result) {
    std::wcerr << L"Ошибка: " << action << L" (код 0x"
               << std::hex << static_cast<unsigned long>(result) << L").\n";
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

int wmain() {
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
        L"Видоискатель — тестовый источник",
        VIRTUALCAMERAMEDIASOURCE_CLSID,
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

    result = camera->Start();
    if (FAILED(result)) {
        ReportFailure(L"не удалось запустить тестовую виртуальную камеру", result);
        camera->Shutdown();
        return 1;
    }

    std::wcout << L"Тестовая камера запущена. Откройте приложение «Камера» Windows,"
                  L" Chrome, Edge или OBS.\n"
                  L"Чтобы остановить её и убрать из списка камер, нажмите Ввод.\n";
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
    std::wcout << L"Тестовая камера остановлена и удалена.\n";
    return 0;
}
