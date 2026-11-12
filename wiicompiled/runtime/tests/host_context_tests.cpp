#include "host_context.h"

#include <cstdlib>
#include <limits>

namespace {
// The worker yields twice; each return to the scheduler must preserve both
// context identities and the worker's continuation point.
HostContext::Handle g_scheduler = nullptr;
HostContext::Handle g_worker = nullptr;
int g_steps = 0;

void Worker(void*)
{
    if (!HostContext::IsCurrent(g_worker)) {
        std::abort();
    }
    ++g_steps;
    HostContext::Switch(g_scheduler);
    if (!HostContext::IsCurrent(g_worker)) {
        std::abort();
    }
    ++g_steps;
    HostContext::Switch(g_scheduler);
}
} // namespace

int main()
{
    if (!HostContext::InitializeScheduler(&g_scheduler) ||
        !HostContext::IsCurrent(g_scheduler)) {
        return 1;
    }
#if !defined(_WIN32) && defined(__x86_64__)
    // libco takes unsigned int: a larger request must not wrap into a small stack.
    const std::size_t oversized = static_cast<std::size_t>(std::numeric_limits<unsigned int>::max()) + 1 + 64 * 1024;
    if (auto context = HostContext::Create(oversized, Worker, nullptr)) {
        HostContext::Destroy(context);
        return 1;
    }
#endif
    g_worker = HostContext::Create(64 * 1024, Worker, nullptr);
    if (!g_worker) {
        return 1;
    }

    HostContext::Switch(g_worker);
    if (g_steps != 1 || !HostContext::IsCurrent(g_scheduler)) {
        return 1;
    }
    HostContext::Switch(g_worker);
    if (g_steps != 2 || !HostContext::IsCurrent(g_scheduler)) {
        return 1;
    }

    HostContext::Destroy(g_worker);
    HostContext::ShutdownScheduler(g_scheduler);
}
