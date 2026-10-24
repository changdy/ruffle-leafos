// #include <glad/glad.h>

// #include <EGL/egl.h>

#include <stdio.h>
#include <string.h>

#include <GLES3/gl32.h>
#include <GLES3/gl3ext.h>

//aarch64-none-linux-gnu-gcc -shared -fPIC -o libgetGlFunc.so getGlFunc.c -L/usr/lib-trimui/ -lGLESv2


const void *getGLFunc(const char *name)
{
    // if(!init)
    // {
    //     if (!gladLoadGLES2Loader((GLADloadproc) eglGetProcAddress)) {
    //         printf("GLES2Loader() failed\n");
    //     }
    //     else {
    //         printf("GLES2Loader() success\n");
    //     }

    //     init=1;
    // }


    //printf("name:%s\n",name);
    const void *f=NULL;
    
if(strcmp(name,"glDrawArraysIndirect")==0)
{
	f=(void*)glDrawArraysIndirect;

	//printf\("get glDrawArraysIndirect\n");
}


else if(strcmp(name,"glGetPointerv")==0)
{
	f=(void*)glGetPointerv;

	//printf\("get glGetPointerv\n");
}


else if(strcmp(name,"glVertexAttribIFormat")==0)
{
	f=(void*)glVertexAttribIFormat;

	//printf\("get glVertexAttribIFormat\n");
}


else if(strcmp(name,"glBindSampler")==0)
{
	f=(void*)glBindSampler;

	//printf\("get glBindSampler\n");
}


else if(strcmp(name,"glUniform3fv")==0)
{
	f=(void*)glUniform3fv;

	//printf\("get glUniform3fv\n");
}


else if(strcmp(name,"glTexSubImage3D")==0)
{
	f=(void*)glTexSubImage3D;

	//printf\("get glTexSubImage3D\n");
}


else if(strcmp(name,"glGetVertexAttribPointerv")==0)
{
	f=(void*)glGetVertexAttribPointerv;

	//printf\("get glGetVertexAttribPointerv\n");
}


else if(strcmp(name,"glProgramUniform3iv")==0)
{
	f=(void*)glProgramUniform3iv;

	//printf\("get glProgramUniform3iv\n");
}


else if(strcmp(name,"glCreateProgram")==0)
{
	f=(void*)glCreateProgram;

	//printf\("get glCreateProgram\n");
}


else if(strcmp(name,"glStencilMaskSeparate")==0)
{
	f=(void*)glStencilMaskSeparate;

	//printf\("get glStencilMaskSeparate\n");
}


else if(strcmp(name,"glGetBooleanv")==0)
{
	f=(void*)glGetBooleanv;

	//printf\("get glGetBooleanv\n");
}


else if(strcmp(name,"glBlendEquationSeparate")==0)
{
	f=(void*)glBlendEquationSeparate;

	//printf\("get glBlendEquationSeparate\n");
}


else if(strcmp(name,"glRenderbufferStorageMultisample")==0)
{
	f=(void*)glRenderbufferStorageMultisample;

	//printf\("get glRenderbufferStorageMultisample\n");
}


else if(strcmp(name,"glGetSamplerParameterfv")==0)
{
	f=(void*)glGetSamplerParameterfv;

	//printf\("get glGetSamplerParameterfv\n");
}


else if(strcmp(name,"glGetUniformBlockIndex")==0)
{
	f=(void*)glGetUniformBlockIndex;

	//printf\("get glGetUniformBlockIndex\n");
}


else if(strcmp(name,"glUniform4iv")==0)
{
	f=(void*)glUniform4iv;

	//printf\("get glUniform4iv\n");
}


else if(strcmp(name,"glVertexAttribBinding")==0)
{
	f=(void*)glVertexAttribBinding;

	//printf\("get glVertexAttribBinding\n");
}


else if(strcmp(name,"glCompressedTexImage2D")==0)
{
	f=(void*)glCompressedTexImage2D;

	//printf\("get glCompressedTexImage2D\n");
}


else if(strcmp(name,"glUniformMatrix2fv")==0)
{
	f=(void*)glUniformMatrix2fv;

	//printf\("get glUniformMatrix2fv\n");
}


else if(strcmp(name,"glCopyTexSubImage3D")==0)
{
	f=(void*)glCopyTexSubImage3D;

	//printf\("get glCopyTexSubImage3D\n");
}


else if(strcmp(name,"glVertexAttrib3fv")==0)
{
	f=(void*)glVertexAttrib3fv;

	//printf\("get glVertexAttrib3fv\n");
}


else if(strcmp(name,"glProgramUniformMatrix2x4fv")==0)
{
	f=(void*)glProgramUniformMatrix2x4fv;

	//printf\("get glProgramUniformMatrix2x4fv\n");
}


else if(strcmp(name,"glGetGraphicsResetStatus")==0)
{
	f=(void*)glGetGraphicsResetStatus;

	//printf\("get glGetGraphicsResetStatus\n");
}


else if(strcmp(name,"glIsTransformFeedback")==0)
{
	f=(void*)glIsTransformFeedback;

	//printf\("get glIsTransformFeedback\n");
}


else if(strcmp(name,"glVertexAttrib1fv")==0)
{
	f=(void*)glVertexAttrib1fv;

	//printf\("get glVertexAttrib1fv\n");
}


else if(strcmp(name,"glGetFramebufferAttachmentParameteriv")==0)
{
	f=(void*)glGetFramebufferAttachmentParameteriv;

	//printf\("get glGetFramebufferAttachmentParameteriv\n");
}


else if(strcmp(name,"glBlendFuncSeparate")==0)
{
	f=(void*)glBlendFuncSeparate;

	//printf\("get glBlendFuncSeparate\n");
}


else if(strcmp(name,"glEnable")==0)
{
	f=(void*)glEnable;

	//printf\("get glEnable\n");
}


else if(strcmp(name,"glProgramUniform4iv")==0)
{
	f=(void*)glProgramUniform4iv;

	//printf\("get glProgramUniform4iv\n");
}


else if(strcmp(name,"glDepthRangef")==0)
{
	f=(void*)glDepthRangef;

	//printf\("get glDepthRangef\n");
}


else if(strcmp(name,"glBufferData")==0)
{
	f=(void*)glBufferData;

	//printf\("get glBufferData\n");
}


else if(strcmp(name,"glCopyTexSubImage2D")==0)
{
	f=(void*)glCopyTexSubImage2D;

	//printf\("get glCopyTexSubImage2D\n");
}


else if(strcmp(name,"glDeleteVertexArrays")==0)
{
	f=(void*)glDeleteVertexArrays;

	//printf\("get glDeleteVertexArrays\n");
}


else if(strcmp(name,"glReadPixels")==0)
{
	f=(void*)glReadPixels;

	//printf\("get glReadPixels\n");
}


else if(strcmp(name,"glGetUniformLocation")==0)
{
	f=(void*)glGetUniformLocation;

	//printf\("get glGetUniformLocation\n");
}


else if(strcmp(name,"glUniformMatrix2x3fv")==0)
{
	f=(void*)glUniformMatrix2x3fv;

	//printf\("get glUniformMatrix2x3fv\n");
}


else if(strcmp(name,"glStencilOpSeparate")==0)
{
	f=(void*)glStencilOpSeparate;

	//printf\("get glStencilOpSeparate\n");
}


else if(strcmp(name,"glUniformMatrix4x2fv")==0)
{
	f=(void*)glUniformMatrix4x2fv;

	//printf\("get glUniformMatrix4x2fv\n");
}


else if(strcmp(name,"glPopDebugGroup")==0)
{
	f=(void*)glPopDebugGroup;

	//printf\("get glPopDebugGroup\n");
}


else if(strcmp(name,"glProgramUniform2uiv")==0)
{
	f=(void*)glProgramUniform2uiv;

	//printf\("get glProgramUniform2uiv\n");
}


else if(strcmp(name,"glUniform2uiv")==0)
{
	f=(void*)glUniform2uiv;

	//printf\("get glUniform2uiv\n");
}


else if(strcmp(name,"glGetnUniformiv")==0)
{
	f=(void*)glGetnUniformiv;

	//printf\("get glGetnUniformiv\n");
}


else if(strcmp(name,"glBindImageTexture")==0)
{
	f=(void*)glBindImageTexture;

	//printf\("get glBindImageTexture\n");
}


else if(strcmp(name,"glGetInteger64v")==0)
{
	f=(void*)glGetInteger64v;

	//printf\("get glGetInteger64v\n");
}


else if(strcmp(name,"glDrawRangeElements")==0)
{
	f=(void*)glDrawRangeElements;

	//printf\("get glDrawRangeElements\n");
}


else if(strcmp(name,"glCheckFramebufferStatus")==0)
{
	f=(void*)glCheckFramebufferStatus;

	//printf\("get glCheckFramebufferStatus\n");
}


else if(strcmp(name,"glUniform1iv")==0)
{
	f=(void*)glUniform1iv;

	//printf\("get glUniform1iv\n");
}


else if(strcmp(name,"glFramebufferTextureLayer")==0)
{
	f=(void*)glFramebufferTextureLayer;

	//printf\("get glFramebufferTextureLayer\n");
}


else if(strcmp(name,"glProgramUniform1i")==0)
{
	f=(void*)glProgramUniform1i;

	//printf\("get glProgramUniform1i\n");
}


else if(strcmp(name,"glTransformFeedbackVaryings")==0)
{
	f=(void*)glTransformFeedbackVaryings;

	//printf\("get glTransformFeedbackVaryings\n");
}


else if(strcmp(name,"glBindFramebuffer")==0)
{
	f=(void*)glBindFramebuffer;

	//printf\("get glBindFramebuffer\n");
}


else if(strcmp(name,"glProgramUniformMatrix2fv")==0)
{
	f=(void*)glProgramUniformMatrix2fv;

	//printf\("get glProgramUniformMatrix2fv\n");
}


else if(strcmp(name,"glDeleteSync")==0)
{
	f=(void*)glDeleteSync;

	//printf\("get glDeleteSync\n");
}


else if(strcmp(name,"glUniform2f")==0)
{
	f=(void*)glUniform2f;

	//printf\("get glUniform2f\n");
}


else if(strcmp(name,"glGetnUniformuiv")==0)
{
	f=(void*)glGetnUniformuiv;

	//printf\("get glGetnUniformuiv\n");
}


else if(strcmp(name,"glSamplerParameterf")==0)
{
	f=(void*)glSamplerParameterf;

	//printf\("get glSamplerParameterf\n");
}


else if(strcmp(name,"glDisablei")==0)
{
	f=(void*)glDisablei;

	//printf\("get glDisablei\n");
}


else if(strcmp(name,"glFramebufferParameteri")==0)
{
	f=(void*)glFramebufferParameteri;

	//printf\("get glFramebufferParameteri\n");
}


else if(strcmp(name,"glGetFramebufferParameteriv")==0)
{
	f=(void*)glGetFramebufferParameteriv;

	//printf\("get glGetFramebufferParameteriv\n");
}


else if(strcmp(name,"glFlushMappedBufferRange")==0)
{
	f=(void*)glFlushMappedBufferRange;

	//printf\("get glFlushMappedBufferRange\n");
}


else if(strcmp(name,"glDebugMessageInsert")==0)
{
	f=(void*)glDebugMessageInsert;

	//printf\("get glDebugMessageInsert\n");
}


else if(strcmp(name,"glIsVertexArray")==0)
{
	f=(void*)glIsVertexArray;

	//printf\("get glIsVertexArray\n");
}


else if(strcmp(name,"glVertexBindingDivisor")==0)
{
	f=(void*)glVertexBindingDivisor;

	//printf\("get glVertexBindingDivisor\n");
}


else if(strcmp(name,"glProgramUniform3ui")==0)
{
	f=(void*)glProgramUniform3ui;

	//printf\("get glProgramUniform3ui\n");
}


else if(strcmp(name,"glValidateProgram")==0)
{
	f=(void*)glValidateProgram;

	//printf\("get glValidateProgram\n");
}


else if(strcmp(name,"glDeleteShader")==0)
{
	f=(void*)glDeleteShader;

	//printf\("get glDeleteShader\n");
}


else if(strcmp(name,"glSamplerParameterIiv")==0)
{
	f=(void*)glSamplerParameterIiv;

	//printf\("get glSamplerParameterIiv\n");
}


else if(strcmp(name,"glIsSampler")==0)
{
	f=(void*)glIsSampler;

	//printf\("get glIsSampler\n");
}


else if(strcmp(name,"glBindRenderbuffer")==0)
{
	f=(void*)glBindRenderbuffer;

	//printf\("get glBindRenderbuffer\n");
}


else if(strcmp(name,"glDisable")==0)
{
	f=(void*)glDisable;

	//printf\("get glDisable\n");
}


else if(strcmp(name,"glProgramUniform1uiv")==0)
{
	f=(void*)glProgramUniform1uiv;

	//printf\("get glProgramUniform1uiv\n");
}


else if(strcmp(name,"glWaitSync")==0)
{
	f=(void*)glWaitSync;

	//printf\("get glWaitSync\n");
}


else if(strcmp(name,"glProgramUniform4ui")==0)
{
	f=(void*)glProgramUniform4ui;

	//printf\("get glProgramUniform4ui\n");
}


else if(strcmp(name,"glTexStorage2DMultisample")==0)
{
	f=(void*)glTexStorage2DMultisample;

	//printf\("get glTexStorage2DMultisample\n");
}


else if(strcmp(name,"glProgramUniform4fv")==0)
{
	f=(void*)glProgramUniform4fv;

	//printf\("get glProgramUniform4fv\n");
}


else if(strcmp(name,"glGetUniformfv")==0)
{
	f=(void*)glGetUniformfv;

	//printf\("get glGetUniformfv\n");
}


else if(strcmp(name,"glProgramUniform3uiv")==0)
{
	f=(void*)glProgramUniform3uiv;

	//printf\("get glProgramUniform3uiv\n");
}


else if(strcmp(name,"glUniform2iv")==0)
{
	f=(void*)glUniform2iv;

	//printf\("get glUniform2iv\n");
}


else if(strcmp(name,"glUniform1f")==0)
{
	f=(void*)glUniform1f;

	//printf\("get glUniform1f\n");
}


else if(strcmp(name,"glBindTransformFeedback")==0)
{
	f=(void*)glBindTransformFeedback;

	//printf\("get glBindTransformFeedback\n");
}


else if(strcmp(name,"glGetnUniformfv")==0)
{
	f=(void*)glGetnUniformfv;

	//printf\("get glGetnUniformfv\n");
}


else if(strcmp(name,"glClearDepthf")==0)
{
	f=(void*)glClearDepthf;

	//printf\("get glClearDepthf\n");
}


else if(strcmp(name,"glUniform2ui")==0)
{
	f=(void*)glUniform2ui;

	//printf\("get glUniform2ui\n");
}


else if(strcmp(name,"glStencilOp")==0)
{
	f=(void*)glStencilOp;

	//printf\("get glStencilOp\n");
}


else if(strcmp(name,"glIsShader")==0)
{
	f=(void*)glIsShader;

	//printf\("get glIsShader\n");
}


else if(strcmp(name,"glFlush")==0)
{
	f=(void*)glFlush;

	//printf\("get glFlush\n");
}


else if(strcmp(name,"glProgramUniformMatrix2x3fv")==0)
{
	f=(void*)glProgramUniformMatrix2x3fv;

	//printf\("get glProgramUniformMatrix2x3fv\n");
}


else if(strcmp(name,"glClearBufferiv")==0)
{
	f=(void*)glClearBufferiv;

	//printf\("get glClearBufferiv\n");
}


else if(strcmp(name,"glDrawElementsInstancedBaseVertex")==0)
{
	f=(void*)glDrawElementsInstancedBaseVertex;

	//printf\("get glDrawElementsInstancedBaseVertex\n");
}


else if(strcmp(name,"glGetUniformIndices")==0)
{
	f=(void*)glGetUniformIndices;

	//printf\("get glGetUniformIndices\n");
}


else if(strcmp(name,"glProgramUniform4f")==0)
{
	f=(void*)glProgramUniform4f;

	//printf\("get glProgramUniform4f\n");
}


else if(strcmp(name,"glGetTexParameterIiv")==0)
{
	f=(void*)glGetTexParameterIiv;

	//printf\("get glGetTexParameterIiv\n");
}


else if(strcmp(name,"glVertexAttribI4uiv")==0)
{
	f=(void*)glVertexAttribI4uiv;

	//printf\("get glVertexAttribI4uiv\n");
}


else if(strcmp(name,"glSamplerParameterIuiv")==0)
{
	f=(void*)glSamplerParameterIuiv;

	//printf\("get glSamplerParameterIuiv\n");
}


else if(strcmp(name,"glObjectLabel")==0)
{
	f=(void*)glObjectLabel;

	//printf\("get glObjectLabel\n");
}


else if(strcmp(name,"glGetProgramResourceIndex")==0)
{
	f=(void*)glGetProgramResourceIndex;

	//printf\("get glGetProgramResourceIndex\n");
}


else if(strcmp(name,"glProgramUniform4uiv")==0)
{
	f=(void*)glProgramUniform4uiv;

	//printf\("get glProgramUniform4uiv\n");
}


else if(strcmp(name,"glProgramUniformMatrix3x4fv")==0)
{
	f=(void*)glProgramUniformMatrix3x4fv;

	//printf\("get glProgramUniformMatrix3x4fv\n");
}


else if(strcmp(name,"glActiveTexture")==0)
{
	f=(void*)glActiveTexture;

	//printf\("get glActiveTexture\n");
}


else if(strcmp(name,"glCompressedTexSubImage2D")==0)
{
	f=(void*)glCompressedTexSubImage2D;

	//printf\("get glCompressedTexSubImage2D\n");
}


else if(strcmp(name,"glGetBufferParameteriv")==0)
{
	f=(void*)glGetBufferParameteriv;

	//printf\("get glGetBufferParameteriv\n");
}


else if(strcmp(name,"glProgramUniformMatrix4x3fv")==0)
{
	f=(void*)glProgramUniformMatrix4x3fv;

	//printf\("get glProgramUniformMatrix4x3fv\n");
}


else if(strcmp(name,"glReadnPixels")==0)
{
	f=(void*)glReadnPixels;

	//printf\("get glReadnPixels\n");
}


else if(strcmp(name,"glUniform1fv")==0)
{
	f=(void*)glUniform1fv;

	//printf\("get glUniform1fv\n");
}


else if(strcmp(name,"glCompressedTexImage3D")==0)
{
	f=(void*)glCompressedTexImage3D;

	//printf\("get glCompressedTexImage3D\n");
}


else if(strcmp(name,"glTexParameterIiv")==0)
{
	f=(void*)glTexParameterIiv;

	//printf\("get glTexParameterIiv\n");
}


else if(strcmp(name,"glDrawRangeElementsBaseVertex")==0)
{
	f=(void*)glDrawRangeElementsBaseVertex;

	//printf\("get glDrawRangeElementsBaseVertex\n");
}


else if(strcmp(name,"glUniform2fv")==0)
{
	f=(void*)glUniform2fv;

	//printf\("get glUniform2fv\n");
}


else if(strcmp(name,"glUniformMatrix3x4fv")==0)
{
	f=(void*)glUniformMatrix3x4fv;

	//printf\("get glUniformMatrix3x4fv\n");
}


else if(strcmp(name,"glGetString")==0)
{
	f=(void*)glGetString;

	//printf\("get glGetString\n");
}


else if(strcmp(name,"glGetIntegerv")==0)
{
	f=(void*)glGetIntegerv;

	//printf\("get glGetIntegerv\n");
}


else if(strcmp(name,"glProgramUniform3i")==0)
{
	f=(void*)glProgramUniform3i;

	//printf\("get glProgramUniform3i\n");
}


else if(strcmp(name,"glBlendFuncSeparatei")==0)
{
	f=(void*)glBlendFuncSeparatei;

	//printf\("get glBlendFuncSeparatei\n");
}


else if(strcmp(name,"glVertexAttrib2fv")==0)
{
	f=(void*)glVertexAttrib2fv;

	//printf\("get glVertexAttrib2fv\n");
}


else if(strcmp(name,"glShaderSource")==0)
{
	f=(void*)glShaderSource;

	//printf\("get glShaderSource\n");
}


else if(strcmp(name,"glIsQuery")==0)
{
	f=(void*)glIsQuery;

	//printf\("get glIsQuery\n");
}


else if(strcmp(name,"glGenBuffers")==0)
{
	f=(void*)glGenBuffers;

	//printf\("get glGenBuffers\n");
}


else if(strcmp(name,"glShaderBinary")==0)
{
	f=(void*)glShaderBinary;

	//printf\("get glShaderBinary\n");
}


else if(strcmp(name,"glActiveShaderProgram")==0)
{
	f=(void*)glActiveShaderProgram;

	//printf\("get glActiveShaderProgram\n");
}


else if(strcmp(name,"glGetProgramPipelineInfoLog")==0)
{
	f=(void*)glGetProgramPipelineInfoLog;

	//printf\("get glGetProgramPipelineInfoLog\n");
}


else if(strcmp(name,"glProgramUniform1ui")==0)
{
	f=(void*)glProgramUniform1ui;

	//printf\("get glProgramUniform1ui\n");
}


else if(strcmp(name,"glProgramUniformMatrix3x2fv")==0)
{
	f=(void*)glProgramUniformMatrix3x2fv;

	//printf\("get glProgramUniformMatrix3x2fv\n");
}


else if(strcmp(name,"glProgramUniform3fv")==0)
{
	f=(void*)glProgramUniform3fv;

	//printf\("get glProgramUniform3fv\n");
}


else if(strcmp(name,"glDrawArraysInstanced")==0)
{
	f=(void*)glDrawArraysInstanced;

	//printf\("get glDrawArraysInstanced\n");
}


else if(strcmp(name,"glBlitFramebuffer")==0)
{
	f=(void*)glBlitFramebuffer;

	//printf\("get glBlitFramebuffer\n");
}


else if(strcmp(name,"glLinkProgram")==0)
{
	f=(void*)glLinkProgram;

	//printf\("get glLinkProgram\n");
}


else if(strcmp(name,"glReleaseShaderCompiler")==0)
{
	f=(void*)glReleaseShaderCompiler;

	//printf\("get glReleaseShaderCompiler\n");
}


else if(strcmp(name,"glUniform4uiv")==0)
{
	f=(void*)glUniform4uiv;

	//printf\("get glUniform4uiv\n");
}


else if(strcmp(name,"glUniformBlockBinding")==0)
{
	f=(void*)glUniformBlockBinding;

	//printf\("get glUniformBlockBinding\n");
}


else if(strcmp(name,"glVertexAttrib2f")==0)
{
	f=(void*)glVertexAttrib2f;

	//printf\("get glVertexAttrib2f\n");
}


else if(strcmp(name,"glGetActiveUniformBlockiv")==0)
{
	f=(void*)glGetActiveUniformBlockiv;

	//printf\("get glGetActiveUniformBlockiv\n");
}


else if(strcmp(name,"glEnablei")==0)
{
	f=(void*)glEnablei;

	//printf\("get glEnablei\n");
}


else if(strcmp(name,"glGetInteger64i_v")==0)
{
	f=(void*)glGetInteger64i_v;

	//printf\("get glGetInteger64i_v\n");
}


else if(strcmp(name,"glGetDebugMessageLog")==0)
{
	f=(void*)glGetDebugMessageLog;

	//printf\("get glGetDebugMessageLog\n");
}


else if(strcmp(name,"glGetUniformiv")==0)
{
	f=(void*)glGetUniformiv;

	//printf\("get glGetUniformiv\n");
}


else if(strcmp(name,"glUniform4ui")==0)
{
	f=(void*)glUniform4ui;

	//printf\("get glUniform4ui\n");
}


else if(strcmp(name,"glReadBuffer")==0)
{
	f=(void*)glReadBuffer;

	//printf\("get glReadBuffer\n");
}


else if(strcmp(name,"glBlendFunci")==0)
{
	f=(void*)glBlendFunci;

	//printf\("get glBlendFunci\n");
}


else if(strcmp(name,"glPrimitiveBoundingBox")==0)
{
	f=(void*)glPrimitiveBoundingBox;

	//printf\("get glPrimitiveBoundingBox\n");
}


else if(strcmp(name,"glPixelStorei")==0)
{
	f=(void*)glPixelStorei;

	//printf\("get glPixelStorei\n");
}


else if(strcmp(name,"glBindBuffer")==0)
{
	f=(void*)glBindBuffer;

	//printf\("get glBindBuffer\n");
}


else if(strcmp(name,"glTexParameterf")==0)
{
	f=(void*)glTexParameterf;

	//printf\("get glTexParameterf\n");
}


else if(strcmp(name,"glUniform3ui")==0)
{
	f=(void*)glUniform3ui;

	//printf\("get glUniform3ui\n");
}


else if(strcmp(name,"glProgramUniform2i")==0)
{
	f=(void*)glProgramUniform2i;

	//printf\("get glProgramUniform2i\n");
}


else if(strcmp(name,"glIsProgramPipeline")==0)
{
	f=(void*)glIsProgramPipeline;

	//printf\("get glIsProgramPipeline\n");
}


else if(strcmp(name,"glTexStorage3DMultisample")==0)
{
	f=(void*)glTexStorage3DMultisample;

	//printf\("get glTexStorage3DMultisample\n");
}


else if(strcmp(name,"glColorMaski")==0)
{
	f=(void*)glColorMaski;

	//printf\("get glColorMaski\n");
}


else if(strcmp(name,"glCullFace")==0)
{
	f=(void*)glCullFace;

	//printf\("get glCullFace\n");
}


else if(strcmp(name,"glGenRenderbuffers")==0)
{
	f=(void*)glGenRenderbuffers;

	//printf\("get glGenRenderbuffers\n");
}


else if(strcmp(name,"glGetSamplerParameterIiv")==0)
{
	f=(void*)glGetSamplerParameterIiv;

	//printf\("get glGetSamplerParameterIiv\n");
}


else if(strcmp(name,"glGetSynciv")==0)
{
	f=(void*)glGetSynciv;

	//printf\("get glGetSynciv\n");
}


else if(strcmp(name,"glDeleteTextures")==0)
{
	f=(void*)glDeleteTextures;

	//printf\("get glDeleteTextures\n");
}


else if(strcmp(name,"glDeleteTransformFeedbacks")==0)
{
	f=(void*)glDeleteTransformFeedbacks;

	//printf\("get glDeleteTransformFeedbacks\n");
}


else if(strcmp(name,"glBeginQuery")==0)
{
	f=(void*)glBeginQuery;

	//printf\("get glBeginQuery\n");
}


else if(strcmp(name,"glClearStencil")==0)
{
	f=(void*)glClearStencil;

	//printf\("get glClearStencil\n");
}


else if(strcmp(name,"glGetProgramInfoLog")==0)
{
	f=(void*)glGetProgramInfoLog;

	//printf\("get glGetProgramInfoLog\n");
}


else if(strcmp(name,"glFramebufferTexture2D")==0)
{
	f=(void*)glFramebufferTexture2D;

	//printf\("get glFramebufferTexture2D\n");
}


else if(strcmp(name,"glColorMask")==0)
{
	f=(void*)glColorMask;

	//printf\("get glColorMask\n");
}


else if(strcmp(name,"glGetProgramResourceName")==0)
{
	f=(void*)glGetProgramResourceName;

	//printf\("get glGetProgramResourceName\n");
}


else if(strcmp(name,"glFenceSync")==0)
{
	f=(void*)glFenceSync;

	//printf\("get glFenceSync\n");
}


else if(strcmp(name,"glGetProgramPipelineiv")==0)
{
	f=(void*)glGetProgramPipelineiv;

	//printf\("get glGetProgramPipelineiv\n");
}


else if(strcmp(name,"glUniformMatrix2x4fv")==0)
{
	f=(void*)glUniformMatrix2x4fv;

	//printf\("get glUniformMatrix2x4fv\n");
}


else if(strcmp(name,"glDepthMask")==0)
{
	f=(void*)glDepthMask;

	//printf\("get glDepthMask\n");
}


else if(strcmp(name,"glBlendEquationi")==0)
{
	f=(void*)glBlendEquationi;

	//printf\("get glBlendEquationi\n");
}


else if(strcmp(name,"glVertexAttribFormat")==0)
{
	f=(void*)glVertexAttribFormat;

	//printf\("get glVertexAttribFormat\n");
}


else if(strcmp(name,"glRenderbufferStorage")==0)
{
	f=(void*)glRenderbufferStorage;

	//printf\("get glRenderbufferStorage\n");
}


else if(strcmp(name,"glGetQueryiv")==0)
{
	f=(void*)glGetQueryiv;

	//printf\("get glGetQueryiv\n");
}


else if(strcmp(name,"glClearColor")==0)
{
	f=(void*)glClearColor;

	//printf\("get glClearColor\n");
}


else if(strcmp(name,"glBindBufferRange")==0)
{
	f=(void*)glBindBufferRange;

	//printf\("get glBindBufferRange\n");
}


else if(strcmp(name,"glIsSync")==0)
{
	f=(void*)glIsSync;

	//printf\("get glIsSync\n");
}


else if(strcmp(name,"glGenerateMipmap")==0)
{
	f=(void*)glGenerateMipmap;

	//printf\("get glGenerateMipmap\n");
}


else if(strcmp(name,"glGetBooleani_v")==0)
{
	f=(void*)glGetBooleani_v;

	//printf\("get glGetBooleani_v\n");
}


else if(strcmp(name,"glGenTextures")==0)
{
	f=(void*)glGenTextures;

	//printf\("get glGenTextures\n");
}


else if(strcmp(name,"glGetProgramResourceiv")==0)
{
	f=(void*)glGetProgramResourceiv;

	//printf\("get glGetProgramResourceiv\n");
}


else if(strcmp(name,"glMemoryBarrierByRegion")==0)
{
	f=(void*)glMemoryBarrierByRegion;

	//printf\("get glMemoryBarrierByRegion\n");
}


else if(strcmp(name,"glGetRenderbufferParameteriv")==0)
{
	f=(void*)glGetRenderbufferParameteriv;

	//printf\("get glGetRenderbufferParameteriv\n");
}


else if(strcmp(name,"glGetIntegeri_v")==0)
{
	f=(void*)glGetIntegeri_v;

	//printf\("get glGetIntegeri_v\n");
}


else if(strcmp(name,"glSamplerParameteriv")==0)
{
	f=(void*)glSamplerParameteriv;

	//printf\("get glSamplerParameteriv\n");
}


else if(strcmp(name,"glEndTransformFeedback")==0)
{
	f=(void*)glEndTransformFeedback;

	//printf\("get glEndTransformFeedback\n");
}


else if(strcmp(name,"glUniformMatrix3fv")==0)
{
	f=(void*)glUniformMatrix3fv;

	//printf\("get glUniformMatrix3fv\n");
}


else if(strcmp(name,"glTexImage2D")==0)
{
	f=(void*)glTexImage2D;

	//printf\("get glTexImage2D\n");
}


else if(strcmp(name,"glDeleteRenderbuffers")==0)
{
	f=(void*)glDeleteRenderbuffers;

	//printf\("get glDeleteRenderbuffers\n");
}


else if(strcmp(name,"glUnmapBuffer")==0)
{
	f=(void*)glUnmapBuffer;

	//printf\("get glUnmapBuffer\n");
}


else if(strcmp(name,"glProgramUniformMatrix3fv")==0)
{
	f=(void*)glProgramUniformMatrix3fv;

	//printf\("get glProgramUniformMatrix3fv\n");
}


else if(strcmp(name,"glMemoryBarrier")==0)
{
	f=(void*)glMemoryBarrier;

	//printf\("get glMemoryBarrier\n");
}


else if(strcmp(name,"glUniform3f")==0)
{
	f=(void*)glUniform3f;

	//printf\("get glUniform3f\n");
}


else if(strcmp(name,"glPauseTransformFeedback")==0)
{
	f=(void*)glPauseTransformFeedback;

	//printf\("get glPauseTransformFeedback\n");
}


else if(strcmp(name,"glResumeTransformFeedback")==0)
{
	f=(void*)glResumeTransformFeedback;

	//printf\("get glResumeTransformFeedback\n");
}


else if(strcmp(name,"glVertexAttribPointer")==0)
{
	f=(void*)glVertexAttribPointer;

	//printf\("get glVertexAttribPointer\n");
}


else if(strcmp(name,"glGetProgramInterfaceiv")==0)
{
	f=(void*)glGetProgramInterfaceiv;

	//printf\("get glGetProgramInterfaceiv\n");
}


else if(strcmp(name,"glBindProgramPipeline")==0)
{
	f=(void*)glBindProgramPipeline;

	//printf\("get glBindProgramPipeline\n");
}


else if(strcmp(name,"glProgramUniform4i")==0)
{
	f=(void*)glProgramUniform4i;

	//printf\("get glProgramUniform4i\n");
}


else if(strcmp(name,"glUniform1i")==0)
{
	f=(void*)glUniform1i;

	//printf\("get glUniform1i\n");
}


else if(strcmp(name,"glClearBufferuiv")==0)
{
	f=(void*)glClearBufferuiv;

	//printf\("get glClearBufferuiv\n");
}


else if(strcmp(name,"glIsEnabled")==0)
{
	f=(void*)glIsEnabled;

	//printf\("get glIsEnabled\n");
}


else if(strcmp(name,"glAttachShader")==0)
{
	f=(void*)glAttachShader;

	//printf\("get glAttachShader\n");
}


else if(strcmp(name,"glProgramUniform1fv")==0)
{
	f=(void*)glProgramUniform1fv;

	//printf\("get glProgramUniform1fv\n");
}


else if(strcmp(name,"glGetAttribLocation")==0)
{
	f=(void*)glGetAttribLocation;

	//printf\("get glGetAttribLocation\n");
}


else if(strcmp(name,"glCompressedTexSubImage3D")==0)
{
	f=(void*)glCompressedTexSubImage3D;

	//printf\("get glCompressedTexSubImage3D\n");
}


else if(strcmp(name,"glGetSamplerParameterIuiv")==0)
{
	f=(void*)glGetSamplerParameterIuiv;

	//printf\("get glGetSamplerParameterIuiv\n");
}


else if(strcmp(name,"glGenProgramPipelines")==0)
{
	f=(void*)glGenProgramPipelines;

	//printf\("get glGenProgramPipelines\n");
}


else if(strcmp(name,"glUniform4fv")==0)
{
	f=(void*)glUniform4fv;

	//printf\("get glUniform4fv\n");
}


else if(strcmp(name,"glGetActiveUniform")==0)
{
	f=(void*)glGetActiveUniform;

	//printf\("get glGetActiveUniform\n");
}


else if(strcmp(name,"glGetTexParameterIuiv")==0)
{
	f=(void*)glGetTexParameterIuiv;

	//printf\("get glGetTexParameterIuiv\n");
}


else if(strcmp(name,"glGetTexParameterfv")==0)
{
	f=(void*)glGetTexParameterfv;

	//printf\("get glGetTexParameterfv\n");
}


else if(strcmp(name,"glDrawElementsBaseVertex")==0)
{
	f=(void*)glDrawElementsBaseVertex;

	//printf\("get glDrawElementsBaseVertex\n");
}


else if(strcmp(name,"glGetVertexAttribIiv")==0)
{
	f=(void*)glGetVertexAttribIiv;

	//printf\("get glGetVertexAttribIiv\n");
}


else if(strcmp(name,"glGetProgramBinary")==0)
{
	f=(void*)glGetProgramBinary;

	//printf\("get glGetProgramBinary\n");
}


else if(strcmp(name,"glMinSampleShading")==0)
{
	f=(void*)glMinSampleShading;

	//printf\("get glMinSampleShading\n");
}


else if(strcmp(name,"glFrontFace")==0)
{
	f=(void*)glFrontFace;

	//printf\("get glFrontFace\n");
}


else if(strcmp(name,"glGetActiveUniformsiv")==0)
{
	f=(void*)glGetActiveUniformsiv;

	//printf\("get glGetActiveUniformsiv\n");
}


else if(strcmp(name,"glTexBuffer")==0)
{
	f=(void*)glTexBuffer;

	//printf\("get glTexBuffer\n");
}


else if(strcmp(name,"glGetTexLevelParameterfv")==0)
{
	f=(void*)glGetTexLevelParameterfv;

	//printf\("get glGetTexLevelParameterfv\n");
}


else if(strcmp(name,"glGetObjectLabel")==0)
{
	f=(void*)glGetObjectLabel;

	//printf\("get glGetObjectLabel\n");
}


else if(strcmp(name,"glUniformMatrix3x2fv")==0)
{
	f=(void*)glUniformMatrix3x2fv;

	//printf\("get glUniformMatrix3x2fv\n");
}


else if(strcmp(name,"glVertexAttribI4ui")==0)
{
	f=(void*)glVertexAttribI4ui;

	//printf\("get glVertexAttribI4ui\n");
}


else if(strcmp(name,"glVertexAttrib4fv")==0)
{
	f=(void*)glVertexAttrib4fv;

	//printf\("get glVertexAttrib4fv\n");
}


else if(strcmp(name,"glGetBufferPointerv")==0)
{
	f=(void*)glGetBufferPointerv;

	//printf\("get glGetBufferPointerv\n");
}


else if(strcmp(name,"glEnableVertexAttribArray")==0)
{
	f=(void*)glEnableVertexAttribArray;

	//printf\("get glEnableVertexAttribArray\n");
}


else if(strcmp(name,"glTexStorage2D")==0)
{
	f=(void*)glTexStorage2D;

	//printf\("get glTexStorage2D\n");
}


else if(strcmp(name,"glGetMultisamplefv")==0)
{
	f=(void*)glGetMultisamplefv;

	//printf\("get glGetMultisamplefv\n");
}


else if(strcmp(name,"glGetBufferParameteri64v")==0)
{
	f=(void*)glGetBufferParameteri64v;

	//printf\("get glGetBufferParameteri64v\n");
}


else if(strcmp(name,"glGetAttachedShaders")==0)
{
	f=(void*)glGetAttachedShaders;

	//printf\("get glGetAttachedShaders\n");
}


else if(strcmp(name,"glGetShaderInfoLog")==0)
{
	f=(void*)glGetShaderInfoLog;

	//printf\("get glGetShaderInfoLog\n");
}


else if(strcmp(name,"glPatchParameteri")==0)
{
	f=(void*)glPatchParameteri;

	//printf\("get glPatchParameteri\n");
}


else if(strcmp(name,"glProgramUniform2ui")==0)
{
	f=(void*)glProgramUniform2ui;

	//printf\("get glProgramUniform2ui\n");
}


else if(strcmp(name,"glDispatchCompute")==0)
{
	f=(void*)glDispatchCompute;

	//printf\("get glDispatchCompute\n");
}


else if(strcmp(name,"glGetActiveUniformBlockName")==0)
{
	f=(void*)glGetActiveUniformBlockName;

	//printf\("get glGetActiveUniformBlockName\n");
}


else if(strcmp(name,"glUseProgram")==0)
{
	f=(void*)glUseProgram;

	//printf\("get glUseProgram\n");
}


else if(strcmp(name,"glBlendEquation")==0)
{
	f=(void*)glBlendEquation;

	//printf\("get glBlendEquation\n");
}


else if(strcmp(name,"glDepthFunc")==0)
{
	f=(void*)glDepthFunc;

	//printf\("get glDepthFunc\n");
}


else if(strcmp(name,"glStencilFunc")==0)
{
	f=(void*)glStencilFunc;

	//printf\("get glStencilFunc\n");
}


else if(strcmp(name,"glGetSamplerParameteriv")==0)
{
	f=(void*)glGetSamplerParameteriv;

	//printf\("get glGetSamplerParameteriv\n");
}


else if(strcmp(name,"glBeginTransformFeedback")==0)
{
	f=(void*)glBeginTransformFeedback;

	//printf\("get glBeginTransformFeedback\n");
}


else if(strcmp(name,"glIsEnabledi")==0)
{
	f=(void*)glIsEnabledi;

	//printf\("get glIsEnabledi\n");
}


else if(strcmp(name,"glDeleteFramebuffers")==0)
{
	f=(void*)glDeleteFramebuffers;

	//printf\("get glDeleteFramebuffers\n");
}


else if(strcmp(name,"glUniform3iv")==0)
{
	f=(void*)glUniform3iv;

	//printf\("get glUniform3iv\n");
}


else if(strcmp(name,"glProgramUniform2f")==0)
{
	f=(void*)glProgramUniform2f;

	//printf\("get glProgramUniform2f\n");
}


else if(strcmp(name,"glIsTexture")==0)
{
	f=(void*)glIsTexture;

	//printf\("get glIsTexture\n");
}


else if(strcmp(name,"glCopyBufferSubData")==0)
{
	f=(void*)glCopyBufferSubData;

	//printf\("get glCopyBufferSubData\n");
}


else if(strcmp(name,"glUseProgramStages")==0)
{
	f=(void*)glUseProgramStages;

	//printf\("get glUseProgramStages\n");
}


else if(strcmp(name,"glProgramUniform2fv")==0)
{
	f=(void*)glProgramUniform2fv;

	//printf\("get glProgramUniform2fv\n");
}


else if(strcmp(name,"glIsFramebuffer")==0)
{
	f=(void*)glIsFramebuffer;

	//printf\("get glIsFramebuffer\n");
}


else if(strcmp(name,"glSamplerParameterfv")==0)
{
	f=(void*)glSamplerParameterfv;

	//printf\("get glSamplerParameterfv\n");
}


else if(strcmp(name,"glIsRenderbuffer")==0)
{
	f=(void*)glIsRenderbuffer;

	//printf\("get glIsRenderbuffer\n");
}


else if(strcmp(name,"glBufferSubData")==0)
{
	f=(void*)glBufferSubData;

	//printf\("get glBufferSubData\n");
}


else if(strcmp(name,"glDeleteBuffers")==0)
{
	f=(void*)glDeleteBuffers;

	//printf\("get glDeleteBuffers\n");
}


else if(strcmp(name,"glTexParameterIuiv")==0)
{
	f=(void*)glTexParameterIuiv;

	//printf\("get glTexParameterIuiv\n");
}


else if(strcmp(name,"glGetActiveAttrib")==0)
{
	f=(void*)glGetActiveAttrib;

	//printf\("get glGetActiveAttrib\n");
}


else if(strcmp(name,"glBlendColor")==0)
{
	f=(void*)glBlendColor;

	//printf\("get glBlendColor\n");
}


else if(strcmp(name,"glProgramUniform2iv")==0)
{
	f=(void*)glProgramUniform2iv;

	//printf\("get glProgramUniform2iv\n");
}


else if(strcmp(name,"glTexImage3D")==0)
{
	f=(void*)glTexImage3D;

	//printf\("get glTexImage3D\n");
}


else if(strcmp(name,"glCreateShader")==0)
{
	f=(void*)glCreateShader;

	//printf\("get glCreateShader\n");
}


else if(strcmp(name,"glProgramUniform1f")==0)
{
	f=(void*)glProgramUniform1f;

	//printf\("get glProgramUniform1f\n");
}


else if(strcmp(name,"glSamplerParameteri")==0)
{
	f=(void*)glSamplerParameteri;

	//printf\("get glSamplerParameteri\n");
}


else if(strcmp(name,"glBlendBarrier")==0)
{
	f=(void*)glBlendBarrier;

	//printf\("get glBlendBarrier\n");
}


else if(strcmp(name,"glClear")==0)
{
	f=(void*)glClear;

	//printf\("get glClear\n");
}


else if(strcmp(name,"glGetFragDataLocation")==0)
{
	f=(void*)glGetFragDataLocation;

	//printf\("get glGetFragDataLocation\n");
}


else if(strcmp(name,"glDrawElementsInstanced")==0)
{
	f=(void*)glDrawElementsInstanced;

	//printf\("get glDrawElementsInstanced\n");
}


else if(strcmp(name,"glVertexAttrib3f")==0)
{
	f=(void*)glVertexAttrib3f;

	//printf\("get glVertexAttrib3f\n");
}


else if(strcmp(name,"glPushDebugGroup")==0)
{
	f=(void*)glPushDebugGroup;

	//printf\("get glPushDebugGroup\n");
}


else if(strcmp(name,"glBindBufferBase")==0)
{
	f=(void*)glBindBufferBase;

	//printf\("get glBindBufferBase\n");
}


else if(strcmp(name,"glVertexAttribIPointer")==0)
{
	f=(void*)glVertexAttribIPointer;

	//printf\("get glVertexAttribIPointer\n");
}


else if(strcmp(name,"glUniform3i")==0)
{
	f=(void*)glUniform3i;

	//printf\("get glUniform3i\n");
}


else if(strcmp(name,"glMapBufferRange")==0)
{
	f=(void*)glMapBufferRange;

	//printf\("get glMapBufferRange\n");
}


else if(strcmp(name,"glUniform4i")==0)
{
	f=(void*)glUniform4i;

	//printf\("get glUniform4i\n");
}


else if(strcmp(name,"glDrawBuffers")==0)
{
	f=(void*)glDrawBuffers;

	//printf\("get glDrawBuffers\n");
}


else if(strcmp(name,"glGetObjectPtrLabel")==0)
{
	f=(void*)glGetObjectPtrLabel;

	//printf\("get glGetObjectPtrLabel\n");
}


else if(strcmp(name,"glDrawElementsIndirect")==0)
{
	f=(void*)glDrawElementsIndirect;

	//printf\("get glDrawElementsIndirect\n");
}


else if(strcmp(name,"glUniform2i")==0)
{
	f=(void*)glUniform2i;

	//printf\("get glUniform2i\n");
}


else if(strcmp(name,"glGetShaderPrecisionFormat")==0)
{
	f=(void*)glGetShaderPrecisionFormat;

	//printf\("get glGetShaderPrecisionFormat\n");
}


else if(strcmp(name,"glTexStorage3D")==0)
{
	f=(void*)glTexStorage3D;

	//printf\("get glTexStorage3D\n");
}


else if(strcmp(name,"glProgramUniform1iv")==0)
{
	f=(void*)glProgramUniform1iv;

	//printf\("get glProgramUniform1iv\n");
}


else if(strcmp(name,"glDrawElements")==0)
{
	f=(void*)glDrawElements;

	//printf\("get glDrawElements\n");
}


else if(strcmp(name,"glSampleMaski")==0)
{
	f=(void*)glSampleMaski;

	//printf\("get glSampleMaski\n");
}


else if(strcmp(name,"glGetTexLevelParameteriv")==0)
{
	f=(void*)glGetTexLevelParameteriv;

	//printf\("get glGetTexLevelParameteriv\n");
}


else if(strcmp(name,"glGetStringi")==0)
{
	f=(void*)glGetStringi;

	//printf\("get glGetStringi\n");
}


else if(strcmp(name,"glProgramUniformMatrix4x2fv")==0)
{
	f=(void*)glProgramUniformMatrix4x2fv;

	//printf\("get glProgramUniformMatrix4x2fv\n");
}


else if(strcmp(name,"glTexSubImage2D")==0)
{
	f=(void*)glTexSubImage2D;

	//printf\("get glTexSubImage2D\n");
}


else if(strcmp(name,"glClearBufferfv")==0)
{
	f=(void*)glClearBufferfv;

	//printf\("get glClearBufferfv\n");
}


else if(strcmp(name,"glClientWaitSync")==0)
{
	f=(void*)glClientWaitSync;

	//printf\("get glClientWaitSync\n");
}


else if(strcmp(name,"glGenTransformFeedbacks")==0)
{
	f=(void*)glGenTransformFeedbacks;

	//printf\("get glGenTransformFeedbacks\n");
}


else if(strcmp(name,"glGetProgramiv")==0)
{
	f=(void*)glGetProgramiv;

	//printf\("get glGetProgramiv\n");
}


else if(strcmp(name,"glGetTexParameteriv")==0)
{
	f=(void*)glGetTexParameteriv;

	//printf\("get glGetTexParameteriv\n");
}


else if(strcmp(name,"glProgramUniformMatrix4fv")==0)
{
	f=(void*)glProgramUniformMatrix4fv;

	//printf\("get glProgramUniformMatrix4fv\n");
}


else if(strcmp(name,"glGetError")==0)
{
	f=(void*)glGetError;

	//printf\("get glGetError\n");
}


else if(strcmp(name,"glTexParameteriv")==0)
{
	f=(void*)glTexParameteriv;

	//printf\("get glTexParameteriv\n");
}


else if(strcmp(name,"glEndQuery")==0)
{
	f=(void*)glEndQuery;

	//printf\("get glEndQuery\n");
}


else if(strcmp(name,"glUniform3uiv")==0)
{
	f=(void*)glUniform3uiv;

	//printf\("get glUniform3uiv\n");
}


else if(strcmp(name,"glGetVertexAttribiv")==0)
{
	f=(void*)glGetVertexAttribiv;

	//printf\("get glGetVertexAttribiv\n");
}


else if(strcmp(name,"glUniform1ui")==0)
{
	f=(void*)glUniform1ui;

	//printf\("get glUniform1ui\n");
}


else if(strcmp(name,"glVertexAttrib1f")==0)
{
	f=(void*)glVertexAttrib1f;

	//printf\("get glVertexAttrib1f\n");
}


else if(strcmp(name,"glDrawArrays")==0)
{
	f=(void*)glDrawArrays;

	//printf\("get glDrawArrays\n");
}


else if(strcmp(name,"glGenQueries")==0)
{
	f=(void*)glGenQueries;

	//printf\("get glGenQueries\n");
}


else if(strcmp(name,"glDebugMessageCallback")==0)
{
	f=(void*)glDebugMessageCallback;

	//printf\("get glDebugMessageCallback\n");
}


else if(strcmp(name,"glBindVertexArray")==0)
{
	f=(void*)glBindVertexArray;

	//printf\("get glBindVertexArray\n");
}


else if(strcmp(name,"glProgramBinary")==0)
{
	f=(void*)glProgramBinary;

	//printf\("get glProgramBinary\n");
}


else if(strcmp(name,"glBindTexture")==0)
{
	f=(void*)glBindTexture;

	//printf\("get glBindTexture\n");
}


else if(strcmp(name,"glStencilMask")==0)
{
	f=(void*)glStencilMask;

	//printf\("get glStencilMask\n");
}


else if(strcmp(name,"glBlendFunc")==0)
{
	f=(void*)glBlendFunc;

	//printf\("get glBlendFunc\n");
}


else if(strcmp(name,"glClearBufferfi")==0)
{
	f=(void*)glClearBufferfi;

	//printf\("get glClearBufferfi\n");
}


else if(strcmp(name,"glDetachShader")==0)
{
	f=(void*)glDetachShader;

	//printf\("get glDetachShader\n");
}


else if(strcmp(name,"glDebugMessageControl")==0)
{
	f=(void*)glDebugMessageControl;

	//printf\("get glDebugMessageControl\n");
}


else if(strcmp(name,"glGetShaderiv")==0)
{
	f=(void*)glGetShaderiv;

	//printf\("get glGetShaderiv\n");
}


else if(strcmp(name,"glGetUniformuiv")==0)
{
	f=(void*)glGetUniformuiv;

	//printf\("get glGetUniformuiv\n");
}


else if(strcmp(name,"glGetProgramResourceLocation")==0)
{
	f=(void*)glGetProgramResourceLocation;

	//printf\("get glGetProgramResourceLocation\n");
}


else if(strcmp(name,"glInvalidateSubFramebuffer")==0)
{
	f=(void*)glInvalidateSubFramebuffer;

	//printf\("get glInvalidateSubFramebuffer\n");
}


else if(strcmp(name,"glBindAttribLocation")==0)
{
	f=(void*)glBindAttribLocation;

	//printf\("get glBindAttribLocation\n");
}


else if(strcmp(name,"glCopyImageSubData")==0)
{
	f=(void*)glCopyImageSubData;

	//printf\("get glCopyImageSubData\n");
}


else if(strcmp(name,"glViewport")==0)
{
	f=(void*)glViewport;

	//printf\("get glViewport\n");
}


else if(strcmp(name,"glGenFramebuffers")==0)
{
	f=(void*)glGenFramebuffers;

	//printf\("get glGenFramebuffers\n");
}


else if(strcmp(name,"glGetShaderSource")==0)
{
	f=(void*)glGetShaderSource;

	//printf\("get glGetShaderSource\n");
}


else if(strcmp(name,"glUniform1uiv")==0)
{
	f=(void*)glUniform1uiv;

	//printf\("get glUniform1uiv\n");
}


else if(strcmp(name,"glPolygonOffset")==0)
{
	f=(void*)glPolygonOffset;

	//printf\("get glPolygonOffset\n");
}


else if(strcmp(name,"glFinish")==0)
{
	f=(void*)glFinish;

	//printf\("get glFinish\n");
}


else if(strcmp(name,"glGetQueryObjectuiv")==0)
{
	f=(void*)glGetQueryObjectuiv;

	//printf\("get glGetQueryObjectuiv\n");
}


else if(strcmp(name,"glUniformMatrix4x3fv")==0)
{
	f=(void*)glUniformMatrix4x3fv;

	//printf\("get glUniformMatrix4x3fv\n");
}


else if(strcmp(name,"glVertexAttrib4f")==0)
{
	f=(void*)glVertexAttrib4f;

	//printf\("get glVertexAttrib4f\n");
}


else if(strcmp(name,"glDeleteProgram")==0)
{
	f=(void*)glDeleteProgram;

	//printf\("get glDeleteProgram\n");
}


else if(strcmp(name,"glDeleteQueries")==0)
{
	f=(void*)glDeleteQueries;

	//printf\("get glDeleteQueries\n");
}


else if(strcmp(name,"glDisableVertexAttribArray")==0)
{
	f=(void*)glDisableVertexAttribArray;

	//printf\("get glDisableVertexAttribArray\n");
}


else if(strcmp(name,"glIsBuffer")==0)
{
	f=(void*)glIsBuffer;

	//printf\("get glIsBuffer\n");
}


else if(strcmp(name,"glVertexAttribDivisor")==0)
{
	f=(void*)glVertexAttribDivisor;

	//printf\("get glVertexAttribDivisor\n");
}


else if(strcmp(name,"glInvalidateFramebuffer")==0)
{
	f=(void*)glInvalidateFramebuffer;

	//printf\("get glInvalidateFramebuffer\n");
}


else if(strcmp(name,"glTexParameterfv")==0)
{
	f=(void*)glTexParameterfv;

	//printf\("get glTexParameterfv\n");
}


else if(strcmp(name,"glObjectPtrLabel")==0)
{
	f=(void*)glObjectPtrLabel;

	//printf\("get glObjectPtrLabel\n");
}


else if(strcmp(name,"glTexParameteri")==0)
{
	f=(void*)glTexParameteri;

	//printf\("get glTexParameteri\n");
}


else if(strcmp(name,"glFramebufferTexture")==0)
{
	f=(void*)glFramebufferTexture;

	//printf\("get glFramebufferTexture\n");
}


else if(strcmp(name,"glUniformMatrix4fv")==0)
{
	f=(void*)glUniformMatrix4fv;

	//printf\("get glUniformMatrix4fv\n");
}


else if(strcmp(name,"glGetInternalformativ")==0)
{
	f=(void*)glGetInternalformativ;

	//printf\("get glGetInternalformativ\n");
}


else if(strcmp(name,"glCreateShaderProgramv")==0)
{
	f=(void*)glCreateShaderProgramv;

	//printf\("get glCreateShaderProgramv\n");
}


else if(strcmp(name,"glGetFloatv")==0)
{
	f=(void*)glGetFloatv;

	//printf\("get glGetFloatv\n");
}


else if(strcmp(name,"glBindVertexBuffer")==0)
{
	f=(void*)glBindVertexBuffer;

	//printf\("get glBindVertexBuffer\n");
}


else if(strcmp(name,"glIsProgram")==0)
{
	f=(void*)glIsProgram;

	//printf\("get glIsProgram\n");
}


else if(strcmp(name,"glSampleCoverage")==0)
{
	f=(void*)glSampleCoverage;

	//printf\("get glSampleCoverage\n");
}


else if(strcmp(name,"glGetVertexAttribIuiv")==0)
{
	f=(void*)glGetVertexAttribIuiv;

	//printf\("get glGetVertexAttribIuiv\n");
}


else if(strcmp(name,"glLineWidth")==0)
{
	f=(void*)glLineWidth;

	//printf\("get glLineWidth\n");
}


else if(strcmp(name,"glGenSamplers")==0)
{
	f=(void*)glGenSamplers;

	//printf\("get glGenSamplers\n");
}


else if(strcmp(name,"glUniform4f")==0)
{
	f=(void*)glUniform4f;

	//printf\("get glUniform4f\n");
}


else if(strcmp(name,"glStencilFuncSeparate")==0)
{
	f=(void*)glStencilFuncSeparate;

	//printf\("get glStencilFuncSeparate\n");
}


else if(strcmp(name,"glDispatchComputeIndirect")==0)
{
	f=(void*)glDispatchComputeIndirect;

	//printf\("get glDispatchComputeIndirect\n");
}


else if(strcmp(name,"glCopyTexImage2D")==0)
{
	f=(void*)glCopyTexImage2D;

	//printf\("get glCopyTexImage2D\n");
}


else if(strcmp(name,"glDeleteSamplers")==0)
{
	f=(void*)glDeleteSamplers;

	//printf\("get glDeleteSamplers\n");
}


else if(strcmp(name,"glDeleteProgramPipelines")==0)
{
	f=(void*)glDeleteProgramPipelines;

	//printf\("get glDeleteProgramPipelines\n");
}


else if(strcmp(name,"glCompileShader")==0)
{
	f=(void*)glCompileShader;

	//printf\("get glCompileShader\n");
}


else if(strcmp(name,"glGetVertexAttribfv")==0)
{
	f=(void*)glGetVertexAttribfv;

	//printf\("get glGetVertexAttribfv\n");
}


else if(strcmp(name,"glProgramUniform3f")==0)
{
	f=(void*)glProgramUniform3f;

	//printf\("get glProgramUniform3f\n");
}


else if(strcmp(name,"glValidateProgramPipeline")==0)
{
	f=(void*)glValidateProgramPipeline;

	//printf\("get glValidateProgramPipeline\n");
}


else if(strcmp(name,"glFramebufferRenderbuffer")==0)
{
	f=(void*)glFramebufferRenderbuffer;

	//printf\("get glFramebufferRenderbuffer\n");
}


else if(strcmp(name,"glScissor")==0)
{
	f=(void*)glScissor;

	//printf\("get glScissor\n");
}


else if(strcmp(name,"glVertexAttribI4i")==0)
{
	f=(void*)glVertexAttribI4i;

	//printf\("get glVertexAttribI4i\n");
}


else if(strcmp(name,"glHint")==0)
{
	f=(void*)glHint;

	//printf\("get glHint\n");
}


else if(strcmp(name,"glGetTransformFeedbackVarying")==0)
{
	f=(void*)glGetTransformFeedbackVarying;

	//printf\("get glGetTransformFeedbackVarying\n");
}


else if(strcmp(name,"glTexBufferRange")==0)
{
	f=(void*)glTexBufferRange;

	//printf\("get glTexBufferRange\n");
}


else if(strcmp(name,"glVertexAttribI4iv")==0)
{
	f=(void*)glVertexAttribI4iv;

	//printf\("get glVertexAttribI4iv\n");
}


else if(strcmp(name,"glGenVertexArrays")==0)
{
	f=(void*)glGenVertexArrays;

	//printf\("get glGenVertexArrays\n");
}


else if(strcmp(name,"glBlendEquationSeparatei")==0)
{
	f=(void*)glBlendEquationSeparatei;

	//printf\("get glBlendEquationSeparatei\n");
}


else if(strcmp(name,"glProgramParameteri")==0)
{
	f=(void*)glProgramParameteri;

	//printf\("get glProgramParameteri\n");
}



    return f;
}

