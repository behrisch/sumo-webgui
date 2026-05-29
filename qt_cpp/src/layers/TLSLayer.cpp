#include "TLSLayer.h"

#include <array>
#include <cstdint>
#include <vector>

#include "gl/Shader.h"
#include "sim/NetworkGeometry.h"

namespace {

// TLS bar geometry: drawn as a rectangle whose long side matches the lane
// width and whose short side is kBarLen along the lane direction.  The bar
// is placed kBarBack metres upstream of the lane end, so the white stop
// line at the lane end remains visible just in front of the colored bar.
constexpr float kBarLen  = 0.9f;
constexpr float kBarBack = 1.2f;
constexpr std::array<float, 12> kQuad = {
    -0.5f, -0.5f,  0.5f, -0.5f,  0.5f,  0.5f,
    -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f,
};

constexpr const char* kVS = R"(#version 330 core
layout(location=0) in vec2 a_local;     // [-0.5, 0.5] x [-0.5, 0.5]
layout(location=1) in vec2 a_pos;       // bar center (world m)
layout(location=2) in vec2 a_tan;       // unit forward lane tangent
layout(location=3) in float a_width;    // lane width (m)
layout(location=4) in vec4 a_color;
uniform mat4 u_proj;
out vec4 v_color;
out vec2 v_local;
void main() {
    vec2 perp = vec2(-a_tan.y, a_tan.x);
    // a_local.x along lane (length), a_local.y across lane (width).
    // Bar quad is already pre-scaled along the lane axis (kBarLen), so the
    // VS just multiplies a_local.x by the tangent directly.  Across-lane
    // width comes from a_width × perp (half on each side via a_local.y).
    vec2 p = a_pos + a_local.x * a_tan + a_local.y * perp * a_width;
    gl_Position = u_proj * vec4(p, 0.0, 1.0);
    v_color = a_color;
    v_local = a_local;
}
)";

constexpr const char* kFS = R"(#version 330 core
in vec4 v_color;
in vec2 v_local;
out vec4 frag;
void main() {
    // Slight darkening at the rim for a subtle outline.
    float r = max(abs(v_local.x), abs(v_local.y)) * 2.0;
    float rim = smoothstep(1.0, 0.85, r);
    vec3 c = mix(v_color.rgb * 0.55, v_color.rgb, rim);
    frag = vec4(c, v_color.a);
}
)";

struct Rgba { std::uint8_t r, g, b, a; };

Rgba colorForStateChar(char c) {
    switch (c) {
        case 'r': case 'R': return {220,  40,  40, 255};
        case 'y': case 'Y': return {235, 200,  40, 255};
        case 'g':           return { 50, 180,  90, 255};
        case 'G':           return { 80, 230, 100, 255};
        case 's':           return {220,  40,  40, 255};
        case 'u':           return {235, 130,  40, 255};
        case 'o': case 'O': return { 90,  90,  90, 255};
        default:            return {120, 120, 120, 255};
    }
}

}  // namespace

TLSLayer::TLSLayer() = default;
TLSLayer::~TLSLayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void TLSLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj = m_gl->glGetUniformLocation(m_program, "u_proj");

    m_vao.create();
    m_quadVbo.create();
    m_posVbo.create();
    m_colVbo.create();
    m_tanVbo.create();
    m_widthVbo.create();

    m_vao.bind();
    m_quadVbo.bind();
    {
        // Pre-scale a_local.x by kBarLen so the VS can do the lane-axis
        // contribution as a_local.x * a_tan (no separate length attribute).
        std::array<float, 12> q = kQuad;
        for (int i = 0; i < 6; ++i) q[i * 2] *= kBarLen;
        m_quadVbo.allocate(q.data(), static_cast<int>(q.size() * sizeof(float)));
    }
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    m_posVbo.bind();
    m_gl->glEnableVertexAttribArray(1);
    m_gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_gl->glVertexAttribDivisor(1, 1);

    m_tanVbo.bind();
    m_gl->glEnableVertexAttribArray(2);
    m_gl->glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_gl->glVertexAttribDivisor(2, 1);

    m_widthVbo.bind();
    m_gl->glEnableVertexAttribArray(3);
    m_gl->glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
    m_gl->glVertexAttribDivisor(3, 1);

    m_colVbo.bind();
    m_gl->glEnableVertexAttribArray(4);
    m_gl->glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4, nullptr);
    m_gl->glVertexAttribDivisor(4, 1);
    m_vao.release();

    if (m_posDirty) uploadPositions();
    if (m_colDirty) uploadColors();
}

void TLSLayer::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    m_posDirty = true;
    if (m_gl && m_program) uploadPositions();
}

void TLSLayer::setSnapshot(const SimSnapshotPtr& snap) {
    m_snap = snap;
    m_colDirty = true;
    if (m_gl && m_program) uploadColors();
}

void TLSLayer::uploadPositions() {
    m_posDirty = false;
    if (!m_ng || m_ng->tls_marker_count() == 0) {
        m_instanceCount = 0;
        return;
    }
    const std::size_t n = m_ng->tls_marker_count();
    std::vector<float> pos(n * 2);
    std::vector<float> tan(n * 2);
    std::vector<float> wid(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Shift bar centre kBarBack metres upstream of the lane end so the
        // white stop-line at the lane end stays visible in front of it.
        pos[i * 2 + 0] = m_ng->tls_x[i] - m_ng->tls_dx[i] * kBarBack;
        pos[i * 2 + 1] = m_ng->tls_y[i] - m_ng->tls_dy[i] * kBarBack;
        tan[i * 2 + 0] = m_ng->tls_dx[i];
        tan[i * 2 + 1] = m_ng->tls_dy[i];
        wid[i]         = m_ng->tls_w[i];
    }
    m_posVbo.bind();
    m_posVbo.allocate(pos.data(), static_cast<int>(pos.size() * sizeof(float)));
    m_tanVbo.bind();
    m_tanVbo.allocate(tan.data(), static_cast<int>(tan.size() * sizeof(float)));
    m_widthVbo.bind();
    m_widthVbo.allocate(wid.data(), static_cast<int>(wid.size() * sizeof(float)));
    m_widthVbo.release();
    m_instanceCount = static_cast<GLsizei>(n);
}

void TLSLayer::uploadColors() {
    m_colDirty = false;
    if (!m_ng || m_instanceCount == 0) return;
    const std::size_t n = m_ng->tls_marker_count();
    std::vector<Rgba> cols(n, Rgba{120, 120, 120, 255});
    if (m_snap) {
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = m_snap->tls_states.find(m_ng->tls_ids[i]);
            if (it == m_snap->tls_states.end()) continue;
            const std::uint32_t idx = m_ng->tls_state_index[i];
            if (idx < it->second.size()) {
                cols[i] = colorForStateChar(it->second[idx]);
            }
        }
    }
    m_colVbo.bind();
    m_colVbo.allocate(cols.data(), static_cast<int>(cols.size() * sizeof(Rgba)));
    m_colVbo.release();
}

void TLSLayer::draw(const float* projectionMatColMajor) {
    if (!m_program || m_instanceCount == 0) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, projectionMatColMajor);
    m_vao.bind();
    m_gl->glDrawArraysInstanced(GL_TRIANGLES, 0, 6, m_instanceCount);
    m_vao.release();
    m_gl->glUseProgram(0);
}
