/*
 * BIOS interrupt 19h handler
 */

#include <stdlib.h>
#include "miscemu.h"
#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(int19)


/**********************************************************************
 *          INT_Int19Handler
 *
 * Handler for int 19h (Reboot).
 */
void WINAPI INT_Int19Handler( CONTEXT86 *context )
{
    WARN("Attempted Reboot\n");
}
