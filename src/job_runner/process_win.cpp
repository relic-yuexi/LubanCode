#include "job_runner/process.hpp"
#include "job_runner/common.hpp"
#include "platform/paths.hpp"
#include <algorithm>
#include <array>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>

namespace lubancode::job_runner {
std::string RandomHex() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw Error("runner.entropy_unavailable");
    }
    std::string out;
    for (auto c : bytes) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
    return out;
}
void VerifyPrivateDirectory(const fs::path& path) {
    const auto native = platform::FileIoPath(path);
    const DWORD attrs = GetFileAttributesW(native.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw Error("runner.state_not_private");
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw Error("runner.state_not_private");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<char> user(size);
    const bool got_user = GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE;
    CloseHandle(token);
    if (!got_user) throw Error("runner.state_not_private");
    PACL dacl = nullptr;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(native.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                             &owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) throw Error("runner.state_not_private");
    bool allowed = dacl != nullptr && owner != nullptr && EqualSid(owner, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
    for (DWORD i = 0; allowed && i < dacl->AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) { allowed = false; break; }
        auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { allowed = false; break; }
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        auto* sid = reinterpret_cast<PSID>(&ace->SidStart);
        allowed = EqualSid(sid, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) ||
                  IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid);
    }
    LocalFree(descriptor);
    if (!allowed) throw Error("runner.state_not_private");
}
std::string ProcessScope() { return "windows_job_object"; }
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
};
std::wstring Quote(const std::wstring& arg) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}
}
struct Process::Impl {
    HANDLE job = nullptr, process = nullptr, thread = nullptr;
    DWORD process_id = 0;
    bool closing = false;
    std::optional<std::int64_t> exit;
    ~Impl() {
        if (thread) CloseHandle(thread);
        if (process) CloseHandle(process);
        if (job) CloseHandle(job);
    }
};
Process::Process(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
std::unique_ptr<Process> Process::Create(const Json& spec, const fs::path& dir,
                                        const std::vector<std::string>& environment) {
    auto impl = std::make_unique<Impl>();
    impl->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!impl->job || !SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        throw Error("runner.job_object_unavailable");
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle output{CreateFileW(platform::FileIoPath(dir / "stdout.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Handle error{CreateFileW(platform::FileIoPath(dir / "stderr.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (output.value == INVALID_HANDLE_VALUE || error.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE) {
        throw Error("runner.launch_failed");
    }
    std::wstring command;
    for (const auto& arg : spec["argv"]) {
        if (!command.empty()) command += L' ';
        command += Quote(platform::Utf8ToWide(arg.get<std::string>()));
    }
    std::vector<std::wstring> entries;
    for (const auto& entry : environment) entries.push_back(platform::Utf8ToWide(entry));
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::wstring env;
    for (const auto& entry : entries) { env += entry; env.push_back(L'\0'); }
    env.push_back(L'\0');
    if (entries.empty()) env.push_back(L'\0');
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attribute_storage(attribute_bytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &attribute_bytes)) throw Error("runner.launch_failed");
    HANDLE inherited[] = {input.value, output.value, error.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(list);
        throw Error("runner.launch_failed");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = output.value;
    startup.StartupInfo.hStdError = error.value;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    const auto exe = platform::Utf8ToPath(spec["argv"][0].get<std::string>());
    const auto cwd = platform::Utf8ToPath(spec["cwd"].get<std::string>());
    const bool created = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        env.data(), cwd.c_str(), &startup.StartupInfo, &process) != FALSE;
    DeleteProcThreadAttributeList(list);
    if (!created) throw Error("runner.launch_failed");
    impl->process = process.hProcess;
    impl->thread = process.hThread;
    impl->process_id = process.dwProcessId;
    if (!AssignProcessToJobObject(impl->job, impl->process)) {
        TerminateProcess(impl->process, 126);
        WaitForSingleObject(impl->process, INFINITE);
        throw Error("runner.job_object_unavailable");
    }
    return std::unique_ptr<Process>(new Process(std::move(impl)));
}
Process::~Process() {
    if (impl_ && impl_->process && !impl_->exit) {
        TerminateJobObject(impl_->job, 137);
        WaitForSingleObject(impl_->process, 5000);
    }
}
std::uint64_t Process::pid() const { return impl_->process_id; }
void Process::Release() {
    if (ResumeThread(impl_->thread) == static_cast<DWORD>(-1)) throw Error("runner.launch_failed");
    CloseHandle(impl_->thread);
    impl_->thread = nullptr;
}
void Process::Cancel() {
    if (!impl_->exit && !TerminateJobObject(impl_->job, 137)) throw Error("runner.cancel_failed");
}
std::optional<std::int64_t> Process::Poll() {
    if (impl_->exit) return impl_->exit;
    const DWORD wait = WaitForSingleObject(impl_->process, 0);
    if (wait == WAIT_TIMEOUT) return std::nullopt;
    if (wait != WAIT_OBJECT_0) throw Error("runner.process_identity_lost");
    DWORD code = 0;
    if (!GetExitCodeProcess(impl_->process, &code)) throw Error("runner.process_identity_lost");
    if (!impl_->closing) {
        if (!TerminateJobObject(impl_->job, 137)) throw Error("runner.cancel_failed");
        impl_->closing = true;
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(impl_->job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
        throw Error("runner.process_identity_lost");
    }
    if (accounting.ActiveProcesses != 0) return std::nullopt;
    impl_->exit = code;
    return impl_->exit;
}
}  // namespace lubancode::job_runner
