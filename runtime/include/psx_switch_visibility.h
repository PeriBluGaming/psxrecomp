/* psx_switch_visibility.h — force-included into every C/C++ translation unit
 * of the Switch build (cmake/psx_platform_switch.cmake,
 * PSX_SWITCH_HIDDEN_VISIBILITY).
 *
 * The NRO is one statically linked position-independent image, so no symbol
 * can be preempted. With -fPIE, GCC still reaches every extern global through
 * the GOT (an extra dependent load per access) because it must assume the
 * definition might live in a shared object. Declaring everything hidden lets
 * it use direct PC-relative addressing instead; the per-instruction cycle
 * bookkeeping reads several globals per emulated MIPS instruction. Linkage
 * and behaviour are unchanged: hidden only affects the dynamic symbol table,
 * which a static NRO does not use. libstdc++ marks namespace std with explicit
 * default visibility, which this pragma does not override. */
#pragma GCC visibility push(hidden)
