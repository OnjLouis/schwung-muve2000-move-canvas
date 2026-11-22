#include "boxedwine.h"
#include <csignal>
#include <cstdlib>
#include <sys/prctl.h>
#include <unistd.h>

// Retire the compatibility worker if Move exits while the guest has the FIFOs open.
__attribute__((constructor)) static void followMoveLifetime() {
    const char *parent = std::getenv("SCBRIDGE_MOVE_PARENT_PID");
    if (parent && *parent &&
        (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != std::atoi(parent)))
        _exit(1);
}

// The Win32 guest's optional Mac audio workgroup call is not used on Linux.
void callOpenGL(CPU* cpu, U32 operation) {
    if (operation == 0x53434155) cpu->reg[0].u32 = 0;
}

