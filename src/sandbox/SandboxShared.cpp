#include "sandbox/SandboxShared.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <semaphore.h>
 #include <sys/mman.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

#include <cerrno>
#include <cstring>

namespace livedaw::sandbox
{

#if JUCE_WINDOWS
namespace
{
    std::wstring objectName (const juce::String& name, const char* suffix)
    {
        return (juce::String ("Local\\") + name.trimCharactersAtStart ("/") + suffix).toWideCharPointer();
    }
} // namespace
#endif

//==============================================================================
SharedMemory::~SharedMemory()
{
    close();
}

bool SharedMemory::create (const juce::String& n, size_t numBytes)
{
    close();
    name = n;
    size = numBytes;
    owner = true;

#if JUCE_WINDOWS
    const auto hi = (DWORD) ((juce::uint64) numBytes >> 32), lo = (DWORD) (numBytes & 0xffffffffu);
    handle = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, objectName (name, "m").c_str());
    if (handle == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
        return close(), false;
    data = MapViewOfFile (handle, FILE_MAP_ALL_ACCESS, 0, 0, numBytes);
#else
    const int fd = shm_open (name.toRawUTF8(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
        return false;
    if (ftruncate (fd, (off_t) numBytes) != 0)
    {
        ::close (fd);
        shm_unlink (name.toRawUTF8());
        return false;
    }
    auto* p = mmap (nullptr, numBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close (fd);
    data = p == MAP_FAILED ? nullptr : p;
#endif

    if (data == nullptr)
        return close(), false;

    // Touch every page now, so the audio thread never takes a first-use page fault.
    std::memset (data, 0, numBytes);
    return true;
}

bool SharedMemory::open (const juce::String& n, size_t numBytes)
{
    close();
    name = n;
    size = numBytes;
    owner = false;

#if JUCE_WINDOWS
    handle = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, objectName (name, "m").c_str());
    if (handle == nullptr)
        return false;
    data = MapViewOfFile (handle, FILE_MAP_ALL_ACCESS, 0, 0, numBytes);
#else
    const int fd = shm_open (name.toRawUTF8(), O_RDWR, 0600);
    if (fd < 0)
        return false;
    struct stat st {};
    if (fstat (fd, &st) != 0 || (size_t) st.st_size < numBytes)
    {
        ::close (fd);
        return false;
    }
    auto* p = mmap (nullptr, numBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close (fd);
    data = p == MAP_FAILED ? nullptr : p;
#endif

    if (data == nullptr)
        return close(), false;
    return true;
}

void SharedMemory::close()
{
#if JUCE_WINDOWS
    if (data != nullptr)
        UnmapViewOfFile (data);
    if (handle != nullptr)
        CloseHandle (handle);
    handle = nullptr;
#else
    if (data != nullptr)
        munmap (data, size);
    if (owner && name.isNotEmpty())
        shm_unlink (name.toRawUTF8());
#endif
    data = nullptr;
    owner = false;
    name = {};
}

//==============================================================================
NamedSemaphore::~NamedSemaphore()
{
    close();
}

bool NamedSemaphore::create (const juce::String& n)
{
    close();
    name = n;
    owner = true;
#if JUCE_WINDOWS
    handle = CreateSemaphoreW (nullptr, 0, 0x7fffffff, objectName (name, "s").c_str());
    if (handle != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
        return close(), false;
#else
    auto* s = sem_open ((name + "s").toRawUTF8(), O_CREAT | O_EXCL, 0600, 0);
    handle = s == SEM_FAILED ? nullptr : s;
#endif
    if (handle == nullptr)
        owner = false;
    return handle != nullptr;
}

bool NamedSemaphore::open (const juce::String& n)
{
    close();
    name = n;
#if JUCE_WINDOWS
    handle = OpenSemaphoreW (SEMAPHORE_MODIFY_STATE | SYNCHRONIZE, FALSE, objectName (name, "s").c_str());
#else
    auto* s = sem_open ((name + "s").toRawUTF8(), 0);
    handle = s == SEM_FAILED ? nullptr : s;
#endif
    return handle != nullptr;
}

void NamedSemaphore::close()
{
#if JUCE_WINDOWS
    if (handle != nullptr)
        CloseHandle (handle);
#else
    if (handle != nullptr)
        sem_close (static_cast<sem_t*> (handle));
    if (owner && name.isNotEmpty())
        sem_unlink ((name + "s").toRawUTF8());
#endif
    handle = nullptr;
    owner = false;
    name = {};
}

void NamedSemaphore::post() noexcept
{
    if (handle == nullptr)
        return;
#if JUCE_WINDOWS
    ReleaseSemaphore (handle, 1, nullptr);
#else
    sem_post (static_cast<sem_t*> (handle));
#endif
}

void NamedSemaphore::wait() noexcept
{
    if (handle == nullptr)
        return;
#if JUCE_WINDOWS
    WaitForSingleObject (handle, INFINITE);
#else
    while (sem_wait (static_cast<sem_t*> (handle)) != 0 && errno == EINTR)
    {
    }
#endif
}

//==============================================================================
juce::String makeUniqueName()
{
    return "/sdc" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()).substring (0, 12);
}

int currentProcessId()
{
#if JUCE_WINDOWS
    return (int) GetCurrentProcessId();
#else
    return (int) getpid();
#endif
}

void allowProcessToTakeFocus ([[maybe_unused]] int pid)
{
#if JUCE_WINDOWS
    if (pid > 0)
        AllowSetForegroundWindow ((DWORD) pid);
#endif
}

} // namespace livedaw::sandbox
