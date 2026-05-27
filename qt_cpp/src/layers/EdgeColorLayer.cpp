#include "EdgeColorLayer.h"

#include <cmath>
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
void main() {
    if (v_col.a < 0.01) discard;
    frag = v_col;
}
)";

void extrudePolyline(const float* pts, std::size_t nPts, float width,
                     std::vector<float>& outVerts) {
    if (nPts < 2 || width <= 0.0f) return;
    const float halfW = 0.5f * width;
    auto segN = [](float x0, float y0, float x1, float y1,
                   float& nx, float& ny) {
        const float dx = x1 - x0, dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) { nx = 0; ny = 0; return; }
        nx = -dy / len; ny = dx / len;
    };
    auto push = [&](float x, float y, float nx, float ny) {
        outVerts.push_back(x + nx * halfW); outVerts.push_back(y + ny * halfW);
        outVerts.push_back(x - nx * halfW); outVerts.push_back(y - ny * halfW);
    };
    float pnx, pny;
    segN(pts[0], pts[1], pts[2], pts[3], pnx, pny);
    push(pts[0], pts[1], pnx, pny);
    for (std::size_t i = 1; i + 1 < nPts; ++i) {
        const std::size_t k = i * 2;
        float n1x, n1y, n2x, n2y;
        segN(pts[k - 2], pts[k - 1], pts[k], pts[k + 1], n1x, n1y);
        segN(pts[k],     pts[k + 1], pts[k + 2], pts[k + 3], n2x, n2y);
        float ax = n1x + n2x, ay = n1y + n2y;
        const float al = std::sqrt(ax * ax + ay * ay);
        if (al < 1e-4f) { ax = n2x; ay = n2y; } else { ax /= al; ay /= al; }
        push(pts[k], pts[k + 1], ax, ay);
        pnx = n2x; pny = n2y;
    }
    const std::size_t lk = (nPts - 1) * 2;
    push(pts[lk], pts[lk + 1], pnx, pny);
}
}  // namespace

EdgeColorLayer::EdgeColorLayer() = default;
EdgeColorLayer::~EdgeColorLayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void EdgeColorLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj = m_gl->glGetUniformLocation(m_program, "u_proj");
    m_vao.create(); m_posVbo.create(); m_colVbo.create();
    if (m_dirty) rebuildGeometry();
}

void EdgeColorLayer::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    m_dirty = true;
    m_hasColors = false;
    if (m_gl && m_program) rebuildGeometry();
}

void EdgeColorLayer::rebuildGeometry() {
    m_dirty = false;
    m_firsts.clear();
    m_counts.clear();
    m_laneFirst.clear();
    m_laneCount.clear();
    m_totalVerts = 0;
    if (!m_ng) return;

    std::vector<float> verts;
    verts.reserve(m_ng->lane_points.size() * 4);
    m_laneFirst.reserve(m_ng->lane_count());
    m_laneCount.reserve(m_ng->lane_count());

    for (std::size_t i = 0; i < m_ng->lane_count(); ++i) {
        const std::uint32_t s = m_ng->lane_offsets[i];
        const std::uint32_t e = m_ng->lane_offsets[i + 1];
        const std::size_t nPts = e - s;
        const std::uint32_t startV = static_cast<std::uint32_t>(verts.size() / 2);
        if (nPts < 2 || m_ng->lane_kind[i] == 1 /*rail*/) {
            m_laneFirst.push_back(startV);
            m_laneCount.push_back(0);
            continue;
        }
        extrudePolyline(&m_ng->lane_points[s * 2], nPts,
                        m_ng->lane_widths[i], verts);
        const std::uint32_t endV = static_cast<std::uint32_t>(verts.size() / 2);
        m_laneFirst.push_back(startV);
        m_laneCount.push_back(endV - startV);
        if (endV > startV) {
            m_firsts.push_back(static_cast<GLint>(startV));
            m_counts.push_back(static_cast<GLsizei>(endV - startV));
        }
    }
    m_totalVerts = static_cast<GLsizei>(verts.size() / 2);

    m_vao.bind();
    m_posVbo.bind();
    m_posVbo.allocate(verts.data(), static_cast<int>(verts.size() * sizeof(float)));
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    // Allocate color VBO sized for one rgba per vertex; fill with grey
    // until setLaneColors is called.
    m_colVbo.bind();
    m_colVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    std::vector<std::uint8_t> initCols(m_totalVerts * 4, 80);
    for (std::size_t v = 0; v < static_cast<std::size_t>(m_totalVerts); ++v) {
        initCols[v * 4 + 3] = 0;  // hidden until colors arrive
    }
    m_colVbo.allocate(initCols.data(), m_totalVerts * 4);
    m_gl->glEnableVertexAttribArray(1);
    m_gl->glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4, nullptr);
    m_colVbo.release();
    m_vao.release();
}

void EdgeColorLayer::setLaneColors(const std::vector<std::uint8_t>& rgba) {
    if (!m_gl || !m_program || m_totalVerts == 0) return;
    if (!m_ng || rgba.size() != m_ng->lane_count() * 4) {
        // Wrong shape — hide.
        m_hasColors = false;
        return;
    }
    std::vector<std::uint8_t> vcols(m_totalVerts * 4, 0);
    for (std::size_t i = 0; i < m_laneFirst.size(); ++i) {
        const std::uint32_t f = m_laneFirst[i];
        const std::uint32_t c = m_laneCount[i];
        const std::uint8_t r = rgba[i * 4];
        const std::uint8_t g = rgba[i * 4 + 1];
        const std::uint8_t b = rgba[i * 4 + 2];
        const std::uint8_t a = rgba[i * 4 + 3];
        for (std::uint32_t v = f; v < f + c; ++v) {
            vcols[v * 4]     = r;
            vcols[v * 4 + 1] = g;
            vcols[v * 4 + 2] = b;
            vcols[v * 4 + 3] = a;
        }
    }
    m_colVbo.bind();
    m_colVbo.write(0, vcols.data(), static_cast<int>(vcols.size()));
    m_colVbo.release();
    m_hasColors = true;
}

void EdgeColorLayer::draw(const float* proj) {
    if (!m_program || !m_gl || !m_active || !m_hasColors) return;
    if (m_firsts.empty()) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, proj);
    m_vao.bind();
    m_gl->glMultiDrawArrays(GL_TRIANGLE_STRIP,
                            m_firsts.data(), m_counts.data(),
                            static_cast<GLsizei>(m_firsts.size()));
    m_vao.release();
    m_gl->glUseProgram(0);
}
