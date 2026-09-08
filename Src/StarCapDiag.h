#pragma once
#include <Windows.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <format>

namespace StarCapDiag
{
    inline std::wstring logPath()
    {
        // Diagnostics belong in the user's writable application-data directory,
        // not beside StarCap.exe. This keeps portable/download folders clean and
        // also works when StarCap is launched from protected locations such as
        // Program Files.
        PWSTR roaming{};
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &roaming)) && roaming) {
            std::filesystem::path dir = std::filesystem::path(roaming) / L"StarCap" / L"Logs";
            CoTaskMemFree(roaming);

            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (!ec || std::filesystem::is_directory(dir)) {
                return (dir / L"StarCap_Diagnostics.log").wstring();
            }
        }
        else if (roaming) {
            CoTaskMemFree(roaming);
        }

        // AppData should normally be available. If it is not, fall back to the
        // user's temporary directory rather than attempting to write beside the EXE.
        wchar_t temp[MAX_PATH + 1]{};
        const DWORD len = GetTempPathW(MAX_PATH + 1, temp);
        if (len > 0 && len < MAX_PATH + 1) {
            std::filesystem::path dir = std::filesystem::path(temp) / L"StarCap" / L"Logs";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (!ec || std::filesystem::is_directory(dir)) {
                return (dir / L"StarCap_Diagnostics.log").wstring();
            }
        }
        return L"";
    }

    inline void append(const std::wstring& message)
    {
        SYSTEMTIME st{}; GetLocalTime(&st);
        auto line = std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} {}\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, message);
        int n = WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return;
        std::string utf8((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(), utf8.data(), n, nullptr, nullptr);
        auto path = logPath();
        if (path.empty()) return;
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0; WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(h);
    }
}

