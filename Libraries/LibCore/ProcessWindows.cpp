/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023-2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <AK/Windows.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace Core {

static constexpr int windows_stdin_fd = 0;
static constexpr int windows_stdout_fd = 1;
static constexpr int windows_stderr_fd = 2;

static ByteString quote_windows_argument(ByteString const& argument)
{
    StringBuilder builder;
    builder.append('"');

    size_t backslash_count = 0;
    for (auto ch : argument) {
        if (ch == '\\') {
            ++backslash_count;
            continue;
        }

        if (ch == '"') {
            for (size_t i = 0; i < backslash_count * 2 + 1; ++i)
                builder.append('\\');
            builder.append('"');
            backslash_count = 0;
            continue;
        }

        for (size_t i = 0; i < backslash_count; ++i)
            builder.append('\\');
        backslash_count = 0;
        builder.append(ch);
    }

    for (size_t i = 0; i < backslash_count * 2; ++i)
        builder.append('\\');
    builder.append('"');

    return builder.to_byte_string();
}

Process::Process(Process&& other)
    : m_handle(exchange(other.m_handle, nullptr))
{
}

Process& Process::operator=(Process&& other)
{
    m_handle = exchange(other.m_handle, nullptr);
    return *this;
}

Process::~Process()
{
    if (m_handle)
        CloseHandle(m_handle);
}

Process Process::current()
{
    return GetCurrentProcess();
}

ErrorOr<Process> Process::spawn(ProcessSpawnOptions const& options)
{
    StringBuilder builder;
    builder.append(quote_windows_argument(options.executable));

    for (auto const& arg : options.arguments) {
        builder.append(' ');
        builder.append(quote_windows_argument(arg));
    }

    builder.append('\0');
    ByteBuffer command_line = TRY(builder.to_byte_buffer());

    STARTUPINFOEXA startup_info = {};
    startup_info.StartupInfo.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info = {};

    Vector<HANDLE> inherited_handles;
    auto cleanup_inherited_handles = ScopeGuard([&] {
        for (auto handle : inherited_handles)
            CloseHandle(handle);
    });

    auto duplicate_inheritable_handle = [&](HANDLE handle) -> ErrorOr<HANDLE> {
        HANDLE duplicated_handle = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicated_handle, 0, TRUE, DUPLICATE_SAME_ACCESS))
            return Error::from_windows_error();
        inherited_handles.append(duplicated_handle);
        return duplicated_handle;
    };

    auto set_startup_std_handle = [&](DWORD std_handle_id, int fd) -> ErrorOr<void> {
        auto duplicated_handle = TRY(duplicate_inheritable_handle(to_handle(fd)));
        startup_info.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        switch (std_handle_id) {
        case STD_INPUT_HANDLE:
            startup_info.StartupInfo.hStdInput = duplicated_handle;
            break;
        case STD_OUTPUT_HANDLE:
            startup_info.StartupInfo.hStdOutput = duplicated_handle;
            break;
        case STD_ERROR_HANDLE:
            startup_info.StartupInfo.hStdError = duplicated_handle;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
        return {};
    };

    for (auto const& action : options.file_actions) {
        auto result = action.visit(
            [&](FileAction::OpenFile const& open_file) -> ErrorOr<void> {
                VERIFY(open_file.fd == windows_stdin_fd || open_file.fd == windows_stdout_fd || open_file.fd == windows_stderr_fd);

                auto file = TRY(Core::File::open(open_file.path, open_file.mode));
                auto std_handle_id = open_file.fd == windows_stdin_fd ? STD_INPUT_HANDLE
                    : open_file.fd == windows_stdout_fd               ? STD_OUTPUT_HANDLE
                                                                      : STD_ERROR_HANDLE;
                return set_startup_std_handle(std_handle_id, file->fd());
            },
            [&](FileAction::CloseFile const& close_file) -> ErrorOr<void> {
                if (close_file.fd == windows_stdin_fd || close_file.fd == windows_stdout_fd || close_file.fd == windows_stderr_fd) {
                    startup_info.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
                    if (close_file.fd == windows_stdin_fd)
                        startup_info.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
                    else if (close_file.fd == windows_stdout_fd)
                        startup_info.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
                    else
                        startup_info.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
                }
                return {};
            },
            [&](FileAction::DupFd const& dup_fd) -> ErrorOr<void> {
                VERIFY(dup_fd.fd == windows_stdin_fd || dup_fd.fd == windows_stdout_fd || dup_fd.fd == windows_stderr_fd);

                auto std_handle_id = dup_fd.fd == windows_stdin_fd ? STD_INPUT_HANDLE
                    : dup_fd.fd == windows_stdout_fd              ? STD_OUTPUT_HANDLE
                                                                   : STD_ERROR_HANDLE;
                return set_startup_std_handle(std_handle_id, dup_fd.write_fd);
            });
        TRY(result);
    }

    if (auto socket_takeover = getenv("SOCKET_TAKEOVER")) {
        auto separator = strchr(socket_takeover, ':');
        if (separator) {
            auto inherited_fd = atoi(separator + 1);
            auto duplicated_handle = TRY(duplicate_inheritable_handle(to_handle(inherited_fd)));
            (void)duplicated_handle;
        }
    }

    SIZE_T attribute_list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_list_size);
    ByteBuffer attribute_list_buffer = TRY(ByteBuffer::create_uninitialized(attribute_list_size));
    startup_info.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_list_buffer.data());
    if (!InitializeProcThreadAttributeList(startup_info.lpAttributeList, 1, 0, &attribute_list_size))
        return Error::from_windows_error();
    auto cleanup_attribute_list = ScopeGuard([&] {
        DeleteProcThreadAttributeList(startup_info.lpAttributeList);
    });

    if (!UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles.data(), inherited_handles.size() * sizeof(HANDLE), nullptr, nullptr))
        return Error::from_windows_error();

    BOOL result = CreateProcessA(
        nullptr,
        reinterpret_cast<char*>(command_line.data()),
        nullptr, // process security attributes
        nullptr, // primary thread security attributes
        TRUE,    // handles are inherited according to the explicit handle list above
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr, // use parent's environment
        nullptr, // working directory
        &startup_info.StartupInfo,
        &process_info);

    if (!result)
        return Error::from_windows_error();

    CloseHandle(process_info.hThread);

    return Process(process_info.hProcess);
}

ErrorOr<Process> Process::spawn(StringView path, ReadonlySpan<ByteString> arguments)
{
    return spawn({
        .executable = path,
        .arguments = Vector<ByteString> { arguments },
    });
}

ErrorOr<Process> Process::spawn(StringView path, ReadonlySpan<StringView> arguments)
{
    Vector<ByteString> backing_strings;
    backing_strings.ensure_capacity(arguments.size());
    for (auto argument : arguments)
        backing_strings.append(argument);

    return spawn({
        .executable = path,
        .arguments = backing_strings,
    });
}

// Get the full path of the executable file of the current process
ErrorOr<String> Process::get_name()
{
    Vector<wchar_t, MAX_PATH> path;
    path.resize(MAX_PATH);

    DWORD length = GetModuleFileNameW(NULL, path.data(), MAX_PATH);

    if (length == path.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        path.resize(UNICODE_STRING_MAX_CHARS);
        length = GetModuleFileNameW(NULL, path.data(), UNICODE_STRING_MAX_CHARS);
    }

    if (!length)
        return Error::from_windows_error();

    return MUST(Utf16View { reinterpret_cast<char16_t const*>(path.data()), length }.to_utf8());
}

ErrorOr<bool> Process::is_being_debugged()
{
    return IsDebuggerPresent();
}

// Forces the process to sleep until a debugger is attached, then breaks.
void Process::wait_for_debugger_and_break()
{
    bool print_message = true;
    for (;;) {
        if (IsDebuggerPresent()) {
            DebugBreak();
            return;
        }
        if (print_message) {
            dbgln("Process {} with pid {} is sleeping, waiting for debugger.", Process::get_name(), GetCurrentProcessId());
            print_message = false;
        }
        Sleep(100);
    }
}

pid_t Process::pid() const
{
    return GetProcessId(m_handle);
}

ErrorOr<int> Process::wait_for_termination() const
{
    auto result = WaitForSingleObject(m_handle, INFINITE);
    if (result == WAIT_FAILED)
        return Error::from_windows_error();

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(m_handle, &exit_code))
        return Error::from_windows_error();

    return exit_code;
}

}
