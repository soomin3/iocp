#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../LifetimeCatalog.h"

namespace
{
using namespace LifetimeVerification;
namespace fs = std::filesystem;
constexpr UINT UpdateMessage = WM_APP + 1;
constexpr int RunSelectedId = 100;
constexpr int RunAllId = 101;
constexpr int CancelId = 102;
constexpr int OpenResultsId = 103;
constexpr int RepeatId = 104;
constexpr int ScopeId = 105;
constexpr int ScenarioCount = static_cast<int>(std::size(kScenarios));
constexpr int GroupColumn = 0;
constexpr int NameColumn = 1;
constexpr int DescriptionColumn = 2;
constexpr int StateColumn = 3;
constexpr int ElapsedColumn = 4;
constexpr COLORREF Background = RGB(244, 247, 250);
constexpr COLORREF Ink = RGB(23, 42, 59);
constexpr COLORREF Muted = RGB(93, 110, 126);
constexpr COLORREF Accent = RGB(0, 119, 120);

class Handle
{
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};

std::wstring FromUtf8(const std::string& value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string ToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring ErrorText(const wchar_t* operation, DWORD error = GetLastError())
{
    return std::wstring(operation) + L" 실패 (Win32 " + std::to_wstring(error) + L")";
}

fs::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return fs::path(path).parent_path();
}

std::wstring Timestamp()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t value[80]{};
    swprintf_s(value, L"%04u%02u%02u-%02u%02u%02u-%03u-%lu",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentProcessId());
    return value;
}

enum class TestScope { All, Common, Game };

bool InScope(TestGroup group, TestScope scope)
{
    return scope == TestScope::All ||
        (scope == TestScope::Common && group == TestGroup::Common) ||
        (scope == TestScope::Game && group == TestGroup::Game);
}

struct GroupTotals
{
    int selected = 0;
    int passed = 0;
    int failed = 0;
    int cancelled = 0;
    int Completed() const { return passed + failed + cancelled; }
};

int GroupIndex(TestGroup group)
{
    return group == TestGroup::Common ? 0 : 1;
}

std::wstring GroupSummary(TestGroup group, const GroupTotals& totals)
{
    if (!totals.selected) return std::wstring(GroupName(group)) + L": 이번 실행 대상 없음";
    return std::wstring(GroupName(group)) + L": 통과 " + std::to_wstring(totals.passed) +
        L" / 실패 " + std::to_wstring(totals.failed) + L" / 중지 " + std::to_wstring(totals.cancelled) +
        L" / 미실행 " + std::to_wstring(totals.selected - totals.Completed()) +
        L"  (선택 " + std::to_wstring(totals.selected) + L"개)";
}

struct Options
{
    bool smoke = false;
    TestScope scope = TestScope::All;
    DWORD cancelAfterMs = 0;
    DWORD timeoutMs = 0;
    fs::path snapshot;
};

enum class EventKind { Log, Started, Completed, Finished };
struct UiEvent
{
    EventKind kind = EventKind::Log;
    int index = -1;
    int completed = 0;
    int passed = 0;
    int failed = 0;
    int cancelled = 0;
    int exitCode = 0;
    std::wstring text;
    std::wstring elapsed;
    fs::path directory;
};

void PostUpdate(HWND window, std::unique_ptr<UiEvent> event)
{
    if (PostMessageW(window, UpdateMessage, 0, reinterpret_cast<LPARAM>(event.get())))
        event.release();
}

void PostLog(HWND window, const std::wstring& text)
{
    auto event = std::make_unique<UiEvent>();
    event->text = text;
    PostUpdate(window, std::move(event));
}

enum class Outcome { Passed, Failed, Cancelled, TimedOut };
struct CaseResult
{
    Outcome outcome = Outcome::Failed;
    DWORD exitCode = 1;
    std::wstring detail;
};

const wchar_t* OutcomeName(Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::Passed: return L"PASS";
    case Outcome::Cancelled: return L"CANCELLED";
    case Outcome::TimedOut: return L"TIMEOUT";
    default: return L"FAIL";
    }
}

CaseResult ExecuteCase(HWND window, HANDLE cancel, const fs::path& directory, int index, int repeat, DWORD timeoutOverride)
{
    const auto& scenario = kScenarios[index];
    std::ofstream log(directory / (std::wstring(scenario.id) + L".log"), std::ios::binary);
    if (!log) return { Outcome::Failed, 1, L"시나리오 로그 파일을 만들 수 없습니다." };

    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0))
        return { Outcome::Failed, GetLastError(), ErrorText(L"출력 파이프 생성") };
    Handle outputRead(readPipe);
    Handle outputWrite(writePipe);
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0))
        return { Outcome::Failed, GetLastError(), ErrorText(L"파이프 상속 설정") };
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    if (!input) return { Outcome::Failed, GetLastError(), ErrorText(L"표준 입력 생성") };

    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
    jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo)))
        return { Outcome::Failed, GetLastError(), ErrorText(L"프로세스 Job 생성") };

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributes(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes))
        return { Outcome::Failed, GetLastError(), ErrorText(L"프로세스 속성 초기화") };
    struct AttributeCleanup
    {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttributeCleanup() { DeleteProcThreadAttributeList(list); }
    } attributeCleanup{ attributeList };
    HANDLE inheritedHandles[]{ outputWrite.Get(), input.Get() };
    if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr))
        return { Outcome::Failed, GetLastError(), ErrorText(L"프로세스 핸들 상속 설정") };

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.Get();
    startup.StartupInfo.hStdOutput = outputWrite.Get();
    startup.StartupInfo.hStdError = outputWrite.Get();
    startup.lpAttributeList = attributeList;
    const auto executable = ExecutableDirectory() / L"LifetimeTests.exe";
    std::wstring command = L"\"" + executable.wstring() + L"\" --case " + scenario.id + L" --repeat " + std::to_wstring(repeat);
    log << ToUtf8(command) << "\n";
    PostLog(window, L"> " + command + L"\r\n");
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process))
        return { Outcome::Failed, GetLastError(), ErrorText(L"LifetimeTests.exe 실행") };
    Handle processHandle(process.hProcess);
    Handle threadHandle(process.hThread);
    if (!AssignProcessToJobObject(job.Get(), process.hProcess))
    {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, error);
        WaitForSingleObject(process.hProcess, INFINITE);
        return { Outcome::Failed, error, ErrorText(L"자식 프로세스 Job 할당", error) };
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
    {
        const DWORD error = GetLastError();
        TerminateJobObject(job.Get(), error);
        WaitForSingleObject(process.hProcess, INFINITE);
        return { Outcome::Failed, error, ErrorText(L"자식 프로세스 시작", error) };
    }

    const ULONGLONG started = GetTickCount64();
    const DWORD timeout = timeoutOverride ? timeoutOverride : std::max<DWORD>(30000, static_cast<DWORD>(repeat) * 20000 + 10000);
    CaseResult result;
    std::string pendingLine;
    auto drainOutput = [&]()
    {
        for (int batch = 0; batch < 16; ++batch)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
            char buffer[4096]{};
            DWORD bytes = 0;
            if (!ReadFile(readPipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &bytes, nullptr) || bytes == 0) break;
            log.write(buffer, bytes);
            pendingLine.append(buffer, bytes);
            const auto lastNewline = pendingLine.find_last_of('\n');
            if (lastNewline != std::string::npos)
            {
                PostLog(window, FromUtf8(pendingLine.substr(0, lastNewline + 1)));
                pendingLine.erase(0, lastNewline + 1);
            }
            if (pendingLine.size() > 65536)
            {
                PostLog(window, FromUtf8(pendingLine));
                pendingLine.clear();
            }
        }
    };
    for (;;)
    {
        drainOutput();
        const DWORD wait = WaitForSingleObject(process.hProcess, 20);
        if (wait == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(process.hProcess, &result.exitCode);
            result.outcome = result.exitCode == 0 ? Outcome::Passed : result.exitCode == 3 ? Outcome::TimedOut : Outcome::Failed;
            if (result.exitCode == 3) result.detail = L"Runner의 반복 1회당 20초 제한 시간을 초과했습니다.";
            break;
        }
        if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)
        {
            result.outcome = Outcome::Cancelled;
            result.exitCode = ERROR_CANCELLED;
            result.detail = L"사용자 중지: 자식 프로세스 Job을 종료하고 회수했습니다.";
            break;
        }
        if (GetTickCount64() - started >= timeout)
        {
            result.outcome = Outcome::TimedOut;
            result.exitCode = WAIT_TIMEOUT;
            result.detail = L"시나리오 제한 시간 초과: 자식 프로세스 Job을 종료하고 회수했습니다.";
            break;
        }
        if (wait == WAIT_FAILED)
        {
            result.outcome = Outcome::Failed;
            result.exitCode = GetLastError();
            result.detail = ErrorText(L"자식 프로세스 대기", result.exitCode);
            break;
        }
    }
    if (result.outcome == Outcome::Cancelled || result.outcome == Outcome::TimedOut || !result.detail.empty())
    {
        TerminateJobObject(job.Get(), result.exitCode);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    drainOutput();
    if (!pendingLine.empty()) PostLog(window, FromUtf8(pendingLine));
    log << "\n[" << ToUtf8(OutcomeName(result.outcome)) << "] exit=" << result.exitCode << " " << ToUtf8(result.detail) << "\n";
    log.flush();
    if (!log)
        return { Outcome::Failed, ERROR_WRITE_FAULT, L"로그 저장에 실패했습니다. 결과를 통과로 처리하지 않습니다." };
    return result;
}

void RunCases(HWND window, HANDLE cancel, std::vector<int> selected, int repeat, DWORD timeout)
{
    auto finished = std::make_unique<UiEvent>();
    finished->kind = EventKind::Finished;
    try
    {
        finished->directory = ExecutableDirectory() / L"results" / Timestamp();
        fs::create_directories(finished->directory);
        std::ofstream summary(finished->directory / L"summary.tsv", std::ios::binary);
        if (!summary) throw std::runtime_error("Cannot create summary.tsv");
        summary << "case\tgroup\tresult\texit_code\telapsed_ms\trepeat\tdetail\n";
        std::array<GroupTotals, 2> totals{};
        for (int index : selected) ++totals[GroupIndex(kScenarios[index].group)].selected;
        PostLog(window, L"검증 범위: 서버 공통 수명·IOCP / 게임 서버 로그인·닉네임·채팅.\r\n"
            L"실제 loopback 통신 + 모의 DB이며 SQL Server / OLE DB 연동은 포함하지 않습니다.\r\n");
        PostLog(window, L"결과 폴더: " + finished->directory.wstring() + L"\r\n\r\n");
        for (int index : selected)
        {
            if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) break;
            auto started = std::make_unique<UiEvent>();
            started->kind = EventKind::Started;
            started->index = index;
            PostUpdate(window, std::move(started));
            const ULONGLONG begin = GetTickCount64();
            const auto result = ExecuteCase(window, cancel, finished->directory, index, repeat, timeout);
            const ULONGLONG elapsed = GetTickCount64() - begin;
            summary << ToUtf8(kScenarios[index].id) << '\t' << ToUtf8(GroupId(kScenarios[index].group)) << '\t'
                << ToUtf8(OutcomeName(result.outcome)) << '\t'
                << result.exitCode << '\t' << elapsed << '\t' << repeat << '\t' << ToUtf8(result.detail) << '\n';
            summary.flush();
            auto completed = std::make_unique<UiEvent>();
            completed->kind = EventKind::Completed;
            completed->index = index;
            completed->text = OutcomeName(result.outcome);
            completed->elapsed = std::to_wstring(elapsed) + L" ms";
            PostUpdate(window, std::move(completed));
            ++finished->completed;
            auto& group = totals[GroupIndex(kScenarios[index].group)];
            if (result.outcome == Outcome::Passed) { ++finished->passed; ++group.passed; }
            else if (result.outcome == Outcome::Cancelled) { ++finished->cancelled; ++group.cancelled; }
            else { ++finished->failed; ++group.failed; }
            PostLog(window, L"[" + std::wstring(GroupName(kScenarios[index].group)) + L" / " + OutcomeName(result.outcome) + L"] " + kScenarios[index].name +
                L" · " + std::to_wstring(elapsed) + L" ms · exit " + std::to_wstring(result.exitCode) + L"\r\n" + result.detail + L"\r\n\r\n");
            if (result.outcome == Outcome::Cancelled) break;
        }
        const bool cancelled = WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0;
        finished->exitCode = finished->failed ? 1 : cancelled ? 2 : 0;
        const int skipped = static_cast<int>(selected.size()) - finished->completed;
        finished->text = L"통과 " + std::to_wstring(finished->passed) + L" / 실패 " + std::to_wstring(finished->failed) +
            L" / 중지 " + std::to_wstring(finished->cancelled) + L" / 미실행 " + std::to_wstring(skipped);
        summary << "\nsummary\tall_selected\t" << (finished->exitCode == 0 ? "PASS" : finished->exitCode == 2 ? "CANCELLED" : "FAIL")
            << '\t' << finished->exitCode << "\n";
        summary.close();
        if (!summary) throw std::runtime_error("Cannot save summary.tsv");
        std::ofstream groupSummary(finished->directory / L"group_summary.tsv", std::ios::binary);
        if (!groupSummary) throw std::runtime_error("Cannot create group_summary.tsv");
        groupSummary << "group\tselected\tpassed\tfailed\tcancelled\tnot_run\trepeat\n";
        for (const TestGroup group : { TestGroup::Common, TestGroup::Game })
        {
            const auto& count = totals[GroupIndex(group)];
            groupSummary << ToUtf8(GroupId(group)) << '\t' << count.selected << '\t' << count.passed << '\t' << count.failed
                << '\t' << count.cancelled << '\t' << count.selected - count.Completed() << '\t' << repeat << '\n';
            PostLog(window, GroupSummary(group, count) + L"\r\n");
        }
        groupSummary.close();
        if (!groupSummary) throw std::runtime_error("Cannot save group_summary.tsv");
        std::ofstream latest(ExecutableDirectory() / L"results" / L"latest.txt", std::ios::binary);
        latest << ToUtf8(finished->directory.wstring()) << "\n";
    }
    catch (const std::exception& error)
    {
        finished->exitCode = 1;
        ++finished->failed;
        finished->text = L"검증 실행 / 결과 저장 실패: " + FromUtf8(error.what());
        PostLog(window, finished->text + L"\r\n");
    }
    PostUpdate(window, std::move(finished));
}

bool SaveSnapshot(HWND window, const fs::path& path)
{
    RECT bounds{};
    GetWindowRect(window, &bounds);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    HDC screen = GetDC(window);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    const auto previous = SelectObject(memory, bitmap);
    const bool printed = PrintWindow(window, memory, PW_RENDERFULLCONTENT) != FALSE;
    SelectObject(memory, previous);
    bool saved = false;
    if (printed)
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        const CLSID png{ 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
        saved = image.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, screen);
    return saved;
}

class Workbench
{
public:
    explicit Workbench(Options options) : options_(std::move(options)), cancel_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Workbench()
    {
        if (worker_.joinable()) { SetEvent(cancel_.Get()); worker_.join(); }
        for (HFONT font : { normalFont_, titleFont_, labelFont_, logFont_ }) if (font) DeleteObject(font);
        if (background_) DeleteObject(background_);
    }
    int ExitCode() const { return exitCode_; }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Workbench*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Workbench*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }
private:
    int Scale(int value) const { return MulDiv(value, dpi_, 96); }
    HWND Control(const wchar_t* kind, const wchar_t* text, DWORD style, int id = 0, DWORD extended = 0)
    {
        HWND control = CreateWindowExW(extended, kind, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont_), FALSE);
        return control;
    }
    void SetCell(int row, int column, const std::wstring& value)
    {
        ListView_SetItemText(list_, row, column, const_cast<wchar_t*>(value.c_str()));
    }
    TestScope Scope() const
    {
        return static_cast<TestScope>(SendMessageW(scope_, CB_GETCURSEL, 0, 0));
    }
    void UpdateGroupSummary()
    {
        SetWindowTextW(groupSummary_, (GroupSummary(TestGroup::Common, totals_[0]) + L"\r\n" +
            GroupSummary(TestGroup::Game, totals_[1])).c_str());
    }
    void AppendLog(std::wstring text)
    {
        // The complete output stays in each case log; cap the on-screen edit control.
        std::wstring normalized;
        normalized.reserve(text.size() + 32);
        wchar_t previous = 0;
        for (wchar_t character : text)
        {
            if (character == L'\n' && previous != L'\r') normalized += L'\r';
            normalized += character;
            previous = character;
        }
        const int length = GetWindowTextLengthW(log_);
        if (length + static_cast<int>(normalized.size()) > 500000)
        {
            SendMessageW(log_, EM_SETSEL, 0, 200000);
            SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"[이전 출력은 결과 폴더의 로그에 보관됩니다.]\r\n"));
        }
        SendMessageW(log_, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(normalized.c_str()));
        SendMessageW(log_, EM_SCROLLCARET, 0, 0);
    }
    void CreateControls()
    {
        dpi_ = static_cast<int>(GetDpiForWindow(window_));
        normalFont_ = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
        titleFont_ = CreateFontW(-Scale(28), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
        labelFont_ = CreateFontW(-Scale(13), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
        logFont_ = CreateFontW(-Scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        background_ = CreateSolidBrush(Background);
        scope_ = Control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, ScopeId);
        for (const auto name : { L"전체", L"서버 공통", L"게임 서버" })
            SendMessageW(scope_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(scope_, CB_SETCURSEL, static_cast<WPARAM>(options_.scope), 0);
        selected_ = Control(L"BUTTON", L"범위 내 선택 실행", WS_TABSTOP | BS_PUSHBUTTON, RunSelectedId);
        all_ = Control(L"BUTTON", L"범위 전체 실행", WS_TABSTOP | BS_DEFPUSHBUTTON, RunAllId);
        stop_ = Control(L"BUTTON", L"중지", WS_TABSTOP | BS_PUSHBUTTON, CancelId);
        open_ = Control(L"BUTTON", L"결과 폴더 열기", WS_TABSTOP | BS_PUSHBUTTON, OpenResultsId);
        repeat_ = Control(L"EDIT", options_.smoke ? L"1" : L"10", WS_TABSTOP | ES_NUMBER | ES_RIGHT, RepeatId, WS_EX_CLIENTEDGE);
        SendMessageW(repeat_, EM_SETLIMITTEXT, 4, 0);
        EnableWindow(stop_, FALSE);
        list_ = Control(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 0, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        ListView_SetBkColor(list_, RGB(255, 255, 255));
        ListView_SetTextColor(list_, Ink);
        const wchar_t* columns[]{ L"구분", L"검증 시나리오", L"확인할 동작", L"상태", L"소요 시간" };
        for (int index = 0; index < static_cast<int>(std::size(columns)); ++index)
        {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(columns[index]);
            column.cx = Scale(200);
            ListView_InsertColumn(list_, index, &column);
        }
        for (int index = 0; index < ScenarioCount; ++index)
        {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.pszText = const_cast<wchar_t*>(GroupName(kScenarios[index].group));
            ListView_InsertItem(list_, &item);
            SetCell(index, NameColumn, kScenarios[index].name);
            SetCell(index, DescriptionColumn, kScenarios[index].description);
            SetCell(index, StateColumn, L"미실행");
            ListView_SetCheckState(list_, index, TRUE);
        }
        progress_ = Control(PROGRESS_CLASSW, L"", PBS_SMOOTH);
        SendMessageW(progress_, PBM_SETBARCOLOR, 0, Accent);
        status_ = Control(L"STATIC", L"준비됨 · 실행 범위를 고르세요. 목록의 체크와 이전 결과는 유지됩니다.", SS_LEFT);
        groupSummary_ = Control(L"STATIC", L"", SS_LEFT);
        UpdateGroupSummary();
        log_ = Control(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
            0, WS_EX_CLIENTEDGE);
        SendMessageW(log_, WM_SETFONT, reinterpret_cast<WPARAM>(logFont_), FALSE);
        SendMessageW(log_, EM_SETLIMITTEXT, 1000000, 0);
        recent_ = Control(L"STATIC", L"최근 결과 없음", SS_PATHELLIPSIS);
        std::ifstream latest(ExecutableDirectory() / L"results" / L"latest.txt", std::ios::binary);
        std::string previous;
        if (latest && std::getline(latest, previous))
        {
            recentDirectory_ = FromUtf8(previous);
            SetWindowTextW(recent_, (L"최근 결과: " + recentDirectory_.wstring()).c_str());
        }
        AppendLog(L"서버 공통: 연결 종료·IOCP 완료 시 코루틴 / 세션 / 버퍼 수명.\r\n"
            L"게임 서버: 실제 로그인·닉네임·채팅 처리와 연결 상태.\r\n"
            L"‘범위 내 선택 실행’은 범위 안에서 체크한 항목만, ‘범위 전체 실행’은 범위 안의 모든 항목을 실행합니다.\r\n"
            L"시나리오별 실제 프로세스 출력이 표시됩니다. SQL Server / OLE DB 연동은 검증하지 않습니다.\r\n"
            L"선택한 테스트는 순차 실행되며 중지 / 시간 초과 시 자식 프로세스를 회수합니다.\r\n"
            L"기본 제한 시간: 시나리오마다 반복당 20초 + 준비 10초 (최소 30초).\r\n");
        if (options_.smoke) PostMessageW(window_, WM_COMMAND, RunAllId, 0);
    }
    void Place(HWND control, int x, int y, int width, int height)
    {
        MoveWindow(control, Scale(x), Scale(y), Scale(width), Scale(height), TRUE);
    }
    void Layout()
    {
        RECT bounds{};
        GetClientRect(window_, &bounds);
        const int width = MulDiv(bounds.right, 96, dpi_);
        const int height = MulDiv(bounds.bottom, 96, dpi_);
        Place(scope_, 70, 142, 160, 200);
        Place(repeat_, 312, 142, 55, 30);
        Place(selected_, width - 476, 138, 142, 36);
        Place(all_, width - 322, 138, 142, 36);
        Place(stop_, width - 168, 138, 140, 36);
        Place(open_, width - 250, height - 52, 222, 32);
        const int listHeight = std::clamp((height - 465) * 2 / 3, 120, 290);
        Place(list_, 28, 215, width - 56, listHeight);
        ListView_SetColumnWidth(list_, GroupColumn, Scale(108));
        ListView_SetColumnWidth(list_, NameColumn, Scale(250));
        ListView_SetColumnWidth(list_, DescriptionColumn, Scale(width - 56 - 108 - 250 - 90 - 100 - 24));
        ListView_SetColumnWidth(list_, StateColumn, Scale(90));
        ListView_SetColumnWidth(list_, ElapsedColumn, Scale(100));
        Place(status_, 28, 225 + listHeight, width - 56, 25);
        Place(groupSummary_, 28, 253 + listHeight, width - 56, 46);
        Place(progress_, 28, 303 + listHeight, width - 56, 8);
        logTop_ = 355 + listHeight;
        Place(log_, 28, logTop_, width - 56, height - logTop_ - 72);
        Place(recent_, 28, height - 47, width - 300, 26);
        InvalidateRect(window_, nullptr, TRUE);
    }
    void Paint()
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        RECT bounds{};
        GetClientRect(window_, &bounds);
        FillRect(dc, &bounds, background_);
        SetBkMode(dc, TRANSPARENT);
        auto draw = [&](int x, int y, const wchar_t* text, HFONT font, COLORREF color)
        {
            SelectObject(dc, font);
            SetTextColor(dc, color);
            TextOutW(dc, Scale(x), Scale(y), text, static_cast<int>(wcslen(text)));
        };
        draw(28, 20, L"GAME SERVER  /  VERIFICATION", labelFont_, Accent);
        draw(28, 43, L"게임 서버 검증 도구", titleFont_, Ink);
        draw(28, 85, L"서버 공통: IOCP · 수명 · 종료   |   게임 서버: 로그인 · 닉네임 · 채팅", normalFont_, Muted);
        draw(28, 110, L"검증 범위: 실제 loopback IOCP + 모의 DB  ·  SQL Server / OLE DB 연동 테스트 제외", normalFont_, Muted);
        draw(28, 149, L"범위", normalFont_, Ink);
        draw(250, 149, L"반복", normalFont_, Ink);
        draw(378, 149, L"회 (1–1000)", normalFont_, Muted);
        draw(28, 190, L"01  검증 시나리오  ·  구분별 실행 / 이전 결과 유지", labelFont_, Ink);
        draw(28, logTop_ - 27, L"02  실행 로그", labelFont_, Ink);
        EndPaint(window_, &paint);
    }
    void SetRunning(bool running)
    {
        running_ = running;
        EnableWindow(selected_, !running);
        EnableWindow(all_, !running);
        EnableWindow(repeat_, !running);
        EnableWindow(scope_, !running);
        EnableWindow(list_, !running);
        EnableWindow(stop_, running);
    }
    void Start(bool all)
    {
        if (running_) return;
        BOOL translated = FALSE;
        const UINT repeat = GetDlgItemInt(window_, RepeatId, &translated, FALSE);
        if (!translated || repeat < 1 || repeat > 1000)
        {
            SetWindowTextW(status_, L"반복 횟수는 1부터 1000까지 입력하세요.");
            SetFocus(repeat_);
            return;
        }
        if (!cancel_)
        {
            SetWindowTextW(status_, L"취소 이벤트를 생성할 수 없어 실행하지 않았습니다.");
            return;
        }
        std::vector<int> selected;
        for (int index = 0; index < ScenarioCount; ++index)
        {
            if (InScope(kScenarios[index].group, Scope()) && (all || ListView_GetCheckState(list_, index)))
                selected.push_back(index);
        }
        if (selected.empty())
        {
            SetWindowTextW(status_, L"현재 범위에서 실행할 시나리오를 하나 이상 체크하세요.");
            return;
        }
        activeCases_ = selected;
        totals_ = {};
        for (int index : selected)
        {
            SetCell(index, StateColumn, L"대기");
            SetCell(index, ElapsedColumn, L"—");
            ++totals_[GroupIndex(kScenarios[index].group)].selected;
        }
        UpdateGroupSummary();
        selectedCount_ = static_cast<int>(selected.size());
        completedCount_ = 0;
        ResetEvent(cancel_.Get());
        SetWindowTextW(log_, L"");
        SetWindowTextW(status_, L"검증 시작 · 실제 실행 결과를 기다리는 중");
        SendMessageW(progress_, PBM_SETRANGE32, 0, selectedCount_);
        SendMessageW(progress_, PBM_SETSTATE, PBST_NORMAL, 0);
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        SetRunning(true);
        try
        {
            worker_ = std::thread(RunCases, window_, cancel_.Get(), std::move(selected), static_cast<int>(repeat), options_.timeoutMs);
            if (options_.cancelAfterMs) SetTimer(window_, 1, options_.cancelAfterMs, nullptr);
        }
        catch (const std::exception& error)
        {
            SetRunning(false);
            AppendLog(L"작업 스레드 시작 실패: " + FromUtf8(error.what()) + L"\r\n");
            exitCode_ = 1;
            if (options_.smoke) DestroyWindow(window_);
        }
    }
    void Cancel()
    {
        if (!running_) return;
        SetEvent(cancel_.Get());
        EnableWindow(stop_, FALSE);
        SetWindowTextW(status_, L"중지 요청됨 · 실행 중인 프로세스를 회수하는 중");
    }
    void ApplyEvent(std::unique_ptr<UiEvent> event)
    {
        switch (event->kind)
        {
        case EventKind::Log:
            AppendLog(event->text);
            break;
        case EventKind::Started:
            SetCell(event->index, StateColumn, L"실행 중");
            ListView_EnsureVisible(list_, event->index, FALSE);
            SetWindowTextW(status_, (L"실행 중 [" + std::wstring(GroupName(kScenarios[event->index].group)) + L"]: " + kScenarios[event->index].name +
                L"  ·  완료 " + std::to_wstring(completedCount_) + L" / " + std::to_wstring(selectedCount_)).c_str());
            break;
        case EventKind::Completed:
            SetCell(event->index, StateColumn, event->text);
            SetCell(event->index, ElapsedColumn, event->elapsed);
            if (event->text == L"PASS") ++totals_[GroupIndex(kScenarios[event->index].group)].passed;
            else if (event->text == L"CANCELLED") ++totals_[GroupIndex(kScenarios[event->index].group)].cancelled;
            else ++totals_[GroupIndex(kScenarios[event->index].group)].failed;
            UpdateGroupSummary();
            ++completedCount_;
            SendMessageW(progress_, PBM_SETPOS, completedCount_, 0);
            break;
        case EventKind::Finished:
            if (worker_.joinable()) worker_.join();
            KillTimer(window_, 1);
            SetRunning(false);
            exitCode_ = event->exitCode;
            SendMessageW(progress_, PBM_SETSTATE, exitCode_ == 0 ? PBST_NORMAL : exitCode_ == 2 ? PBST_PAUSED : PBST_ERROR, 0);
            // A backwards position update avoids capturing the native progress animation mid-flight.
            SendMessageW(progress_, PBM_SETRANGE32, 0, selectedCount_ + 1);
            SendMessageW(progress_, PBM_SETPOS, completedCount_ + 1, 0);
            SendMessageW(progress_, PBM_SETPOS, completedCount_, 0);
            SendMessageW(progress_, PBM_SETRANGE32, 0, selectedCount_);
            for (int index : activeCases_)
            {
                wchar_t state[32]{};
                ListView_GetItemText(list_, index, StateColumn, state, static_cast<int>(std::size(state)));
                if (wcscmp(state, L"대기") == 0 || wcscmp(state, L"실행 중") == 0) SetCell(index, StateColumn, L"미실행");
            }
            SetWindowTextW(status_, (std::wstring(exitCode_ == 0 ? L"검증 완료  ·  " : exitCode_ == 2 ? L"검증 중지  ·  " : L"검증 실패  ·  ") + event->text).c_str());
            AppendLog(L"실행 요약: " + event->text + L"\r\n");
            if (!event->directory.empty())
            {
                recentDirectory_ = event->directory;
                SetWindowTextW(recent_, (L"최근 결과: " + recentDirectory_.wstring()).c_str());
            }
            if (options_.smoke) SetTimer(window_, 2, 250, nullptr);
            else if (closing_) DestroyWindow(window_);
            break;
        }
    }
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            CreateControls();
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO:
        {
            MONITORINFO monitor{ sizeof(monitor) };
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {
                std::min<LONG>(Scale(980), monitor.rcWork.right - monitor.rcWork.left - 16),
                std::min<LONG>(Scale(650), monitor.rcWork.bottom - monitor.rcWork.top - 16)
            };
            return 0;
        }
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wParam), Ink);
            SetBkColor(reinterpret_cast<HDC>(wParam), Background);
            return reinterpret_cast<LRESULT>(background_);
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case RunSelectedId: Start(false); break;
            case RunAllId: Start(true); break;
            case CancelId: Cancel(); break;
            case ScopeId:
                if (HIWORD(wParam) == CBN_SELCHANGE)
                    SetWindowTextW(status_, L"범위 변경됨 · 선택 실행은 범위 안의 체크 항목, 전체 실행은 범위 안의 모든 항목을 실행합니다.");
                break;
            case OpenResultsId:
            {
                const fs::path target = recentDirectory_.empty() ? ExecutableDirectory() / L"results" : recentDirectory_;
                std::error_code error;
                fs::create_directories(target, error);
                if (!error) ShellExecuteW(window_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                else SetWindowTextW(status_, L"결과 폴더를 열 수 없습니다.");
                break;
            }
            }
            return 0;
        case WM_NOTIFY:
        {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->hwndFrom == list_ && header->code == LVN_GETINFOTIPW)
            {
                auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
                if (tip->iItem >= 0 && tip->iItem < ScenarioCount)
                    wcsncpy_s(tip->pszText, static_cast<size_t>(tip->cchTextMax), kScenarios[tip->iItem].description, _TRUNCATE);
                return 0;
            }
            if (header->hwndFrom == list_ && header->code == NM_CUSTOMDRAW)
            {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
                if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM))
                {
                    draw->clrTextBk = draw->nmcd.dwItemSpec % 2 == 0 ? RGB(255, 255, 255) : RGB(248, 250, 252);
                    if (draw->iSubItem == StateColumn)
                    {
                        wchar_t state[32]{};
                        ListView_GetItemText(list_, static_cast<int>(draw->nmcd.dwItemSpec), StateColumn, state, static_cast<int>(std::size(state)));
                        draw->clrText = wcscmp(state, L"PASS") == 0 ? Accent :
                            wcscmp(state, L"FAIL") == 0 || wcscmp(state, L"TIMEOUT") == 0 ? RGB(183, 45, 48) : Muted;
                    }
                    return CDRF_NEWFONT;
                }
            }
            break;
        }
        case UpdateMessage:
            ApplyEvent(std::unique_ptr<UiEvent>(reinterpret_cast<UiEvent*>(lParam)));
            return 0;
        case WM_TIMER:
            if (wParam == 1) { KillTimer(window_, 1); Cancel(); }
            if (wParam == 2)
            {
                KillTimer(window_, 2);
                if (!options_.snapshot.empty())
                {
                    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                    if (!SaveSnapshot(window_, options_.snapshot)) exitCode_ = 1;
                }
                DestroyWindow(window_);
            }
            return 0;
        case WM_CLOSE:
            if (running_) { closing_ = true; Cancel(); }
            else DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(exitCode_);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    Options options_;
    Handle cancel_;
    HWND window_ = nullptr, selected_ = nullptr, all_ = nullptr, stop_ = nullptr, open_ = nullptr;
    HWND repeat_ = nullptr, list_ = nullptr, progress_ = nullptr, status_ = nullptr, log_ = nullptr, recent_ = nullptr;
    HWND scope_ = nullptr, groupSummary_ = nullptr;
    HFONT normalFont_ = nullptr, titleFont_ = nullptr, labelFont_ = nullptr, logFont_ = nullptr;
    HBRUSH background_ = nullptr;
    int dpi_ = 96;
    int logTop_ = 0;
    int selectedCount_ = 0, completedCount_ = 0;
    std::array<GroupTotals, 2> totals_{};
    std::vector<int> activeCases_;
    int exitCode_ = 0;
    bool running_ = false, closing_ = false;
    fs::path recentDirectory_;
    std::thread worker_;
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput graphicsInput;
    ULONG_PTR graphics = 0;
    if (Gdiplus::GdiplusStartup(&graphics, &graphicsInput, nullptr) != Gdiplus::Ok) return 1;
    Options options;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool valid = arguments != nullptr;
    for (int index = 1; valid && index < count; ++index)
    {
        const std::wstring argument = arguments[index];
        if (argument == L"--smoke") options.smoke = true;
        else if (argument == L"--suite" && index + 1 < count)
        {
            const std::wstring suite = arguments[++index];
            if (suite == L"all") options.scope = TestScope::All;
            else if (suite == L"common") options.scope = TestScope::Common;
            else if (suite == L"game") options.scope = TestScope::Game;
            else valid = false;
        }
        else if (argument == L"--snapshot" && index + 1 < count) options.snapshot = arguments[++index];
        else if ((argument == L"--cancel-after-ms" || argument == L"--timeout-ms") && index + 1 < count)
        {
            wchar_t* end = nullptr;
            const unsigned long value = wcstoul(arguments[++index], &end, 10);
            valid = end && *end == L'\0' && value > 0 && value <= 3600000;
            if (argument == L"--cancel-after-ms") options.cancelAfterMs = value;
            else options.timeoutMs = value;
        }
        else valid = false;
    }
    if (arguments) LocalFree(arguments);
    if (!options.smoke && (options.cancelAfterMs || options.timeoutMs || !options.snapshot.empty())) valid = false;
    if (!valid) { Gdiplus::GdiplusShutdown(graphics); return 64; }
    const bool smoke = options.smoke;
    int exitCode = 1;
    {
        Workbench workbench(std::move(options));
        WNDCLASSEXW windowClass{ sizeof(windowClass) };
        windowClass.lpfnWndProc = Workbench::WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.lpszClassName = L"IOCPLifetimeVerificationWorkbench";
        RegisterClassExW(&windowClass);
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const int dpi = static_cast<int>(GetDpiForSystem());
        const int width = std::min(MulDiv(1220, dpi, 96), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) - 24);
        const int height = std::min(MulDiv(900, dpi, 96), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) - 24);
        HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"게임 서버 검증 도구 · GameServer / DBServer",
            WS_OVERLAPPEDWINDOW, monitor.rcWork.left + 12, monitor.rcWork.top + 12, width, height, nullptr, nullptr, instance, &workbench);
        if (window)
        {
            ShowWindow(window, smoke ? SW_SHOWNOACTIVATE : show);
            UpdateWindow(window);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                if (!IsDialogMessageW(window, &message))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            exitCode = workbench.ExitCode();
        }
    }
    Gdiplus::GdiplusShutdown(graphics);
    return exitCode;
}
