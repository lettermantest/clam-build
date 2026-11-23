// Clam Build — entry point.
#include <windows.h>
#include <shellapi.h>

#include <string>

#include "config.h"
#include "ui.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show_command) {
    (void)command_line;

    // Optional headless helpers mirroring the terminal build's flags.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring flag = argv[i];
            if (flag == L"--install") {
                std::wstring exe = L"";
                wchar_t path[MAX_PATH];
                if (GetModuleFileNameW(nullptr, path, MAX_PATH)) exe = path;
                std::string local = clam::config_dir();
                (void)local;
                MessageBoxW(nullptr,
                            L"Clam Build is portable: move Clam Build.exe anywhere and pin it.\n"
                            L"Settings and your key live in %APPDATA%\\clam.",
                            L"Clam Build", MB_OK | MB_ICONINFORMATION);
                LocalFree(argv);
                return 0;
            }
        }
        LocalFree(argv);
    }

    return clam::RunClamBuildApp(instance, show_command);
}
