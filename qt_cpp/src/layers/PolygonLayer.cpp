#include "PolygonLayer.h"

#include "gl/Shader.h"
#include "sim/NetworkGeometry.h"

namespace {

constexpr const char* kVS = R"(#version 330 core
layout(location=0) in vec2 a_pos;
layout(location=1) in vec4 a_color;
uniform mat4 u_proj;
out vec4 v_color;
void main() {
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
    v_color = a_color;
}
)";

constexpr const char* kFS = R"(#version 330 core
in vec4 v_color;
out vec4 frag;
void main() { frag = v_color; }
)";

struct Vert {
    float x, y;
    std::uint8_t r, g, b, a;
};
static_assert(sizeof(Vert) == 12);

}  // namespace

PolygonLayer::PolygonLayer() = default;
PolygonLayer::~PolygonLayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void PolygonLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj = m_gl->glGetUniformLocation(m_program, "u_proj");

    auto setupVao = [&](QOpenGLVertexArrayObject& vao, QOpenGLBuffer& vbo) {
        vao.create();
        vbo.create();
        vao.bind();
        vbo.bind();
        m_gl->glEnableVertexAttribArray(0);
        m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vert),
                                    reinterpret_cast<void*>(offsetof(Vert, x)));
        m_gl->glEnableVertexAttribArray(1);
        m_gl->glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vert),
                                    reinterpret_cast<void*>(offsetof(Vert, r)));
        vao.release();
    };
    setupVao(m_fillVao, m_fillVbo);
    setupVao(m_lineVao, m_lineVbo);

    if (m_dirty) rebuildBuffers();
}

void PolygonLayer::setGeometry(const std::shared_ptr<NetworkGeometry>& ng) {
    m_ng = ng;
    m_dirty = true;
    if (m_gl && m_program) rebuildBuffers();
}

void PolygonLayer::rebuildBuffers() {
    m_dirty = false;
    m_fillFirsts.clear(); m_fillCounts.clear();
    m_lineFirsts.clear(); m_lineCounts.clear();
    if (!m_ng) return;

    std::vector<Vert> fills, lines;
    for (std::size_t i = 0; i < m_ng->polygon_count(); ++i) {
        const std::uint32_t s = m_ng->polygon_offsets[i];
        const std::uint32_t e = m_ng->polygon_offsets[i + 1];
        if (e - s < 2) continue;
        const Vert col{
            0, 0,
            m_ng->polygon_rgba[i * 4 + 0],
            m_ng->polygon_rgba[i * 4 + 1],
            m_ng->polygon_rgba[i * 4 + 2],
            m_ng->polygon_rgba[i * 4 + 3]};

        if (m_ng->polygon_filled[i] && e - s >= 3) {
            // Triangle fan around centroid. Convex enough for typical
            // buildings; concave polygons will look approximate (deferred).
            float cx = 0.0f, cy = 0.0f;
            for (std::uint32_t p = s; p < e; ++p) {
                cx += m_ng->polygon_points[p * 2];
                cy += m_ng->polygon_points[p * 2 + 1];
            }
            cx /= static_cast<float>(e - s);
            cy /= static_cast<float>(e - s);
            const GLint first = static_cast<GLint>(fills.size());
            fills.push_back({cx, cy, col.r, col.g, col.b, col.a});
            for (std::uint32_t p = s; p < e; ++p) {
                fills.push_back({m_ng->polygon_points[p * 2],
                                 m_ng->polygon_points[p * 2 + 1],
                                 col.r, col.g, col.b, col.a});
            }
            // Close the fan.
            fills.push_back({m_ng->polygon_points[s * 2],
                             m_ng->polygon_points[s * 2 + 1],
                             col.r, col.g, col.b, col.a});
            m_fillFirsts.push_back(first);
            m_fillCounts.push_back(static_cast<GLsizei>((e - s) + 2));
        } else {
            const GLint first = static_cast<GLint>(lines.size());
            for (std::uint32_t p = s; p < e; ++p) {
                lines.push_back({m_ng->polygon_points[p * 2],
                                 m_ng->polygon_points[p * 2 + 1],
                                 col.r, col.g, col.b, col.a});
            }
            m_lineFirsts.push_back(first);
            m_lineCounts.push_back(static_cast<GLsizei>(e - s));
        }
    }

    m_fillVao.bind();
    m_fillVbo.bind();
    m_fillVbo.allocate(fills.data(), static_cast<int>(fills.size() * sizeof(Vert)));
    m_fillVao.release();

    m_lineVao.bind();
    m_lineVbo.bind();
    m_lineVbo.allocate(lines.data(), static_cast<int>(lines.size() * sizeof(Vert)));
    m_lineVao.release();
}

void PolygonLayer::draw(const float* projectionMatColMajor) {
    if (!m_program) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, projectionMatColMajor);
    if (!m_fillFirsts.empty()) {
        m_fillVao.bind();
        m_gl->glMultiDrawArrays(GL_TRIANGLE_FAN,
                                m_fillFirsts.data(),
                                m_fillCounts.data(),
                                static_cast<GLsizei>(m_fillFirsts.size()));
        m_fillVao.release();
    }
    if (!m_lineFirsts.empty()) {
        m_lineVao.bind();
        m_gl->glMultiDrawArrays(GL_LINE_STRIP,
                                m_lineFirsts.data(),
                                m_lineCounts.data(),
                                static_cast<GLsizei>(m_lineFirsts.size()));
        m_lineVao.release();
    }
    m_gl->glUseProgram(0);
}
