#include "PersonLayer.h"

#include <array>

#include "gl/Shader.h"

namespace {

constexpr float kHalf = 0.6f;  // person square side / 2 in meters
constexpr std::array<float, 12> kQuad = {
    -kHalf, -kHalf,  kHalf, -kHalf,  kHalf,  kHalf,
    -kHalf, -kHalf,  kHalf,  kHalf, -kHalf,  kHalf,
};

constexpr const char* kVS = R"(#version 330 core
layout(location=0) in vec2  a_local;
layout(location=1) in vec2 a_pos;
layout(location=2) in vec4 a_color;
uniform mat4 u_proj;
out vec4 v_color;
void main() {
    gl_Position = u_proj * vec4(a_pos + a_local, 0.0, 1.0);
    v_color = a_color;
}
)";

constexpr const char* kFS = R"(#version 330 core
in vec4 v_color;
out vec4 frag;
void main() { frag = v_color; }
)";

}  // namespace

PersonLayer::PersonLayer() = default;
PersonLayer::~PersonLayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void PersonLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj = m_gl->glGetUniformLocation(m_program, "u_proj");

    m_vao.create();
    m_quadVbo.create();
    m_posVbo.create();
    m_colVbo.create();

    m_vao.bind();
    m_quadVbo.bind();
    m_quadVbo.allocate(kQuad.data(), static_cast<int>(kQuad.size() * sizeof(float)));
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    m_posVbo.bind();
    m_gl->glEnableVertexAttribArray(1);
    // f64 stride 24 (x,y,z packed); driver narrows to float for `vec2`.
    m_gl->glVertexAttribPointer(1, 2, GL_DOUBLE, GL_FALSE,
                                3 * sizeof(double), nullptr);
    m_gl->glVertexAttribDivisor(1, 1);

    m_colVbo.bind();
    m_gl->glEnableVertexAttribArray(2);
    m_gl->glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4, nullptr);
    m_gl->glVertexAttribDivisor(2, 1);
    m_vao.release();
}

void PersonLayer::setSnapshot(const SimSnapshotPtr& snap) {
    if (!m_gl || !m_program) return;
    if (!snap || snap->person_count() == 0) {
        m_instanceCount = 0;
        return;
    }
    const std::size_t n = snap->person_count();
    // Upload the packed f64 positions directly (stride 24, vec2 read).
    m_posVbo.bind();
    m_posVbo.allocate(snap->agent_positions.data(),
                      static_cast<int>(snap->agent_positions.size()));
    m_colVbo.bind();
    m_colVbo.allocate(snap->person_rgba.data(),
                      static_cast<int>(snap->person_rgba.size()));
    m_colVbo.release();
    m_instanceCount = static_cast<GLsizei>(n);
}

void PersonLayer::draw(const float* projectionMatColMajor) {
    if (!m_program || m_instanceCount == 0) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, projectionMatColMajor);
    m_vao.bind();
    m_gl->glDrawArraysInstanced(GL_TRIANGLES, 0, 6, m_instanceCount);
    m_vao.release();
    m_gl->glUseProgram(0);
}
