/* Global core lock.
 *
 * Verbs and object-table mutations are serialized across CPUs. Blocking verbs
 * release the lock around the context switch and reacquire it on wake.
 */
#include "finte/object.h"

static volatile int g_core_lock;

void finte_core_lock(void)
{
    fcpu_t *cpu = hal_cpu_self();
    if (cpu->core_depth++ == 0) {
        cpu->lock_flags = hal_irq_save();
        hal_spin_lock(&g_core_lock);
    }
}

void finte_core_unlock(void)
{
    fcpu_t *cpu = hal_cpu_self();
    if (cpu->core_depth == 0) {
        return;
    }
    if (--cpu->core_depth == 0) {
        hal_spin_unlock(&g_core_lock);
        hal_irq_restore(cpu->lock_flags);
    }
}

uint32_t finte_core_lock_suspend(void)
{
    fcpu_t *cpu = hal_cpu_self();
    uint32_t depth = cpu->core_depth;
    if (depth) {
        cpu->core_depth = 0;
        hal_spin_unlock(&g_core_lock);
        hal_irq_restore(cpu->lock_flags);
    }
    return depth;
}

void finte_core_lock_resume(uint32_t depth)
{
    if (!depth) {
        return;
    }
    fcpu_t *cpu = hal_cpu_self();
    cpu->lock_flags = hal_irq_save();
    hal_spin_lock(&g_core_lock);
    cpu->core_depth = depth;
}
