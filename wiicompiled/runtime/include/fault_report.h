#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

// Fault recovery may interrupt the allocator, streams or the window system.
// Keep diagnostics on the stack and bypass those subsystems. Fatal reports
// terminate directly without invoking other handlers or flushing streams.
namespace FaultReport {
class Message {
public:
    Message& Text(std::string_view text) noexcept {
        for (char c : text) {
            if (size_ == sizeof(bytes_) - 1) break;
            bytes_[size_++] = c;
        }
        return *this;
    }
    Message& Hex(uint64_t value, unsigned digits = 8) noexcept {
        Text("0x");
        for (unsigned i = digits; i != 0; --i) {
            const char c = "0123456789ABCDEF"[(value >> ((i - 1) * 4)) & 15];
            Text(std::string_view(&c, 1));
        }
        return *this;
    }
    Message& Decimal(uint64_t value) noexcept {
        char digits[20];
        size_t begin = sizeof(digits);
        do {
            digits[--begin] = '0' + value % 10;
            value /= 10;
        } while (value);
        return Text(std::string_view(digits + begin, sizeof(digits) - begin));
    }
    void Write() noexcept {
        bytes_[size_] = '\n';
#if defined(_WIN32)
        const DWORD savedError = GetLastError();
        DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), bytes_, static_cast<DWORD>(size_ + 1), &written, nullptr);
        SetLastError(savedError);
#else
        const int savedErrno = errno;
        const auto ignored = write(STDERR_FILENO, bytes_, size_ + 1);
        (void)ignored;
        errno = savedErrno;
#endif
    }
    [[noreturn]] void Fatal() noexcept {
        Write();
#if defined(_WIN32)
        TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
        std::_Exit(EXIT_FAILURE);
#else
        // The runtime's abort handler also exits with failure. Preserve that
        // outcome without calling handlers, flushing streams or creating a core.
        std::_Exit(EXIT_FAILURE);
#endif
    }
private:
    // At most POSIX's minimum PIPE_BUF: a pipe-backed transcript gets one record
    // per write, without interleaving concurrent fault reports.
    char bytes_[512];
    size_t size_ = 0;
};
} // namespace FaultReport
