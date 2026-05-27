#include "RailLayer.h"

#include <cmath>
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

// Approximate standard gauge (1.435 m) — half of this is the rail offset
// from the lane centerline.
constexpr float kGauge = 1.435f;
constexpr float kRailHalfWidth   = 0.10f;  // 20cm-thick rendered rail
constexpr float kSleeperHalfLen  = 1.20f;  // sleeper extends slightly past gauge
constexpr float kSleeperHalfWid  = 0.20f;
constexpr float kSleeperSpacing  = 3.5f;   // meters between sleepers

void pushQuad(std::vector<float>& v,
              float ax, float ay, float bx, float by,
              float cx, float cy, float dx, float dy) {
    v.insert(v.end(), {ax, ay, bx, by, cx, cy,
                       ax, ay, cx, cy, dx, dy});
}

// Emit triangle quads along a polyline offset by `offset` along each
// segment's left normal, half-thickness `half`.
void offsetStrip(const float* pts, std::size_t nPts,
                 float offset, float half,
                 std::vector<float>& out) {
    for (std::size_t i = 0; i + 1 < nPts; ++i) {
        const float x0 = pts[i * 2];
        const float y0 = pts[i * 2 + 1];
        const float x1 = pts[i * 2 + 2];
        const float y1 = pts[i * 2 + 3];
        const float ex = x1 - x0;
        const float ey = y1 - y0;
        const float len = std::sqrt(ex * ex + ey * ey);
        if (len < 1e-4f) continue;
        const float ux = ex / len;
        const float uy = ey / len;
        // Left normal (rotate +90): (-uy, ux).
        const float nx = -uy;
        const float ny = ux;
        const float cx0 = x0 + nx * offset;
        const float cy0 = y0 + ny * offset;
        const float cx1 = x1 + nx * offset;
        const float cy1 = y1 + ny * offset;
        pushQuad(out,
            cx0 + nx * half, cy0 + ny * half,
            cx1 + nx * half, cy1 + ny * half,
            cx1 - nx * half, cy1 - ny * half,
            cx0 - nx * half, cy0 - ny * half);
    }
}

// Place sleepers at uniform spacing along the polyline.
void placeSleepers(const float* pts, std::size_t nPts,
                   std::vector<float>& out) {
    float distSinceLast = kSleeperSpacing * 0.5f;
    for (std::size_t i = 0; i + 1 < nPts; ++i) {
        const float x0 = pts[i * 2];
        const float y0 = pts[i * 2 + 1];
        const float x1 = pts[i * 2 + 2];
        const float y1 = pts[i * 2 + 3];
        float ex = x1 - x0;
        float ey = y1 - y0;
        float segLen = std::sqrt(ex * ex + ey * ey);
        if (segLen < 1e-4f) continue;
        const float ux = ex / segLen;
        const float uy = ey / segLen;
        const float nx = -uy;
        const float ny = ux;
        float along = kSleeperSpacing - distSinceLast;
        while (along <= segLen) {
            const float cx = x0 + ux * along;
            const float cy = y0 + uy * along;
            // Sleeper centered on cx,cy, oriented perpendicular to lane.
            pushQuad(out,
                cx + nx * kSleeperHalfLen - ux * kSleeperHalfWid,
                cy + ny * kSleeperHalfLen - uy * kSleeperHalfWid,
                cx + nx * kSleeperHalfLen + ux * kSleeperHalfWid,
                cy + ny * kSleeperHalfLen + uy * kSleeperHalfWid,
                cx - nx * kSleeperHalfLen + ux * kSleeperHalfWid,
                cy - ny * kSleeperHalfLen + uy * kSleeperHalfWid,
                cx - nx * kSleeperHalfLen - ux * kSleeperHalfWid,
                cy - ny * kSleeperHalfLen - uy * kSleeperHalfWid);
            along += kSleeperSpacing;
        }
        distSinceLast = (segLen - (along - kSleeperSpacing));
    }
}
}  // namespace

RailLayer::RailLayer() = default;
RailLayer::~RailLayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void RailLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj  = m_gl->glGetUniformLocation(m_program, "u_proj");
    m_locColor = m_gl->glGetUniformLocation(m_program, "u_color");
    m_railVao.create(); m_railVbo.create();
    m_sleeperVao.create(); m_sleeperVbo.create();
    if (m_dirty) rebuild();
}

void RailLayer::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    m_dirty = true;
    if (m_gl && m_program) rebuild();
}

void RailLayer::rebuild() {
    m_dirty = false;
    m_railVerts = 0;
    m_sleeperVerts = 0;
    if (!m_ng) return;

    std::vector<float> rails;
    std::vector<float> sleepers;
    const float halfGauge = 0.5f * kGauge;
    for (std::size_t i = 0; i < m_ng->lane_count(); ++i) {
        if (m_ng->lane_kind[i] != 1) continue;
        const std::uint32_t s = m_ng->lane_offsets[i];
        const std::uint32_t e = m_ng->lane_offsets[i + 1];
        const std::size_t nPts = e - s;
        if (nPts < 2) continue;
        const float* pts = &m_ng->lane_points[s * 2];
        // Left rail (+halfGauge) and right rail (-halfGauge).
        offsetStrip(pts, nPts,  halfGauge, kRailHalfWidth, rails);
        offsetStrip(pts, nPts, -halfGauge, kRailHalfWidth, rails);
        placeSleepers(pts, nPts, sleepers);
    }

    m_railVerts = static_cast<GLsizei>(rails.size() / 2);
    m_railVao.bind();
    m_railVbo.bind();
    m_railVbo.allocate(rails.data(), static_cast<int>(rails.size() * sizeof(float)));
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_railVbo.release();
    m_railVao.release();

    m_sleeperVerts = static_cast<GLsizei>(sleepers.size() / 2);
    m_sleeperVao.bind();
    m_sleeperVbo.bind();
    m_sleeperVbo.allocate(sleepers.data(),
                          static_cast<int>(sleepers.size() * sizeof(float)));
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_sleeperVbo.release();
    m_sleeperVao.release();
}

void RailLayer::draw(const float* proj) {
    if (!m_program || !m_gl) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, proj);

    if (m_sleeperVerts > 0) {
        m_gl->glUniform4f(m_locColor, 0.32f, 0.22f, 0.15f, 1.0f);
        m_sleeperVao.bind();
        m_gl->glDrawArrays(GL_TRIANGLES, 0, m_sleeperVerts);
        m_sleeperVao.release();
    }
    if (m_railVerts > 0) {
        m_gl->glUniform4f(m_locColor, 0.85f, 0.85f, 0.90f, 1.0f);
        m_railVao.bind();
        m_gl->glDrawArrays(GL_TRIANGLES, 0, m_railVerts);
        m_railVao.release();
    }
    m_gl->glUseProgram(0);
}
