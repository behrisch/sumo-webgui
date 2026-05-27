#include "DetectorLayer.h"

#include <cstdint>
#include <vector>

#include "gl/Shader.h"
#include "sim/NetworkGeometry.h"

namespace {
constexpr const char* kVS = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_col;
uniform mat4 u_proj;
out vec4 v_col;
void main() {
    v_col = a_col;
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
}
)";
constexpr const char* kFS = R"(#version 330 core
in vec4 v_col;
out vec4 frag;
void main() { frag = v_col; }
)";

struct Vert { float x, y; std::uint8_t r, g, b, a; };

void pushQuad(std::vector<Vert>& v,
              float ax, float ay, float bx, float by,
              float cx, float cy, float dx, float dy,
              std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    const Vert A{ax, ay, r, g, b, a};
    const Vert B{bx, by, r, g, b, a};
    const Vert C{cx, cy, r, g, b, a};
    const Vert D{dx, dy, r, g, b, a};
    v.insert(v.end(), {A, B, C, A, C, D});
}
}  // namespace

DetectorLayer::DetectorLayer() = default;
DetectorLayer::~DetectorLayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void DetectorLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj = m_gl->glGetUniformLocation(m_program, "u_proj");
    m_vao.create();
    m_vbo.create();
    if (m_dirty) rebuild();
}

void DetectorLayer::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    m_dirty = true;
    if (m_gl && m_program) rebuild();
}

void DetectorLayer::rebuild() {
    m_dirty = false;
    m_vertexCount = 0;
    if (!m_ng) return;
    std::vector<Vert> verts;
    const std::size_t n = m_ng->det_count();
    verts.reserve(n * 6);
    constexpr float kPointBar = 0.5f;   // half-length along lane for loops
    constexpr float kBandHalfWidth = 1.6f;  // perpendicular half-width
    for (std::size_t i = 0; i < n; ++i) {
        const float cx = m_ng->det_x[i];
        const float cy = m_ng->det_y[i];
        const float dx = m_ng->det_dx[i];
        const float dy = m_ng->det_dy[i];
        const float len = m_ng->det_len[i];
        const float px = -dy, py = dx;
        std::uint8_t r, g, b, a;
        float halfL, halfW;
        if (m_ng->det_kind[i] == 0) {
            // Induction loop: bright magenta short bar.
            r = 240; g = 60; b = 200; a = 230;
            halfL = kPointBar;
            halfW = kBandHalfWidth;
        } else {
            // Lane-area: translucent magenta band along the lane.
            r = 240; g = 60; b = 200; a = 150;
            halfL = 0.5f * std::max(0.5f, len);
            halfW = kBandHalfWidth * 0.6f;
        }
        const float hx = dx * halfL, hy = dy * halfL;
        const float kx = px * halfW, ky = py * halfW;
        pushQuad(verts,
                 cx - hx - kx, cy - hy - ky,
                 cx + hx - kx, cy + hy - ky,
                 cx + hx + kx, cy + hy + ky,
                 cx - hx + kx, cy - hy + ky,
                 r, g, b, a);
    }
    m_vertexCount = static_cast<GLsizei>(verts.size());
    m_vao.bind();
    m_vbo.bind();
    m_vbo.allocate(verts.data(), static_cast<int>(verts.size() * sizeof(Vert)));
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vert),
                                reinterpret_cast<void*>(offsetof(Vert, x)));
    m_gl->glEnableVertexAttribArray(1);
    m_gl->glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vert),
                                reinterpret_cast<void*>(offsetof(Vert, r)));
    m_vbo.release();
    m_vao.release();
}

void DetectorLayer::draw(const float* proj) {
    if (!m_program || !m_gl || m_vertexCount == 0) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, proj);
    m_vao.bind();
    m_gl->glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);
    m_vao.release();
    m_gl->glUseProgram(0);
}
