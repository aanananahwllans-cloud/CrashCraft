/*
 * OpenGL entry points past 1.1 (Windows' opengl32 exports only 1.1).
 * Loaded once through SDL after the context exists; GLLoad() returns 0 if any is missing.
 */
#ifndef _PC_GFX_GLLOAD_H_
#define _PC_GFX_GLLOAD_H_

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>
#include <GL/glext.h>

#define GLLOAD_FUNCS(X) \
  X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
  X(PFNGLBLENDEQUATIONPROC, glBlendEquation) \
  X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
  X(PFNGLGENBUFFERSPROC, glGenBuffers) \
  X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
  X(PFNGLBINDBUFFERPROC, glBindBuffer) \
  X(PFNGLBUFFERDATAPROC, glBufferData) \
  X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
  X(PFNGLCREATESHADERPROC, glCreateShader) \
  X(PFNGLDELETESHADERPROC, glDeleteShader) \
  X(PFNGLSHADERSOURCEPROC, glShaderSource) \
  X(PFNGLCOMPILESHADERPROC, glCompileShader) \
  X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
  X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
  X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
  X(PFNGLATTACHSHADERPROC, glAttachShader) \
  X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) \
  X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
  X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
  X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
  X(PFNGLUSEPROGRAMPROC, glUseProgram) \
  X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
  X(PFNGLUNIFORM1IPROC, glUniform1i) \
  X(PFNGLUNIFORM1FPROC, glUniform1f) \
  X(PFNGLUNIFORM2FPROC, glUniform2f) \
  X(PFNGLUNIFORM3FPROC, glUniform3f) \
  X(PFNGLUNIFORM4FPROC, glUniform4f) \
  X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) \
  X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
  X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) \
  X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
  X(PFNGLGETATTRIBLOCATIONPROC, glGetAttribLocation) \
  X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
  X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)

#ifdef __cplusplus
extern "C" {
#endif

#define GLLOAD_DECLARE(type, name) extern type p_##name;
GLLOAD_FUNCS(GLLOAD_DECLARE)
#undef GLLOAD_DECLARE

int GLLoad(void *(*get_proc)(const char *));

#ifdef __cplusplus
}
#endif

#ifndef GLLOAD_IMPL
#define glActiveTexture p_glActiveTexture
#define glBlendEquation p_glBlendEquation
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glGenBuffers p_glGenBuffers
#define glDeleteBuffers p_glDeleteBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glCreateShader p_glCreateShader
#define glDeleteShader p_glDeleteShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glBindAttribLocation p_glBindAttribLocation
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glUniform1f p_glUniform1f
#define glUniform2f p_glUniform2f
#define glUniform3f p_glUniform3f
#define glUniform4f p_glUniform4f
#define glUniformMatrix4fv p_glUniformMatrix4fv
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glVertexAttribPointer p_glVertexAttribPointer
#define glGetAttribLocation p_glGetAttribLocation
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#endif

#endif /* _PC_GFX_GLLOAD_H_ */
