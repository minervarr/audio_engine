#pragma once
// pread() is POSIX-only — MinGW/Windows' CRT has no direct equivalent, and
// this was never given a Windows branch (unistd.h included unconditionally
// in every caller). One shared, header-only primitive for the handful of
// backends/{dsd,flac,mp3}/*.cpp callers instead of scattering #ifdef _WIN32
// through each of them individually.
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
using ssize_t = long long;
#else
#include <unistd.h>
#endif

// Reads up to `count` bytes from `fd` at absolute file offset `offset`
// WITHOUT moving the file's current read position — matches POSIX pread()'s
// contract exactly, including safety when called concurrently on the same
// fd from multiple threads (a seek()+read() pair would race on the shared
// file position; this doesn't touch it). Returns bytes read (0 at EOF), or
// -1 on error.
inline ssize_t osPread(int fd, void* buf, size_t count, int64_t offset) {
#ifdef _WIN32
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, static_cast<DWORD>(count), &got, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        return -1;
    }
    return static_cast<ssize_t>(got);
#else
    return ::pread(fd, buf, count, offset);
#endif
}
