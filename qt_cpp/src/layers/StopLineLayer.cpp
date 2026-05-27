#include "StopLineLayer.h"

#include <vector>

#include "gl/Shader.h"
#include "sim/NetworkGeometry.h"

namespace {
constexpr const char* kVS = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
uniform mat4 u_proj;
void main() { gl_Position = u_proj * vec4(a_pos, 0.0, 1.0); }
)";
constexpr const char* kFS = R"(#version 330 core
out vec4 frag;
uniform vec4 u_color;
void main() { frag = u_color; }
)";
}  // namespace

StopLineLayer::StopLineLayer() = default;
StopLineLayer::~StopLineLayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void StopLineLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj  = m_gl->glGetUniformLocation(m_program, "u_proj");
    m_locColor = m_gl->glGetUniformLocation(m_program, "u_color");
    m_vao.create();
    m_vbo.create();
    if (m_dirty) rebuild();
}

void StopLineLayer::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    m_dirty = true;
    if (m_gl && m_program) rebuild();
}

void StopLineLayer::rebuild() {
    m_dirty = false;
    m_vertexCount = 0;
    if (!m_ng) return;

    // Each stop line: short rectangle 0.5m thick along lane direction,
    // (lane_width) wide perpendicular. 6 vertices per quad (2 triangles).
    constexpr float kThickness = 0.5f;
    const std::size_t n = m_ng->stopline_count();
    std::vector<float> verts;
    verts.reserve(n * 12);

    for (std::size_t i = 0; i < n; ++i) {
        const float cx = m_ng->stopline_x[i];
        const float cy = m_ng->stopline_y[i];
        const float dx = m_ng->stopline_dx[i];
        const float dy = m_ng->stopline_dy[i];
        const float w  = m_ng->stopline_w[i];
        // Perpendicular = rotate (dx,dy) by +90 deg: (-dy, dx).
        const float px = -dy, py = dx;
        const float hw = 0.5f * w;
        const float ht = 0.5f * kThickness;
        const float ax = cx - dx * ht - px * hw;
        const float ay = cy - dy * ht - py * hw;
        const float bx = cx + dx * ht - px * hw;
        const float by = cy + dy * ht - py * hw;
        const float cx2 = cx + dx * ht + px * hw;
        const float cy2 = cy + dy * ht + py * hw;
        const float dxv = cx - dx * ht + px * hw;
        const float dyv = cy - dy * ht + py * hw;
        verts.insert(verts.end(), {ax, ay, bx, by, cx2, cy2,
                                   ax, ay, cx2, cy2, dxv, dyv});
    }
    m_vertexCount = static_cast<GLsizei>(verts.size() / 2);

    m_vao.bind();
    m_vbo.bind();
    m_vbo.allocate(verts.data(), static_cast<int>(verts.size() * sizeof(float)));
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_vbo.release();
    m_vao.release();
}

void StopLineLayer::draw(const float* proj) {
    if (!m_program || !m_gl || m_vertexCount == 0) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, proj);
    m_gl->glUniform4f(m_locColor, 0.95f, 0.95f, 0.95f, 0.85f);
    m_vao.bind();
    m_gl->glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);
    m_vao.release();
    m_gl->glUseProgram(0);
}
