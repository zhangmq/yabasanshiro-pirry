/*  YabaSanshiro — libretro port: the platform layer the engine expects.

    This file is part of YabaSanshiro.

    YabaSanshiro is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    YabaSanshiro is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with YabaSanshiro; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*
 * The engine calls a handful of platform hooks (declared in yui.h).  Under
 * retro_arena they live in main.cpp; here they are the libretro equivalents:
 *
 *   YuiSwapBuffers()            present -> video_cb(RETRO_HW_FRAME_BUFFER_VALID)
 *   YuiUseOGLOnThisThread()     no-op: the front end owns the only context and
 *   YuiRevokeOGLOnThisThread()  only the retro_run thread may touch it
 *   YuiErrorMsg()               -> front end log (fatal engine errors)
 *
 * Threading contract (see .notes/CURRENT/DESIGN-THREADMODEL.md): the renderer
 * runs inside retro_run on the front end's thread, in the front end's context.
 * The machine thread never touches GL.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libretro.h"
#include "log.h"
#include "yui.h"
#include "m68kcore.h"
#include "sh2core.h"
#include "sh2int.h"
#include "peripheral.h"
#include "cdbase.h"
#include "scsp.h"
#include "vidshared.h"
#include "vidogl.h"
#include "osdcore.h"
#include "nanovg/nanovg_osdcore.h"

extern SH2Interface_struct SH2Dyn, SH2DynDebug;

/* The 68k core list is per port (retro_arena/main.cpp, gtk/main.c, ...); it is
   what M68KCoreList in m68kcore.c walks to pick an interpreter. */
M68K_struct *M68KCoreList[] = {
   &M68KDummy,
#ifdef HAVE_MUSASHI
   &M68KMusashi,
#endif
   NULL
};

/* The rest of the port-owned core tables.  Input/audio are deliberately the
   dummy cores for now: the emulation+render path is what is being brought up. */
SH2Interface_struct *SH2CoreList[] = {
   &SH2Interpreter,
   &SH2DebugInterpreter,
#ifdef DYNAREC_DEVMIYAX
   &SH2Dyn,
   &SH2DynDebug,
#endif
   NULL
};

PerInterface_struct *PERCoreList[] = { &PERDummy, NULL };

CDInterface *CDCoreList[] = { &DummyCD, &ISOCD, NULL };

SoundInterface_struct *SNDCoreList[] = { &SNDDummy, NULL };

VideoInterface_struct *VIDCoreList[] = { &VIDDummy, &VIDOGL, NULL };

#ifdef YAB_PORT_OSD
OSD_struct *OSDCoreList[] = { &OSDNnovg, &OSDDummy, NULL };
#endif

/* Implemented by libretro.c: the frame that the renderer just finished. */
void LibretroPresentFrame(void);
int  LibretroHasVideo(void);

void YuiErrorMsg(const char *string)
{
   LOG_WARN("engine: %s", string ? string : "(null)");
}

void YuiSwapBuffers(void)
{
   /* Present point: the engine has finished the field in the front end's FBO. */
   LibretroPresentFrame();
}

int YuiUseOGLOnThisThread(void)
{
   /* The front end's context is current on the thread that called retro_run and
      nowhere else; there is nothing to bind here. */
   return 0;
}

int YuiRevokeOGLOnThisThread(void)
{
   return 0;
}
