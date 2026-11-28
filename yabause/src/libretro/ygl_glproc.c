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

/* Resolver for the entry points declared in ygl_glproc.h.  See that header for
   why the core does not link a GL library.

   Two sources, in this order:
     1. the front end's retro_hw_render_callback.get_proc_address -- the
        answer that is guaranteed to belong to the context the front end made
        current for us;
     2. dlsym(RTLD_DEFAULT) -- only for names the callback did not know.  This
        keeps the core usable with a front end that sets no callback at all
        while still having its GL driver loaded (the usual libretro front end
        links EGLS/GLES itself), and it is logged loudly because a pointer from
        here may belong to a different library than the callback's answers.
   Both are counted and reported, so a run tells which path a given front end
   takes instead of leaving it to be guessed.

   The loop is the same idea as libretro-common's rglgen_resolve_symbols_custom()
   (MIT): a { name, slot } table walked with the front end's callback.  Only the
   loop is taken from there -- the table has to be our own, because glsym's ES3
   table lists extension entry points only and none of the core ES 3.2 names the
   renderer calls. */

#include <dlfcn.h>
#include <stddef.h>
#include <string.h>

#include "log.h"
#include "ygl_glproc.h"

/* Definitions of the pointer variables (the header only declares them; the
   redefinitions in it do not touch these names). */
PFNGLACTIVETEXTUREPROC p_glActiveTexture;
PFNGLATTACHSHADERPROC p_glAttachShader;
PFNGLBINDATTRIBLOCATIONPROC p_glBindAttribLocation;
PFNGLBINDBUFFERPROC p_glBindBuffer;
PFNGLBINDBUFFERBASEPROC p_glBindBufferBase;
PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
PFNGLBINDIMAGETEXTUREPROC p_glBindImageTexture;
PFNGLBINDRENDERBUFFERPROC p_glBindRenderbuffer;
PFNGLBINDTEXTUREPROC p_glBindTexture;
PFNGLBINDVERTEXARRAYPROC p_glBindVertexArray;
PFNGLBLENDFUNCPROC p_glBlendFunc;
PFNGLBLENDFUNCSEPARATEPROC p_glBlendFuncSeparate;
PFNGLBUFFERDATAPROC p_glBufferData;
PFNGLBUFFERSUBDATAPROC p_glBufferSubData;
PFNGLCHECKFRAMEBUFFERSTATUSPROC p_glCheckFramebufferStatus;
PFNGLCLEARPROC p_glClear;
PFNGLCLEARCOLORPROC p_glClearColor;
PFNGLCLEARDEPTHFPROC p_glClearDepthf;
PFNGLCLEARSTENCILPROC p_glClearStencil;
PFNGLCLIENTWAITSYNCPROC p_glClientWaitSync;
PFNGLCOLORMASKPROC p_glColorMask;
PFNGLCOMPILESHADERPROC p_glCompileShader;
PFNGLCOPYBUFFERSUBDATAPROC p_glCopyBufferSubData;
PFNGLCREATEPROGRAMPROC p_glCreateProgram;
PFNGLCREATESHADERPROC p_glCreateShader;
PFNGLCULLFACEPROC p_glCullFace;
PFNGLDELETEBUFFERSPROC p_glDeleteBuffers;
PFNGLDELETEFRAMEBUFFERSPROC p_glDeleteFramebuffers;
PFNGLDELETEPROGRAMPROC p_glDeleteProgram;
PFNGLDELETERENDERBUFFERSPROC p_glDeleteRenderbuffers;
PFNGLDELETESHADERPROC p_glDeleteShader;
PFNGLDELETESYNCPROC p_glDeleteSync;
PFNGLDELETETEXTURESPROC p_glDeleteTextures;
PFNGLDELETEVERTEXARRAYSPROC p_glDeleteVertexArrays;
PFNGLDEPTHFUNCPROC p_glDepthFunc;
PFNGLDEPTHMASKPROC p_glDepthMask;
PFNGLDETACHSHADERPROC p_glDetachShader;
PFNGLDISABLEPROC p_glDisable;
PFNGLDISABLEVERTEXATTRIBARRAYPROC p_glDisableVertexAttribArray;
PFNGLDISPATCHCOMPUTEPROC p_glDispatchCompute;
PFNGLDRAWARRAYSPROC p_glDrawArrays;
PFNGLENABLEPROC p_glEnable;
PFNGLENABLEVERTEXATTRIBARRAYPROC p_glEnableVertexAttribArray;
PFNGLFENCESYNCPROC p_glFenceSync;
PFNGLFINISHPROC p_glFinish;
PFNGLFLUSHPROC p_glFlush;
PFNGLFRAMEBUFFERRENDERBUFFERPROC p_glFramebufferRenderbuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC p_glFramebufferTexture2D;
PFNGLFRONTFACEPROC p_glFrontFace;
PFNGLGENBUFFERSPROC p_glGenBuffers;
PFNGLGENERATEMIPMAPPROC p_glGenerateMipmap;
PFNGLGENFRAMEBUFFERSPROC p_glGenFramebuffers;
PFNGLGENRENDERBUFFERSPROC p_glGenRenderbuffers;
PFNGLGENTEXTURESPROC p_glGenTextures;
PFNGLGENVERTEXARRAYSPROC p_glGenVertexArrays;
PFNGLGETATTRIBLOCATIONPROC p_glGetAttribLocation;
PFNGLGETERRORPROC p_glGetError;
PFNGLGETINTEGERVPROC p_glGetIntegerv;
PFNGLGETPROGRAMINFOLOGPROC p_glGetProgramInfoLog;
PFNGLGETPROGRAMIVPROC p_glGetProgramiv;
PFNGLGETSHADERINFOLOGPROC p_glGetShaderInfoLog;
PFNGLGETSHADERIVPROC p_glGetShaderiv;
PFNGLGETSTRINGPROC p_glGetString;
PFNGLGETUNIFORMBLOCKINDEXPROC p_glGetUniformBlockIndex;
PFNGLGETUNIFORMLOCATIONPROC p_glGetUniformLocation;
PFNGLLINKPROGRAMPROC p_glLinkProgram;
PFNGLMAPBUFFERRANGEPROC p_glMapBufferRange;
PFNGLMEMORYBARRIERPROC p_glMemoryBarrier;
PFNGLPATCHPARAMETERIPROC p_glPatchParameteri;
PFNGLPIXELSTOREIPROC p_glPixelStorei;
PFNGLREADPIXELSPROC p_glReadPixels;
PFNGLRENDERBUFFERSTORAGEPROC p_glRenderbufferStorage;
PFNGLSCISSORPROC p_glScissor;
PFNGLSHADERSOURCEPROC p_glShaderSource;
PFNGLSTENCILFUNCPROC p_glStencilFunc;
PFNGLSTENCILMASKPROC p_glStencilMask;
PFNGLSTENCILOPPROC p_glStencilOp;
PFNGLSTENCILOPSEPARATEPROC p_glStencilOpSeparate;
PFNGLTEXIMAGE2DPROC p_glTexImage2D;
PFNGLTEXPARAMETERIPROC p_glTexParameteri;
PFNGLTEXSTORAGE2DPROC p_glTexStorage2D;
PFNGLTEXSUBIMAGE2DPROC p_glTexSubImage2D;
PFNGLUNIFORM1FPROC p_glUniform1f;
PFNGLUNIFORM1IPROC p_glUniform1i;
PFNGLUNIFORM2FPROC p_glUniform2f;
PFNGLUNIFORM2FVPROC p_glUniform2fv;
PFNGLUNIFORM2IVPROC p_glUniform2iv;
PFNGLUNIFORM4FVPROC p_glUniform4fv;
PFNGLUNIFORMBLOCKBINDINGPROC p_glUniformBlockBinding;
PFNGLUNIFORMMATRIX4FVPROC p_glUniformMatrix4fv;
PFNGLUNMAPBUFFERPROC p_glUnmapBuffer;
PFNGLUSEPROGRAMPROC p_glUseProgram;
PFNGLVALIDATEPROGRAMPROC p_glValidateProgram;
PFNGLVERTEXATTRIBPOINTERPROC p_glVertexAttribPointer;
PFNGLVIEWPORTPROC p_glViewport;
PFNGLWAITSYNCPROC p_glWaitSync;

struct ygl_gl_sym {
   const char *name;
   void       *slot;   /* address of the PFN variable; filled with memcpy() */
};

static const struct ygl_gl_sym ygl_gl_syms[] = {
   { "glActiveTexture", &p_glActiveTexture },
   { "glAttachShader", &p_glAttachShader },
   { "glBindAttribLocation", &p_glBindAttribLocation },
   { "glBindBuffer", &p_glBindBuffer },
   { "glBindBufferBase", &p_glBindBufferBase },
   { "glBindFramebuffer", &p_glBindFramebuffer },
   { "glBindImageTexture", &p_glBindImageTexture },
   { "glBindRenderbuffer", &p_glBindRenderbuffer },
   { "glBindTexture", &p_glBindTexture },
   { "glBindVertexArray", &p_glBindVertexArray },
   { "glBlendFunc", &p_glBlendFunc },
   { "glBlendFuncSeparate", &p_glBlendFuncSeparate },
   { "glBufferData", &p_glBufferData },
   { "glBufferSubData", &p_glBufferSubData },
   { "glCheckFramebufferStatus", &p_glCheckFramebufferStatus },
   { "glClear", &p_glClear },
   { "glClearColor", &p_glClearColor },
   { "glClearDepthf", &p_glClearDepthf },
   { "glClearStencil", &p_glClearStencil },
   { "glClientWaitSync", &p_glClientWaitSync },
   { "glColorMask", &p_glColorMask },
   { "glCompileShader", &p_glCompileShader },
   { "glCopyBufferSubData", &p_glCopyBufferSubData },
   { "glCreateProgram", &p_glCreateProgram },
   { "glCreateShader", &p_glCreateShader },
   { "glCullFace", &p_glCullFace },
   { "glDeleteBuffers", &p_glDeleteBuffers },
   { "glDeleteFramebuffers", &p_glDeleteFramebuffers },
   { "glDeleteProgram", &p_glDeleteProgram },
   { "glDeleteRenderbuffers", &p_glDeleteRenderbuffers },
   { "glDeleteShader", &p_glDeleteShader },
   { "glDeleteSync", &p_glDeleteSync },
   { "glDeleteTextures", &p_glDeleteTextures },
   { "glDeleteVertexArrays", &p_glDeleteVertexArrays },
   { "glDepthFunc", &p_glDepthFunc },
   { "glDepthMask", &p_glDepthMask },
   { "glDetachShader", &p_glDetachShader },
   { "glDisable", &p_glDisable },
   { "glDisableVertexAttribArray", &p_glDisableVertexAttribArray },
   { "glDispatchCompute", &p_glDispatchCompute },
   { "glDrawArrays", &p_glDrawArrays },
   { "glEnable", &p_glEnable },
   { "glEnableVertexAttribArray", &p_glEnableVertexAttribArray },
   { "glFenceSync", &p_glFenceSync },
   { "glFinish", &p_glFinish },
   { "glFlush", &p_glFlush },
   { "glFramebufferRenderbuffer", &p_glFramebufferRenderbuffer },
   { "glFramebufferTexture2D", &p_glFramebufferTexture2D },
   { "glFrontFace", &p_glFrontFace },
   { "glGenBuffers", &p_glGenBuffers },
   { "glGenerateMipmap", &p_glGenerateMipmap },
   { "glGenFramebuffers", &p_glGenFramebuffers },
   { "glGenRenderbuffers", &p_glGenRenderbuffers },
   { "glGenTextures", &p_glGenTextures },
   { "glGenVertexArrays", &p_glGenVertexArrays },
   { "glGetAttribLocation", &p_glGetAttribLocation },
   { "glGetError", &p_glGetError },
   { "glGetIntegerv", &p_glGetIntegerv },
   { "glGetProgramInfoLog", &p_glGetProgramInfoLog },
   { "glGetProgramiv", &p_glGetProgramiv },
   { "glGetShaderInfoLog", &p_glGetShaderInfoLog },
   { "glGetShaderiv", &p_glGetShaderiv },
   { "glGetString", &p_glGetString },
   { "glGetUniformBlockIndex", &p_glGetUniformBlockIndex },
   { "glGetUniformLocation", &p_glGetUniformLocation },
   { "glLinkProgram", &p_glLinkProgram },
   { "glMapBufferRange", &p_glMapBufferRange },
   { "glMemoryBarrier", &p_glMemoryBarrier },
   { "glPatchParameteri", &p_glPatchParameteri },
   { "glPixelStorei", &p_glPixelStorei },
   { "glReadPixels", &p_glReadPixels },
   { "glRenderbufferStorage", &p_glRenderbufferStorage },
   { "glScissor", &p_glScissor },
   { "glShaderSource", &p_glShaderSource },
   { "glStencilFunc", &p_glStencilFunc },
   { "glStencilMask", &p_glStencilMask },
   { "glStencilOp", &p_glStencilOp },
   { "glStencilOpSeparate", &p_glStencilOpSeparate },
   { "glTexImage2D", &p_glTexImage2D },
   { "glTexParameteri", &p_glTexParameteri },
   { "glTexStorage2D", &p_glTexStorage2D },
   { "glTexSubImage2D", &p_glTexSubImage2D },
   { "glUniform1f", &p_glUniform1f },
   { "glUniform1i", &p_glUniform1i },
   { "glUniform2f", &p_glUniform2f },
   { "glUniform2fv", &p_glUniform2fv },
   { "glUniform2iv", &p_glUniform2iv },
   { "glUniform4fv", &p_glUniform4fv },
   { "glUniformBlockBinding", &p_glUniformBlockBinding },
   { "glUniformMatrix4fv", &p_glUniformMatrix4fv },
   { "glUnmapBuffer", &p_glUnmapBuffer },
   { "glUseProgram", &p_glUseProgram },
   { "glValidateProgram", &p_glValidateProgram },
   { "glVertexAttribPointer", &p_glVertexAttribPointer },
   { "glViewport", &p_glViewport },
   { "glWaitSync", &p_glWaitSync },
};

#define YGL_GL_SYM_COUNT (sizeof(ygl_gl_syms) / sizeof(ygl_gl_syms[0]))


static void append_name(char *buf, size_t size, size_t *used, const char *name)
{
   size_t len = strlen(name);
   /* ", name" plus the terminator must still fit */
   if (*used + len + 3 > size)
      return;
   if (*used)
      buf[(*used)++] = ',';
   buf[(*used)++] = ' ';
   memcpy(buf + *used, name, len + 1);
   *used += len;
}

int YglGlProcResolve(ygl_gl_get_proc_address_t get_proc_address)
{
   char   fell_back[4096];
   char   missing[2048];
   size_t fb_used = 0, miss_used = 0, from_cb = 0, from_dlsym = 0, i;

   fell_back[0] = '\0';
   missing[0]   = '\0';

   for (i = 0; i < YGL_GL_SYM_COUNT; i++) {
      ygl_gl_proc_t fn = get_proc_address ? get_proc_address(ygl_gl_syms[i].name)
                                          : NULL;
      if (fn != NULL)
         from_cb++;
      else {
         /* dlsym() answers with void *; copying the bytes is the portable
            transfer between the two kinds of pointer (rglgen does the same). */
         void *sym = dlsym(RTLD_DEFAULT, ygl_gl_syms[i].name);
         if (sym != NULL) {
            memcpy(&fn, &sym, sizeof(fn));
            from_dlsym++;
            append_name(fell_back, sizeof(fell_back), &fb_used, ygl_gl_syms[i].name);
         } else
            append_name(missing, sizeof(missing), &miss_used, ygl_gl_syms[i].name);
      }
      if (fn != NULL)
         memcpy(ygl_gl_syms[i].slot, &fn, sizeof(fn));
      else
         memset(ygl_gl_syms[i].slot, 0, sizeof(fn));
   }

   LOG_INFO("glproc: %u entry points (callback=%u dlsym=%u unresolved=%u)",
            (unsigned)YGL_GL_SYM_COUNT, (unsigned)from_cb, (unsigned)from_dlsym,
            (unsigned)(YGL_GL_SYM_COUNT - from_cb - from_dlsym));

   if (get_proc_address == NULL)
      LOG_WARN("glproc: the front end set no get_proc_address, every entry point "
               "came from the process itself");
   else if (from_dlsym != 0)
      LOG_WARN("glproc: %u entry point(s) unknown to the front end's callback, "
               "taken from dlsym: %s", (unsigned)from_dlsym, fell_back);

   if (YGL_GL_SYM_COUNT != from_cb + from_dlsym)
      LOG_WARN("glproc: unresolved: %s", missing);

   return (int)(YGL_GL_SYM_COUNT - from_cb - from_dlsym);
}
