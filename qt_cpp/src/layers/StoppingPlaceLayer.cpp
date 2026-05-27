#include "StoppingPlaceLayer.h"

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

StoppingPlaceLayer::StoppingPlaceLayer() = default;
StoppingPlaceLayer::~StoppingPlaceLayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void StoppingPlaceLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj = m_gl->glGetUniformLocation(m_program, "u_proj");
    m_vao.create();
    m_vbo.create();
    if (m_dirty) rebuild();
}

void StoppingPlaceLayer::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    m_dirty = true;
    if (m_gl && m_program) rebuild();
}

void StoppingPlaceLayer::rebuild() {
    m_dirty = false;
    m_vertexCount = 0;
    if (!m_ng) return;
    std::vector<Vert> verts;
    const std::size_t n = m_ng->stop_count();
    verts.reserve(n * 6);
    for (std::size_t i = 0; i < n; ++i) {
        const float cx = m_ng->stop_x[i];
        const float cy = m_ng->stop_y[i];
        const float dx = m_ng->stop_dx[i];
        const float dy = m_ng->stop_dy[i];
        const float len = m_ng->stop_len[i];
        const float w   = m_ng->stop_w[i];
        const float px = -dy, py = dx;        // perpendicular (left normal)
        // Offset band sits to the right of the lane (negative perpendicular),
        // half-lane-width away from the centerline, ~lane-width tall.
        const float bandW = std::max(1.0f, 0.6f * w);
        const float offset = 0.5f * w + 0.5f * bandW;
        const float ox = -px * offset, oy = -py * offset;
        const float hx = 0.5f * dx * len, hy = 0.5f * dy * len;
        const float kx = -px * 0.5f * bandW, ky = -py * 0.5f * bandW;
        std::uint8_t r = 0, g = 0, b = 0, a = 220;
        switch (m_ng->stop_kind[i]) {
            case 0: r = 40;  g = 120; b = 220; break;  // bus (blue)
            case 1: r = 80;  g = 220; b = 220; break;  // charging (cyan)
            case 2: r = 230; g = 150; b = 30;  break;  // parking (orange)
        }
        pushQuad(verts,
                 cx + ox - hx - kx, cy + oy - hy - ky,
                 cx + ox + hx - kx, cy + oy + hy - ky,
                 cx + ox + hx + kx, cy + oy + hy + ky,
                 cx + ox - hx + kx, cy + oy - hy + ky,
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

void StoppingPlaceLayer::draw(const float* proj) {
    if (!m_program || !m_gl || m_vertexCount == 0) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, proj);
    m_vao.bind();
    m_gl->glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);
    m_vao.release();
    m_gl->glUseProgram(0);
}
