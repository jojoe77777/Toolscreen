#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>
#include <windows.h>

#include "common/utils.h"

namespace gloverlay {

constexpr int kFloatsPerVertex = 8;
constexpr int kVerticesPerQuad = 6;

inline constexpr const char* kQuadVertShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
})";

inline constexpr const char* kQuadFragShader = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uTex;
void main() {
    FragColor = texture(uTex, vUV) * vColor;
})";

class ScopedState {
public:
    ScopedState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer_);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer_);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture_);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2D_);
        blend_ = glIsEnabled(GL_BLEND);
        depth_ = glIsEnabled(GL_DEPTH_TEST);
        cull_ = glIsEnabled(GL_CULL_FACE);
        scissor_ = glIsEnabled(GL_SCISSOR_TEST);
        stencil_ = glIsEnabled(GL_STENCIL_TEST);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb_);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb_);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha_);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha_);

        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    ~ScopedState() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer_));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer_));
        glBindVertexArray(static_cast<GLuint>(vao_));
        glActiveTexture(GL_TEXTURE0);
        BindTextureDirect(GL_TEXTURE_2D, static_cast<GLuint>(texture2D_));
        glActiveTexture(static_cast<GLenum>(activeTexture_));
        glUseProgram(static_cast<GLuint>(program_));
        if (blend_) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (depth_) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (cull_) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        if (scissor_) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (stencil_) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        glBlendFuncSeparate(blendSrcRgb_, blendDstRgb_, blendSrcAlpha_, blendDstAlpha_);
    }

    ScopedState(const ScopedState&) = delete;
    ScopedState& operator=(const ScopedState&) = delete;

private:
    GLint program_ = 0, vao_ = 0, arrayBuffer_ = 0, drawFramebuffer_ = 0;
    GLint activeTexture_ = GL_TEXTURE0, texture2D_ = 0;
    GLint blendSrcRgb_ = GL_SRC_ALPHA, blendDstRgb_ = GL_ONE_MINUS_SRC_ALPHA;
    GLint blendSrcAlpha_ = GL_SRC_ALPHA, blendDstAlpha_ = GL_ONE_MINUS_SRC_ALPHA;
    GLboolean blend_ = GL_FALSE, depth_ = GL_FALSE, cull_ = GL_FALSE, scissor_ = GL_FALSE, stencil_ = GL_FALSE;
};

class ScopedUnpackState {
public:
    explicit ScopedUnpackState(GLint alignment) {
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength_);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels_);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows_);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    }

    ~ScopedUnpackState() {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength_);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels_);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment_);
    }

    ScopedUnpackState(const ScopedUnpackState&) = delete;
    ScopedUnpackState& operator=(const ScopedUnpackState&) = delete;

private:
    GLint rowLength_ = 0, skipPixels_ = 0, skipRows_ = 0, alignment_ = 4;
};

class QuadBatch {
public:
    bool Ensure() {
        const DWORD tid = GetCurrentThreadId();
        if (threadId_ != tid) {
            program_ = 0;
            vao_ = 0;
            vbo_ = 0;
            ready_ = false;
            threadId_ = tid;
        }
        if (ready_) { return true; }

        if (program_ == 0) { program_ = CreateShaderProgram(kQuadVertShader, kQuadFragShader); }
        if (vao_ == 0) { glGenVertexArrays(1, &vao_); }
        if (vbo_ == 0) { glGenBuffers(1, &vbo_); }
        if (program_ == 0 || vao_ == 0 || vbo_ == 0) {
            if (program_ != 0) { glDeleteProgram(program_); program_ = 0; }
            if (vao_ != 0) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
            if (vbo_ != 0) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
            return false;
        }
        texUniform_ = glGetUniformLocation(program_, "uTex");

        GLint prevVao = 0, prevArrayBuffer = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        const GLsizei stride = kFloatsPerVertex * sizeof(float);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glBindVertexArray(static_cast<GLuint>(prevVao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArrayBuffer));

        ready_ = true;
        return true;
    }

    void Draw(const float* interleaved, GLsizei vertexCount) {
        glUseProgram(program_);
        glUniform1i(texUniform_, 0);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(vertexCount) * kFloatsPerVertex * sizeof(float),
                     interleaved, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, vertexCount);
    }

private:
    GLuint program_ = 0, vao_ = 0, vbo_ = 0;
    GLint texUniform_ = -1;
    DWORD threadId_ = 0;
    bool ready_ = false;
};

}
