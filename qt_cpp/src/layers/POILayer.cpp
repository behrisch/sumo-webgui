#include "POILayer.h"

#include <array>
#include <cstdint>
#include <vector>

#include "gl/Shader.h"
#include "sim/NetworkGeometry.h"

namespace {

constexpr float kHalf = 1.8f;  // half side of a POI disc (m)
constexpr std::array<float, 12> kQuad = {
    -kHalf, -kHalf,  kHalf, -kHalf,  kHalf,  kHalf,
    -kHalf, -kHalf,  kHalf,  kHalf, -kHalf,  kHalf,
};

constexpr const char* kVS = R"(#version 330 core
layout(location=0) in vec2 a_local;
layout(location=1) in vec2 a_pos;
layout(location=2) in vec4 a_color;
uniform mat4 u_proj;
uniform float u_half;
out vec4 v_color;
out vec2 v_local;
void main() {
    gl_Position = u_proj * vec4(a_pos + a_local, 0.0, 1.0);
    v_color = a_color;
    v_local = a_local / u_half;
}
)";

constexpr const char* kFS = R"(#version 330 core
in vec4 v_color;
in vec2 v_local;
out vec4 frag;
void main() {
    float r = length(v_local);
    if (r > 1.0) discard;
    // Slight darkening near edge to give a 3D feel + dark outline.
    float edge = smoothstep(1.0, 0.9, r);
    float rim  = smoothstep(0.95, 0.82, r);
    vec3 c = mix(vec3(0.0), v_color.rgb, rim);
    frag = vec4(c, v_color.a * edge);
}
)";

}  // namespace

POILayer::POILayer() = default;
POILayer::~POILayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void POILayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj = m_gl->glGetUniformLocation(m_program, "u_proj");
    const int locHalf = m_gl->glGetUniformLocation(m_program, "u_half");
    m_gl->glUseProgram(m_program);
    m_gl->glUniform1f(locHalf, kHalf);
    m_gl->glUseProgram(0);

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
    m_gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_gl->glVertexAttribDivisor(1, 1);

    m_colVbo.bind();
    m_gl->glEnableVertexAttribArray(2);
    m_gl->glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4, nullptr);
    m_gl->glVertexAttribDivisor(2, 1);
    m_vao.release();

    if (m_dirty) upload();
}

void POILayer::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    m_dirty = true;
    if (m_gl && m_program) upload();
}

void POILayer::upload() {
    m_dirty = false;
    if (!m_ng || m_ng->poi_count() == 0) {
        m_instanceCount = 0;
        return;
    }
    const std::size_t n = m_ng->poi_count();
    std::vector<float> pos(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        pos[i * 2 + 0] = m_ng->poi_x[i];
        pos[i * 2 + 1] = m_ng->poi_y[i];
    }
    m_posVbo.bind();
    m_posVbo.allocate(pos.data(), static_cast<int>(pos.size() * sizeof(float)));
    m_posVbo.release();
    m_colVbo.bind();
    m_colVbo.allocate(m_ng->poi_rgba.data(),
                      static_cast<int>(m_ng->poi_rgba.size() * sizeof(std::uint8_t)));
    m_colVbo.release();
    m_instanceCount = static_cast<GLsizei>(n);
}

void POILayer::draw(const float* projectionMatColMajor) {
    if (!m_program || m_instanceCount == 0) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, projectionMatColMajor);
    m_vao.bind();
    m_gl->glDrawArraysInstanced(GL_TRIANGLES, 0, 6, m_instanceCount);
    m_vao.release();
    m_gl->glUseProgram(0);
}
