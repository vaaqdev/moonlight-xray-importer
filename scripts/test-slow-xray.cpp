// TEST ONLY: simulate a laptop whose security scanner delays Xray by 8 seconds.
// Build this in a dedicated fixture directory, never ship it in the portable.
#include <windows.h>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    Sleep(8000);
    wchar_t module[32768];
    if (!GetModuleFileNameW(nullptr, module, 32768)) return 125;
    std::wstring real(module);
    real = real.substr(0, real.find_last_of(L"\\/")) + L"\\xray-real.exe";
    std::wstring command = L"\"" + real + L"\"";
    for (int i = 1; i < argc; ++i) command += L" \"" + std::wstring(argv[i]) + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(real.c_str(), &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 126;
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 127;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return int(code);
}
