#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>

#include "plugin2.h"
#include "config2.h"

namespace fs = std::filesystem;

static HWND registeredWindow = nullptr;
static EDIT_HANDLE editHandle{};

static void RegisterWindow(LPCWSTR, HWND hwnd) { registeredWindow = hwnd; }
static EDIT_HANDLE* CreateEditHandle() { return &editHandle; }
static HWND GetHostAppWindow() { return registeredWindow; }
static void GetEditInfo(EDIT_INFO* info, int size) {
    EDIT_INFO sample{};
    sample.rate = 30;
    sample.scale = 1;
    sample.sample_rate = 48000;
    sample.layer = 0;
    sample.frame = 0;
    memcpy(info, &sample, min(size, static_cast<int>(sizeof(sample))));
}

static std::wstring Text(HWND hwnd) {
    int size = GetWindowTextLengthW(hwnd);
    std::wstring value(size + 1, L'\0');
    GetWindowTextW(hwnd, value.data(), size + 1);
    value.resize(size);
    return value;
}

static std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

static void Pump(DWORD milliseconds) {
    DWORD end = GetTickCount() + milliseconds;
    MSG msg{};
    while (static_cast<LONG>(GetTickCount() - end) < 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::wcerr << L"usage: smoke_host plugin output-folder ffmpeg\n";
        return 2;
    }
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) {
        std::wcerr << L"LoadLibrary failed: " << GetLastError() << L"\n";
        return 3;
    }
    auto initialize = reinterpret_cast<bool(*)(DWORD)>(GetProcAddress(module, "InitializePlugin"));
    auto initializeConfig = reinterpret_cast<void(*)(CONFIG_HANDLE*)>(GetProcAddress(module, "InitializeConfig"));
    auto registerPlugin = reinterpret_cast<void(*)(HOST_APP_TABLE*)>(GetProcAddress(module, "RegisterPlugin"));
    auto uninitialize = reinterpret_cast<void(*)()>(GetProcAddress(module, "UninitializePlugin"));
    if (!initialize || !initializeConfig || !registerPlugin || !uninitialize) return 4;

    std::wstring configRoot = fs::path(argv[2]).parent_path().wstring();
    CONFIG_HANDLE config{};
    config.app_data_path = configRoot.c_str();
    initializeConfig(&config);
    if (!initialize(2010701)) return 5;
    editHandle.get_edit_info = GetEditInfo;
    editHandle.get_host_app_window = GetHostAppWindow;
    HOST_APP_TABLE host{};
    host.register_window_client = RegisterWindow;
    host.create_edit_handle = CreateEditHandle;
    registerPlugin(&host);
    if (!registeredWindow) {
        std::wcerr << L"No plugin window was registered.\n";
        return 6;
    }

    SetWindowTextW(GetDlgItem(registeredWindow, 1007), argv[2]);
    SetWindowTextW(GetDlgItem(registeredWindow, 1009), argv[3]);
    SendMessageW(GetDlgItem(registeredWindow, 1011), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(registeredWindow, 1012), BM_CLICK, 0, 0);
    Pump(2000);
    SendMessageW(GetDlgItem(registeredWindow, 1013), BM_CLICK, 0, 0);
    Pump(3000);

    std::cout << "status=" << Utf8(Text(GetDlgItem(registeredWindow, 1015))) << "\n";
    size_t files = 0;
    if (fs::exists(argv[2])) {
        for (const auto& item : fs::directory_iterator(argv[2])) if (item.is_regular_file()) ++files;
    }
    DestroyWindow(registeredWindow);
    uninitialize();
    FreeLibrary(module);
    std::cout << "recording-files=" << files << "\n";
    return files > 0 ? 0 : 7;
}
