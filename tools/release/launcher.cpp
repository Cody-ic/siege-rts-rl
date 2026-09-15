// Small Unicode GUI entry point for the portable player package.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    wchar_t module[32768]{};
    const DWORD size=GetModuleFileNameW(nullptr,module,32768);
    if(size==0 || size>=32768) return 1;
    const auto root=std::filesystem::path(module).parent_path();
    const auto data=root/L"游戏数据";
    const auto exe=data/L"rts_render.exe";
    const auto font=data/L"NotoSansSC.ttf";
    if(!std::filesystem::exists(exe) || !std::filesystem::exists(font)) {
        MessageBoxW(nullptr,L"请先完整解压下载的 ZIP，再启动圣城.exe。请勿单独移动启动程序或游戏数据文件夹。",L"圣城",MB_OK|MB_ICONERROR);
        return 1;
    }
    wchar_t local[32768]{};
    const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    const auto logs=(n>0 && n<32768?std::filesystem::path(local):root)/L"SiegeRTS";
    std::error_code ec;
    std::filesystem::create_directories(logs,ec);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE log=CreateFileW((logs/L"launcher.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    if(log!=INVALID_HANDLE_VALUE && input!=INVALID_HANDLE_VALUE) {
        startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdOutput=log;startup.hStdError=log;startup.hStdInput=input;
    }
    // Keep the renderer's engineering-build font priority. Its last fallback
    // lives in the child working directory (the packaged game data folder).
    std::wstring command=L"\""+exe.wstring()+L"\" "+arguments;
    PROCESS_INFORMATION process{};
    const BOOL started=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,data.c_str(),&startup,&process);
    if(log!=INVALID_HANDLE_VALUE) CloseHandle(log);
    if(input!=INVALID_HANDLE_VALUE) CloseHandle(input);
    if(!started) {
        MessageBoxW(nullptr,L"游戏启动失败。请完整解压发布包，并确认使用 Windows 10/11 64 位系统。",L"圣城",MB_OK|MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess,INFINITE);
    DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);
    if(code!=0) {
        const auto message=L"游戏未能正常运行。诊断日志：\n"+(logs/L"launcher.log").wstring();
        MessageBoxW(nullptr,message.c_str(),L"圣城",MB_OK|MB_ICONERROR);
    }
    return static_cast<int>(code);
}
