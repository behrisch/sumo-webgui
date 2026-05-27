#include "PedAreaLayer.h"

#include <cmath>
#include <cstdint>
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
uniform vec4 u_color;
out vec4 frag;
void main() { frag = u_color; }
)";

// Extrude one lane into independent triangles (6 vertices per segment) so
// each lane is self-contained — no degenerate bridges needed.
void extrudeIndependent(const float* pts, std::size_t nPts, float width,
                        std::vector<float>& out) {
    if (nPts < 2 || width <= 0.0f) return;
    const float halfW = 0.5f * width;
    for (std::size_t i = 0; i + 1 < nPts; ++i) {
        const float x0 = pts[i * 2];
        const float y0 = pts[i * 2 + 1];
        const float x1 = pts[i * 2 + 2];
        const float y1 = pts[i * 2 + 3];
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float L = std::sqrt(dx * dx + dy * dy);
        if (L < 1e-6f) continue;
        const float nx = -dy / L * halfW;
        const float ny =  dx / L * halfW;
        const float ax = x0 + nx, ay = y0 + ny;
        const float bx = x0 - nx, by = y0 - ny;
        const float cx = x1 + nx, cy = y1 + ny;
        const float ddx = x1 - nx, ddy = y1 - ny;
        out.insert(out.end(), {ax, ay, bx, by, cx, cy,
                               bx, by, ddx, ddy, cx, cy});
    }
}

void setupBuffer(QOpenGLFunctions_3_3_Core* gl,
                 PedAreaLayer::Group& g,
                 const std::vector<float>& verts) {
    g.vertexCount = static_cast<GLsizei>(verts.size() / 2);
    if (g.vertexCount == 0) return;
    g.vao.bind();
    g.vbo.bind();
    g.vbo.allocate(verts.data(), static_cast<int>(verts.size() * sizeof(float)));
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    g.vbo.release();
    g.vao.release();
}

}  // namespace

PedAreaLayer::PedAreaLayer() {
    // Sidewalk: warm tan.
    m_sidewalk.color[0] = 0.55f;
    m_sidewalk.color[1] = 0.50f;
    m_sidewalk.color[2] = 0.40f;
    m_sidewalk.color[3] = 1.0f;
    // Walking area / crossing: light grey.
    m_walkArea.color[0] = 0.65f;
    m_walkArea.color[1] = 0.65f;
    m_walkArea.color[2] = 0.62f;
    m_walkArea.color[3] = 1.0f;
}

PedAreaLayer::~PedAreaLayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void PedAreaLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj  = m_gl->glGetUniformLocation(m_program, "u_proj");
    m_locColor = m_gl->glGetUniformLocation(m_program, "u_color");
    m_sidewalk.vao.create(); m_sidewalk.vbo.create();
    m_walkArea.vao.create(); m_walkArea.vbo.create();
    if (m_dirty) rebuild();
}

void PedAreaLayer::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    m_dirty = true;
    if (m_gl && m_program) rebuild();
}

void PedAreaLayer::rebuild() {
    m_dirty = false;
    std::vector<float> sw, wa;
    if (!m_ng) {
        setupBuffer(m_gl, m_sidewalk, sw);
        setupBuffer(m_gl, m_walkArea, wa);
        return;
    }
    sw.reserve(m_ng->lane_points.size());
    wa.reserve(m_ng->lane_points.size());
    for (std::size_t i = 0; i < m_ng->lane_count(); ++i) {
        const auto kind = m_ng->lane_kind[i];
        if (kind != 2 && kind != 3) continue;
        const std::uint32_t s = m_ng->lane_offsets[i];
        const std::uint32_t e = m_ng->lane_offsets[i + 1];
        if (e - s < 2) continue;
        auto& out = (kind == 2) ? sw : wa;
        extrudeIndependent(&m_ng->lane_points[s * 2], e - s,
                           m_ng->lane_widths[i], out);
    }
    setupBuffer(m_gl, m_sidewalk, sw);
    setupBuffer(m_gl, m_walkArea, wa);
}

void PedAreaLayer::draw(const float* projectionMatColMajor) {
    if (!m_program) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, projectionMatColMajor);
    auto drawGroup = [&](Group& g) {
        if (g.vertexCount == 0) return;
        m_gl->glUniform4fv(m_locColor, 1, g.color);
        g.vao.bind();
        m_gl->glDrawArrays(GL_TRIANGLES, 0, g.vertexCount);
        g.vao.release();
    };
    // Walking areas first (broader, lower) then sidewalks on top.
    drawGroup(m_walkArea);
    drawGroup(m_sidewalk);
    m_gl->glUseProgram(0);
}
