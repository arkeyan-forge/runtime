#include "platform/platform.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

namespace
{
    void (*g_onFault)(const char* description, const void* address);

    // The thread printing a fault report. A fault on another thread meanwhile waits for the report to end the
    // process; a fault inside the report itself ends it at once.
    std::atomic<bool> g_reporting;
    std::atomic<pthread_t> g_reporter;

    // Faults are reported on this stack when the thread has one (threads started by PlatformStartThread and the one
    // that installed the handler), so that a stack overflow can be reported too.
    constexpr size_t AltStackSize = 64 * 1024;

    void InstallAltStack()
    {
        stack_t stack{};
        stack.ss_sp = malloc(AltStackSize);
        stack.ss_size = AltStackSize;
        if (stack.ss_sp != nullptr && sigaltstack(&stack, nullptr) != 0)
            free(stack.ss_sp);
    }

    void RemoveAltStack()
    {
        stack_t stack{}, previous{};
        stack.ss_flags = SS_DISABLE;
        if (sigaltstack(&stack, &previous) == 0 && !(previous.ss_flags & SS_DISABLE))
            free(previous.ss_sp);
    }

    void WriteError(const char* text)
    {
        ssize_t written = write(STDERR_FILENO, text, strlen(text));
        (void)written;
    }

    struct Registers
    {
        uintptr_t pc = 0, lr = 0, fp = 0, sp = 0;
    };

    Registers InterruptedRegisters(const void* context)
    {
        const auto* ucontext = static_cast<const ucontext_t*>(context);
        Registers r;
#if defined(__APPLE__) && defined(__aarch64__)
        r.pc = ucontext->uc_mcontext->__ss.__pc;
        r.lr = ucontext->uc_mcontext->__ss.__lr;
        r.fp = ucontext->uc_mcontext->__ss.__fp;
        r.sp = ucontext->uc_mcontext->__ss.__sp;
#elif defined(__APPLE__) && defined(__x86_64__)
        r.pc = ucontext->uc_mcontext->__ss.__rip;
        r.fp = ucontext->uc_mcontext->__ss.__rbp;
        r.sp = ucontext->uc_mcontext->__ss.__rsp;
#elif defined(__linux__) && defined(__aarch64__)
        r.pc = ucontext->uc_mcontext.pc;
        r.lr = ucontext->uc_mcontext.regs[30];
        r.fp = ucontext->uc_mcontext.regs[29];
        r.sp = ucontext->uc_mcontext.sp;
#elif defined(__linux__) && defined(__x86_64__)
        r.pc = ucontext->uc_mcontext.gregs[REG_RIP];
        r.fp = ucontext->uc_mcontext.gregs[REG_RBP];
        r.sp = ucontext->uc_mcontext.gregs[REG_RSP];
#endif
        return r;
    }

    // The calling thread's stack, [low, high).
    bool ThreadStack(uintptr_t& low, uintptr_t& high)
    {
#if defined(__APPLE__)
        high = uintptr_t(pthread_get_stackaddr_np(pthread_self()));
        low = high - pthread_get_stacksize_np(pthread_self());
        return true;
#else
        pthread_attr_t attributes;
        void* address = nullptr;
        size_t size = 0;
        if (pthread_getattr_np(pthread_self(), &attributes) != 0)
            return false;
        pthread_attr_getstack(&attributes, &address, &size);
        pthread_attr_destroy(&attributes);
        low = uintptr_t(address);
        high = low + size;
        return true;
#endif
    }

    // The recompiled guest functions on the stack (sub_XXXXXXXX), innermost first: the crash report names the guest function
    // that was running from them.
    constexpr int MaxGuestFrames = 4;
    int g_guestFrames;
    char g_guestNames[MaxGuestFrames][32];

    void PrintCodeAddress(const char* label, uintptr_t address)
    {
        Dl_info info{};
        bool found = address != 0 && dladdr(reinterpret_cast<void*>(address), &info) != 0;
        // The symbol is sub_XXXXXXXX, __imp__sub_XXXXXXXX or the C++ mangling of one: the guest address is the 8 hex digits.
        if (const char* at = found && info.dli_sname != nullptr ? strstr(info.dli_sname, "sub_") : nullptr;
            at != nullptr && g_guestFrames < MaxGuestFrames && strspn(at + 4, "0123456789ABCDEF") >= 8)
        {
            char name[13];
            snprintf(name, sizeof(name), "%.12s", at);
            if (g_guestFrames == 0 || strcmp(g_guestNames[g_guestFrames - 1], name) != 0)
                snprintf(g_guestNames[g_guestFrames++], sizeof(g_guestNames[0]), "%s", name);
        }
        if (found && info.dli_sname != nullptr)
            fprintf(stderr, "%s 0x%llx %s + %llu (%s)\n", label, (unsigned long long)address, info.dli_sname,
                (unsigned long long)(address - uintptr_t(info.dli_saddr)), info.dli_fname);
        else if (found)
            fprintf(stderr, "%s 0x%llx (%s + 0x%llx)\n", label, (unsigned long long)address, info.dli_fname,
                (unsigned long long)(address - uintptr_t(info.dli_fbase)));
        else
            fprintf(stderr, "%s 0x%llx\n", label, (unsigned long long)address);
    }

    // The faulting instruction, the link register (when a wild function pointer is called, the frame chain cannot
    // name the caller but the link register still does), then the interrupted code's frame-pointer chain.
    void PrintFaultingCode(const void* context)
    {
        char name[64] = "";
        pthread_getname_np(pthread_self(), name, sizeof(name));
        Registers r = InterruptedRegisters(context);
        fprintf(
            stderr, "thread \"%s\", sp 0x%llx, fp 0x%llx\n", name, (unsigned long long)r.sp, (unsigned long long)r.fp);
        PrintCodeAddress("pc", r.pc);
        if (r.lr != 0)
            PrintCodeAddress("lr", r.lr);

        uintptr_t low = 0, high = 0;
        bool stackKnown = ThreadStack(low, high);
        int depth = 0;
        uintptr_t fp = r.fp;
        while (stackKnown && depth < 64)
        {
            // A frame record is { previous fp, return address }, deeper frames sit at higher addresses.
            if (fp < low || fp + 2 * sizeof(uintptr_t) > high || fp % sizeof(uintptr_t) != 0)
                break;
            const uintptr_t* record = reinterpret_cast<const uintptr_t*>(fp);
            if (record[1] == 0)
                break;
            char label[8];
            snprintf(label, sizeof(label), "#%d", depth++);
            PrintCodeAddress(label, record[1]);
            if (record[0] <= fp)
                break;
            fp = record[0];
        }
        if (depth == 0)
        {
            // No frame record to walk (code built without frame pointers): ask the unwinder, from this handler.
            void* frames[64];
            backtrace_symbols_fd(frames, backtrace(frames, 64), STDERR_FILENO);
        }

        // Which guest function it was: the innermost recompiled one on the host stack, then its callers.
        if (g_guestFrames > 0)
        {
            fprintf(stderr, "[crash] guest function: %s", g_guestNames[0]);
            for (int i = 1; i < g_guestFrames; i++)
                fprintf(stderr, "%s%s", i == 1 ? ", called from " : ", ", g_guestNames[i]);
            fprintf(stderr, "\n");
        }
        else
            fprintf(stderr, "[crash] guest function: none named on the host stack (the fault is in host code)\n");

        // Raw addresses can be symbolised later: atos -o <executable> -l <base> <address>...
        Dl_info self{};
        if (dladdr(reinterpret_cast<void*>(&PrintFaultingCode), &self) != 0)
            fprintf(stderr, "executable %s loaded at %p\n", self.dli_fname, self.dli_fbase);
    }

    void OnSignal(int signal, siginfo_t* info, void* context)
    {
        bool expected = false;
        if (!g_reporting.compare_exchange_strong(expected, true))
        {
            if (pthread_equal(g_reporter.load(), pthread_self()))
            {
                WriteError("\nFault while printing the fault report\n");
                _exit(1);
            }
            while (true)
                pause(); // the first report ends the process
        }
        g_reporter = pthread_self();

        char description[32];
        snprintf(description, sizeof(description), "signal %d%s", signal,
            signal == SIGABRT   ? " abort"
            : signal == SIGSEGV ? " segv"
            : signal == SIGBUS  ? " bus"
            : signal == SIGILL  ? " illegal"
            : signal == SIGTRAP ? " trap"
                                : "");
        g_onFault(description, info->si_addr);
        PrintFaultingCode(context);
        _exit(1);
    }

    void* ThreadMain(void* argument)
    {
        std::unique_ptr<std::function<void()>> body(static_cast<std::function<void()>*>(argument));
        InstallAltStack();
        (*body)();
        RemoveAltStack();
        return nullptr;
    }
}

void PlatformInit() {}

void PlatformInstallFaultHandler(void (*onFault)(const char* description, const void* address))
{
    g_onFault = onFault;
    InstallAltStack();

    struct sigaction action{};
    action.sa_sigaction = OnSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGTRAP, &action, nullptr); // libc++ hardening assertions and __builtin_trap
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGABRT, &action,
        nullptr); // the runtime's own fatal errors (abort(): an unrecompiled indirect call, a full heap)
}

uint8_t* PlatformReserveMemory(size_t size)
{
    // Anonymous pages are committed lazily, so reserving a large range is cheap.
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return memory != MAP_FAILED ? static_cast<uint8_t*>(memory) : nullptr;
}

bool PlatformProtectNoAccess(void* address, size_t size) { return mprotect(address, size, PROT_NONE) == 0; }

void PlatformStartThread(size_t stackSize, std::function<void()> body)
{
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, stackSize);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);

    pthread_t handle;
    pthread_create(&handle, &attributes, ThreadMain, new std::function<void()>(std::move(body)));
    pthread_attr_destroy(&attributes);
}

void PlatformSetThreadName(const char* name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#else
    // Linux rejects names longer than 15 bytes instead of truncating them.
    char truncated[16];
    snprintf(truncated, sizeof(truncated), "%s", name);
    pthread_setname_np(pthread_self(), truncated);
#endif
}

void PlatformSetThreadTimingCritical(uint32_t periodUs)
{
#if defined(__APPLE__)
    // Up to an eighth of the period of work, done within a quarter of it; a thread that works longer is demoted by the
    // kernel to the normal policy, so a slow interrupt callback costs nothing worse than before.
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    auto ticks = [&](uint64_t us) { return uint32_t(us * 1000 * timebase.denom / timebase.numer); };
    thread_time_constraint_policy_data_t policy{ ticks(periodUs), ticks(periodUs / 8), ticks(periodUs / 4), 1 };
    kern_return_t result = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (result != KERN_SUCCESS)
        fprintf(stderr, "[platform] real-time thread policy refused (%d)\n", result);
#else
    (void)periodUs;
#endif
}

PlatformFile PlatformOpenFile(const char* path) { return open(path, O_RDONLY); }

int64_t PlatformReadFile(PlatformFile file, void* buffer, uint32_t size, uint64_t offset)
{
    return pread(int(file), buffer, size, off_t(offset));
}

void PlatformCloseFile(PlatformFile file) { close(int(file)); }
