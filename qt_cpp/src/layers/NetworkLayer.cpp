#include "NetworkLayer.h"

#include <QDebug>
#include <cmath>
#include <vector>

#include "gl/Shader.h"
#include "sim/NetworkGeometry.h"

namespace {

constexpr const char* kVS = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
uniform mat4 u_proj;
void main() {
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
}
)";

constexpr const char* kFS = R"(#version 330 core
out vec4 frag;
uniform vec4 u_color;
void main() { frag = u_color; }
)";

// Extrude a polyline into a triangle strip of width `w` (full width).
// Emits two vertices per polyline point (left, right) so the strip can be
// drawn with GL_TRIANGLE_STRIP. Uses adjacent-segment averaging for the
// normal at interior vertices, segment normal at the endpoints, to avoid
// gaps at bends without producing the miter spikes typical at sharp angles.
void extrudePolyline(const float* pts, std::size_t nPts, float width,
                     std::vector<float>& outVerts) {
    if (nPts < 2 || width <= 0.0f) return;
    const float halfW = 0.5f * width;

    auto seg_normal = [](float x0, float y0, float x1, float y1,
                         float& nx, float& ny) {
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) { nx = 0.0f; ny = 0.0f; return; }
        // Left normal (rotate +90deg): (-dy, dx) / len.
        nx = -dy / len;
        ny = dx / len;
    };

    auto pushVerts = [&](float x, float y, float nx, float ny) {
        outVerts.push_back(x + nx * halfW);
        outVerts.push_back(y + ny * halfW);
        outVerts.push_back(x - nx * halfW);
        outVerts.push_back(y - ny * halfW);
    };

    float prevNx = 0.0f, prevNy = 0.0f;
    seg_normal(pts[0], pts[1], pts[2], pts[3], prevNx, prevNy);
    pushVerts(pts[0], pts[1], prevNx, prevNy);

    for (std::size_t i = 1; i + 1 < nPts; ++i) {
        const std::size_t k = i * 2;
        float n1x, n1y;
        seg_normal(pts[k - 2], pts[k - 1], pts[k], pts[k + 1], n1x, n1y);
        float n2x, n2y;
        seg_normal(pts[k], pts[k + 1], pts[k + 2], pts[k + 3], n2x, n2y);
        float avx = n1x + n2x;
        float avy = n1y + n2y;
        const float al = std::sqrt(avx * avx + avy * avy);
        if (al < 1e-4f) { avx = n2x; avy = n2y; }
        else            { avx /= al;  avy /= al; }
        pushVerts(pts[k], pts[k + 1], avx, avy);
        prevNx = n2x; prevNy = n2y;
    }

    const std::size_t lastK = (nPts - 1) * 2;
    pushVerts(pts[lastK], pts[lastK + 1], prevNx, prevNy);
}

// Append a degenerate transition so multiple strips can be drawn as one.
void emitStripBreak(std::vector<float>& outVerts) {
    if (outVerts.size() < 4) return;
    const std::size_t n = outVerts.size();
    const float lx = outVerts[n - 2];
    const float ly = outVerts[n - 1];
    outVerts.push_back(lx);
    outVerts.push_back(ly);
}

}  // namespace

NetworkLayer::NetworkLayer() = default;
NetworkLayer::~NetworkLayer() {
    if (m_gl && m_program) {
        m_gl->glDeleteProgram(m_program);
    }
}

void NetworkLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj  = m_gl->glGetUniformLocation(m_program, "u_proj");
    m_locColor = m_gl->glGetUniformLocation(m_program, "u_color");

    m_laneVao.create();
    m_laneVbo.create();
    m_juncVao.create();
    m_juncVbo.create();

    if (m_dirty) rebuildBuffers();
}

void NetworkLayer::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    m_dirty = true;
    if (m_gl && m_program) rebuildBuffers();
}

void NetworkLayer::rebuildBuffers() {
    m_dirty = false;
    m_laneVertexCount = 0;
    m_juncFirsts.clear();
    m_juncCounts.clear();
    if (!m_ng) return;

    // ---- Lanes: extrude into one big GL_TRIANGLE_STRIP with degenerate
    // bridges between lanes.
    std::vector<float> laneVerts;
    laneVerts.reserve(m_ng->lane_points.size() * 6);
    for (std::size_t i = 0; i < m_ng->lane_count(); ++i) {
        const std::uint32_t s = m_ng->lane_offsets[i];
        const std::uint32_t e = m_ng->lane_offsets[i + 1];
        const std::size_t nPts = e - s;
        if (nPts < 2) continue;

        const std::size_t startVerts = laneVerts.size();
        extrudePolyline(&m_ng->lane_points[s * 2], nPts,
                        m_ng->lane_widths[i], laneVerts);

        // Bridge: duplicate the first vertex of this strip after the last
        // vertex of the previous strip, plus a duplicate of the new strip's
        // first vertex. Two degenerates collapse adjacent triangles.
        if (i > 0 && laneVerts.size() > startVerts + 4) {
            const float fx = laneVerts[startVerts];
            const float fy = laneVerts[startVerts + 1];
            // Insert two degenerates BEFORE the new strip:
            // copy previous last, then this first.
            const std::size_t prevLastIdx = startVerts - 2;
            const float plx = laneVerts[prevLastIdx];
            const float ply = laneVerts[prevLastIdx + 1];
            // Need to shift current strip — easier to just insert.
            laneVerts.insert(laneVerts.begin() + startVerts,
                             {plx, ply, fx, fy});
        }
    }
    m_laneVertexCount = static_cast<GLsizei>(laneVerts.size() / 2);

    m_laneVao.bind();
    m_laneVbo.bind();
    m_laneVbo.allocate(laneVerts.data(),
                       static_cast<int>(laneVerts.size() * sizeof(float)));
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_laneVbo.release();
    m_laneVao.release();

    // ---- Junctions: one triangle fan per junction (assumes convex-ish
    // closed polygons; SUMO junction shapes are convex). We pack all fans
    // into one VBO and remember (first,count) for glMultiDrawArrays.
    std::vector<float> juncVerts;
    juncVerts.reserve(m_ng->junction_points.size() * 2);
    for (std::size_t i = 0; i < m_ng->junction_count(); ++i) {
        const std::uint32_t s = m_ng->junction_offsets[i];
        const std::uint32_t e = m_ng->junction_offsets[i + 1];
        if (e - s < 3) continue;

        // Compute centroid as fan center.
        float cx = 0.0f, cy = 0.0f;
        for (std::uint32_t p = s; p < e; ++p) {
            cx += m_ng->junction_points[p * 2];
            cy += m_ng->junction_points[p * 2 + 1];
        }
        cx /= static_cast<float>(e - s);
        cy /= static_cast<float>(e - s);

        const GLint first = static_cast<GLint>(juncVerts.size() / 2);
        juncVerts.push_back(cx); juncVerts.push_back(cy);
        for (std::uint32_t p = s; p < e; ++p) {
            juncVerts.push_back(m_ng->junction_points[p * 2]);
            juncVerts.push_back(m_ng->junction_points[p * 2 + 1]);
        }
        // Close the fan by repeating the first perimeter vertex.
        juncVerts.push_back(m_ng->junction_points[s * 2]);
        juncVerts.push_back(m_ng->junction_points[s * 2 + 1]);

        const GLsizei count = static_cast<GLsizei>((e - s) + 2);
        m_juncFirsts.push_back(first);
        m_juncCounts.push_back(count);
    }

    m_juncVao.bind();
    m_juncVbo.bind();
    m_juncVbo.allocate(juncVerts.data(),
                       static_cast<int>(juncVerts.size() * sizeof(float)));
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_juncVbo.release();
    m_juncVao.release();
}

void NetworkLayer::draw(const float* projectionMatColMajor) {
    if (!m_program || !m_gl) return;

    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, projectionMatColMajor);

    // Junctions first (slightly darker), then lanes on top. Same order as
    // deck.gl: junctions -> roads.
    if (!m_juncFirsts.empty()) {
        m_gl->glUniform4f(m_locColor, 0.20f, 0.20f, 0.22f, 1.0f);
        m_juncVao.bind();
        m_gl->glMultiDrawArrays(GL_TRIANGLE_FAN,
                                m_juncFirsts.data(),
                                m_juncCounts.data(),
                                static_cast<GLsizei>(m_juncFirsts.size()));
        m_juncVao.release();
    }

    if (m_laneVertexCount > 0) {
        m_gl->glUniform4f(m_locColor, 0.30f, 0.30f, 0.32f, 1.0f);
        m_laneVao.bind();
        m_gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, m_laneVertexCount);
        m_laneVao.release();
    }

    m_gl->glUseProgram(0);
}
