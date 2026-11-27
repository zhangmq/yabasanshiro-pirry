/*  YabaSanshiro -- libretro port: runtime-resolved GL entry points.

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
 * The hard-linked GL library is gone on purpose.  A libretro core does not own
 * its GL context: the front end creates it and publishes the entry points
 * through retro_hw_render_callback.get_proc_address ("use this to retrieve
 * symbols and extensions", libretro.h).  Linking a vendor library instead pins
 * the binary to one SoC's driver, and on a device that has the EGL/GLES stubs
 * but no such library the whole core fails to load -- before a single line of
 * it runs, so the failure cannot even be reported.  Every GL call the core
 * makes therefore goes through the function pointers below.
 *
 * YglGlProcResolve() fills them once per context reset; the names are then
 * redefined to the pointers, so call sites keep reading like plain GL calls.
 *
 * Include this header only AFTER the real GL headers.  The redefinitions are
 * plain object-like macros: a GL header processed afterwards would declare
 * "p_glFoo" as a function.
 */

#ifndef YGL_GLPROC_H
#define YGL_GLPROC_H

#ifdef __LIBRETRO__

#include <GLES3/gl32.h>   /* the PFN types; ES 3.2 is what the core is built for */

#ifdef __cplusplus
extern "C" {
#endif

/* Filled by YglGlProcResolve(); NULL means the entry point was not found. */
extern PFNGLACTIVETEXTUREPROC p_glActiveTexture;
extern PFNGLATTACHSHADERPROC p_glAttachShader;
extern PFNGLBINDATTRIBLOCATIONPROC p_glBindAttribLocation;
extern PFNGLBINDBUFFERPROC p_glBindBuffer;
extern PFNGLBINDBUFFERBASEPROC p_glBindBufferBase;
extern PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
extern PFNGLBINDIMAGETEXTUREPROC p_glBindImageTexture;
extern PFNGLBINDRENDERBUFFERPROC p_glBindRenderbuffer;
extern PFNGLBINDTEXTUREPROC p_glBindTexture;
extern PFNGLBINDVERTEXARRAYPROC p_glBindVertexArray;
extern PFNGLBLENDFUNCPROC p_glBlendFunc;
extern PFNGLBLENDFUNCSEPARATEPROC p_glBlendFuncSeparate;
extern PFNGLBUFFERDATAPROC p_glBufferData;
extern PFNGLBUFFERSUBDATAPROC p_glBufferSubData;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC p_glCheckFramebufferStatus;
extern PFNGLCLEARPROC p_glClear;
extern PFNGLCLEARCOLORPROC p_glClearColor;
extern PFNGLCLEARDEPTHFPROC p_glClearDepthf;
extern PFNGLCLEARSTENCILPROC p_glClearStencil;
extern PFNGLCLIENTWAITSYNCPROC p_glClientWaitSync;
extern PFNGLCOLORMASKPROC p_glColorMask;
extern PFNGLCOMPILESHADERPROC p_glCompileShader;
extern PFNGLCOPYBUFFERSUBDATAPROC p_glCopyBufferSubData;
extern PFNGLCREATEPROGRAMPROC p_glCreateProgram;
extern PFNGLCREATESHADERPROC p_glCreateShader;
extern PFNGLCULLFACEPROC p_glCullFace;
extern PFNGLDELETEBUFFERSPROC p_glDeleteBuffers;
extern PFNGLDELETEFRAMEBUFFERSPROC p_glDeleteFramebuffers;
extern PFNGLDELETEPROGRAMPROC p_glDeleteProgram;
extern PFNGLDELETERENDERBUFFERSPROC p_glDeleteRenderbuffers;
extern PFNGLDELETESHADERPROC p_glDeleteShader;
extern PFNGLDELETESYNCPROC p_glDeleteSync;
extern PFNGLDELETETEXTURESPROC p_glDeleteTextures;
extern PFNGLDELETEVERTEXARRAYSPROC p_glDeleteVertexArrays;
extern PFNGLDEPTHFUNCPROC p_glDepthFunc;
extern PFNGLDEPTHMASKPROC p_glDepthMask;
extern PFNGLDETACHSHADERPROC p_glDetachShader;
extern PFNGLDISABLEPROC p_glDisable;
extern PFNGLDISABLEVERTEXATTRIBARRAYPROC p_glDisableVertexAttribArray;
extern PFNGLDISPATCHCOMPUTEPROC p_glDispatchCompute;
extern PFNGLDRAWARRAYSPROC p_glDrawArrays;
extern PFNGLENABLEPROC p_glEnable;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC p_glEnableVertexAttribArray;
extern PFNGLFENCESYNCPROC p_glFenceSync;
extern PFNGLFINISHPROC p_glFinish;
extern PFNGLFLUSHPROC p_glFlush;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC p_glFramebufferRenderbuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC p_glFramebufferTexture2D;
extern PFNGLFRONTFACEPROC p_glFrontFace;
extern PFNGLGENBUFFERSPROC p_glGenBuffers;
extern PFNGLGENERATEMIPMAPPROC p_glGenerateMipmap;
extern PFNGLGENFRAMEBUFFERSPROC p_glGenFramebuffers;
extern PFNGLGENRENDERBUFFERSPROC p_glGenRenderbuffers;
extern PFNGLGENTEXTURESPROC p_glGenTextures;
extern PFNGLGENVERTEXARRAYSPROC p_glGenVertexArrays;
extern PFNGLGETATTRIBLOCATIONPROC p_glGetAttribLocation;
extern PFNGLGETERRORPROC p_glGetError;
extern PFNGLGETINTEGERVPROC p_glGetIntegerv;
extern PFNGLGETPROGRAMINFOLOGPROC p_glGetProgramInfoLog;
extern PFNGLGETPROGRAMIVPROC p_glGetProgramiv;
extern PFNGLGETSHADERINFOLOGPROC p_glGetShaderInfoLog;
extern PFNGLGETSHADERIVPROC p_glGetShaderiv;
extern PFNGLGETSTRINGPROC p_glGetString;
extern PFNGLGETUNIFORMBLOCKINDEXPROC p_glGetUniformBlockIndex;
extern PFNGLGETUNIFORMLOCATIONPROC p_glGetUniformLocation;
extern PFNGLLINKPROGRAMPROC p_glLinkProgram;
extern PFNGLMAPBUFFERRANGEPROC p_glMapBufferRange;
extern PFNGLMEMORYBARRIERPROC p_glMemoryBarrier;
extern PFNGLPATCHPARAMETERIPROC p_glPatchParameteri;
extern PFNGLPIXELSTOREIPROC p_glPixelStorei;
extern PFNGLREADPIXELSPROC p_glReadPixels;
extern PFNGLRENDERBUFFERSTORAGEPROC p_glRenderbufferStorage;
extern PFNGLSCISSORPROC p_glScissor;
extern PFNGLSHADERSOURCEPROC p_glShaderSource;
extern PFNGLSTENCILFUNCPROC p_glStencilFunc;
extern PFNGLSTENCILMASKPROC p_glStencilMask;
extern PFNGLSTENCILOPPROC p_glStencilOp;
extern PFNGLSTENCILOPSEPARATEPROC p_glStencilOpSeparate;
extern PFNGLTEXIMAGE2DPROC p_glTexImage2D;
extern PFNGLTEXPARAMETERIPROC p_glTexParameteri;
extern PFNGLTEXSTORAGE2DPROC p_glTexStorage2D;
extern PFNGLTEXSUBIMAGE2DPROC p_glTexSubImage2D;
extern PFNGLUNIFORM1FPROC p_glUniform1f;
extern PFNGLUNIFORM1IPROC p_glUniform1i;
extern PFNGLUNIFORM2FPROC p_glUniform2f;
extern PFNGLUNIFORM2FVPROC p_glUniform2fv;
extern PFNGLUNIFORM2IVPROC p_glUniform2iv;
extern PFNGLUNIFORM4FVPROC p_glUniform4fv;
extern PFNGLUNIFORMBLOCKBINDINGPROC p_glUniformBlockBinding;
extern PFNGLUNIFORMMATRIX4FVPROC p_glUniformMatrix4fv;
extern PFNGLUNMAPBUFFERPROC p_glUnmapBuffer;
extern PFNGLUSEPROGRAMPROC p_glUseProgram;
extern PFNGLVALIDATEPROGRAMPROC p_glValidateProgram;
extern PFNGLVERTEXATTRIBPOINTERPROC p_glVertexAttribPointer;
extern PFNGLVIEWPORTPROC p_glViewport;
extern PFNGLWAITSYNCPROC p_glWaitSync;

/* A generic entry point, and the front end's lookup for one: the same shape as
   libretro.h's retro_proc_address_t / retro_hw_get_proc_address_t, repeated
   here so this header does not depend on the libretro API headers. */
typedef void (*ygl_gl_proc_t)(void);
typedef ygl_gl_proc_t (*ygl_gl_get_proc_address_t)(const char *name);

/* Returns the number of entry points that could not be resolved (0 = all of
   them), after logging what came from where.  get_proc_address is the front
   end's callback and may be NULL.  Calling it again simply re-resolves. */
int YglGlProcResolve(ygl_gl_get_proc_address_t get_proc_address);

#ifdef __cplusplus
}
#endif

/* The redefinitions.  Everything below this line calls through the table. */
#define glActiveTexture p_glActiveTexture
#define glAttachShader p_glAttachShader
#define glBindAttribLocation p_glBindAttribLocation
#define glBindBuffer p_glBindBuffer
#define glBindBufferBase p_glBindBufferBase
#define glBindFramebuffer p_glBindFramebuffer
#define glBindImageTexture p_glBindImageTexture
#define glBindRenderbuffer p_glBindRenderbuffer
#define glBindTexture p_glBindTexture
#define glBindVertexArray p_glBindVertexArray
#define glBlendFunc p_glBlendFunc
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glClear p_glClear
#define glClearColor p_glClearColor
#define glClearDepthf p_glClearDepthf
#define glClearStencil p_glClearStencil
#define glClientWaitSync p_glClientWaitSync
#define glColorMask p_glColorMask
#define glCompileShader p_glCompileShader
#define glCopyBufferSubData p_glCopyBufferSubData
#define glCreateProgram p_glCreateProgram
#define glCreateShader p_glCreateShader
#define glCullFace p_glCullFace
#define glDeleteBuffers p_glDeleteBuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glDeleteProgram p_glDeleteProgram
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glDeleteShader p_glDeleteShader
#define glDeleteSync p_glDeleteSync
#define glDeleteTextures p_glDeleteTextures
#define glDeleteVertexArrays p_glDeleteVertexArrays
#define glDepthFunc p_glDepthFunc
#define glDepthMask p_glDepthMask
#define glDetachShader p_glDetachShader
#define glDisable p_glDisable
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glDispatchCompute p_glDispatchCompute
#define glDrawArrays p_glDrawArrays
#define glEnable p_glEnable
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glFenceSync p_glFenceSync
#define glFinish p_glFinish
#define glFlush p_glFlush
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFrontFace p_glFrontFace
#define glGenBuffers p_glGenBuffers
#define glGenerateMipmap p_glGenerateMipmap
#define glGenFramebuffers p_glGenFramebuffers
#define glGenRenderbuffers p_glGenRenderbuffers
#define glGenTextures p_glGenTextures
#define glGenVertexArrays p_glGenVertexArrays
#define glGetAttribLocation p_glGetAttribLocation
#define glGetError p_glGetError
#define glGetIntegerv p_glGetIntegerv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glGetProgramiv p_glGetProgramiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glGetShaderiv p_glGetShaderiv
#define glGetString p_glGetString
#define glGetUniformBlockIndex p_glGetUniformBlockIndex
#define glGetUniformLocation p_glGetUniformLocation
#define glLinkProgram p_glLinkProgram
#define glMapBufferRange p_glMapBufferRange
#define glMemoryBarrier p_glMemoryBarrier
#define glPatchParameteri p_glPatchParameteri
#define glPixelStorei p_glPixelStorei
#define glReadPixels p_glReadPixels
#define glRenderbufferStorage p_glRenderbufferStorage
#define glScissor p_glScissor
#define glShaderSource p_glShaderSource
#define glStencilFunc p_glStencilFunc
#define glStencilMask p_glStencilMask
#define glStencilOp p_glStencilOp
#define glStencilOpSeparate p_glStencilOpSeparate
#define glTexImage2D p_glTexImage2D
#define glTexParameteri p_glTexParameteri
#define glTexStorage2D p_glTexStorage2D
#define glTexSubImage2D p_glTexSubImage2D
#define glUniform1f p_glUniform1f
#define glUniform1i p_glUniform1i
#define glUniform2f p_glUniform2f
#define glUniform2fv p_glUniform2fv
#define glUniform2iv p_glUniform2iv
#define glUniform4fv p_glUniform4fv
#define glUniformBlockBinding p_glUniformBlockBinding
#define glUniformMatrix4fv p_glUniformMatrix4fv
#define glUnmapBuffer p_glUnmapBuffer
#define glUseProgram p_glUseProgram
#define glValidateProgram p_glValidateProgram
#define glVertexAttribPointer p_glVertexAttribPointer
#define glViewport p_glViewport
#define glWaitSync p_glWaitSync

#endif /* __LIBRETRO__ */

#endif /* YGL_GLPROC_H */
