#if defined(RTEMS_LEGACY_STACK) || defined(RTEMS_LIBBSD_STACK)
/* The classic VME bridge API (BSP_installVME_isr, BSP_vme2local_adrs, ...)
 * is provided by the BSP independently of which network stack is in use.
 * Gating VME support on the legacy network stack excludes BSPs (e.g.
 * beatnik) that have full VME support but use the libbsd stack.
 */
#include "os/RTEMS-score/devLibVMEOSD.c"
#else
#pragma message "\n VME Support requires the RTEMS Legacy or libbsd network stack\n"
#include "os/default/devLibVMEOSD.c"
#endif
