// Miscellaneous XAM services.
#include "kernel/objects.h"
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    // A notification listener is a waitable handle signaled while notifications are pending. The system sends
    // none yet (sign-in changes, storage, background music...), so it never is.
    struct NotificationListener : KernelObject
    {
        uint64_t mask;

        explicit NotificationListener(uint64_t mask) : mask(mask) {}
    };
}

PPC_FUNC(__imp__XamNotifyCreateListener)
{
    ctx.r3.u64 = CreateHandle(std::make_shared<NotificationListener>(ctx.r3.u64));
}

// XNotifyGetNext(listener, match id, id out, param out): FALSE when nothing is pending.
PPC_FUNC(__imp__XNotifyGetNext) { ctx.r3.u64 = 0; }

// XNotifyPositionUI(position): where system notifications pop up. None are drawn, so the position is not kept.
PPC_FUNC(__imp__XNotifyPositionUI) {}
