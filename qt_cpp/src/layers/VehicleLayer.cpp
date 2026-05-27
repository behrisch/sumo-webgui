#include "VehicleLayer.h"

#include <array>

#include "gl/Shader.h"

namespace {

// Quad in local vehicle frame: length along +x, width along +y, centred on
// origin so the vehicle's position is its centre. Two triangles, CCW.
constexpr float kHalfLen = 2.5f;  // meters
constexpr float kHalfWid = 1.0f;
constexpr std::array<float, 12> kQuad = {
    -kHalfLen, -kHalfWid,
     kHalfLen, -kHalfWid,
     kHalfLen,  kHalfWid,
    -kHalfLen, -kHalfWid,
     kHalfLen,  kHalfWid,
    -kHalfLen,  kHalfWid,
};

constexpr const char* kVS = R"(#version 330 core
layout(location=0) in vec2 a_local;        // quad vertex in vehicle frame
layout(location=1) in vec2 a_pos;          // per-instance world XY
layout(location=2) in vec2 a_rot;          // per-instance cos(a),sin(a)
layout(location=3) in vec4 a_color;        // per-instance rgba (normalised)
uniform mat4 u_proj;
out vec4 v_color;
void main() {
    float c = a_rot.x;
    float s = a_rot.y;
    vec2 rotated = vec2(c*a_local.x - s*a_local.y,
                        s*a_local.x + c*a_local.y);
    vec2 world = a_pos + rotated;
    gl_Position = u_proj * vec4(world, 0.0, 1.0);
    v_color = a_color;
}
)";

constexpr const char* kFS = R"(#version 330 core
in  vec4 v_color;
out vec4 frag;
void main() { frag = v_color; }
)";

}  // namespace

VehicleLayer::VehicleLayer() = default;
VehicleLayer::~VehicleLayer() {
    if (m_gl && m_program) m_gl->glDeleteProgram(m_program);
}

void VehicleLayer::initGL(QOpenGLFunctions_3_3_Core* gl) {
    m_gl = gl;
    m_program = buildProgram(*m_gl, kVS, kFS);
    if (!m_program) return;
    m_locProj = m_gl->glGetUniformLocation(m_program, "u_proj");

    m_vao.create();
    m_quadVbo.create();
    m_posVbo.create();
    m_rotVbo.create();
    m_colVbo.create();

    m_vao.bind();

    m_quadVbo.bind();
    m_quadVbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
    m_quadVbo.allocate(kQuad.data(), static_cast<int>(kQuad.size() * sizeof(float)));
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    m_posVbo.bind();
    m_posVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    m_gl->glEnableVertexAttribArray(1);
    m_gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_gl->glVertexAttribDivisor(1, 1);

    m_rotVbo.bind();
    m_rotVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    m_gl->glEnableVertexAttribArray(2);
    m_gl->glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_gl->glVertexAttribDivisor(2, 1);

    m_colVbo.bind();
    m_colVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    m_gl->glEnableVertexAttribArray(3);
    m_gl->glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4 * sizeof(GLubyte), nullptr);
    m_gl->glVertexAttribDivisor(3, 1);

    m_vao.release();
}

void VehicleLayer::setSnapshot(const SimSnapshotPtr& snap) {
    if (!m_gl || !m_program) return;
    if (!snap || snap->vehicle_count() == 0) {
        m_instanceCount = 0;
        return;
    }
    const std::size_t n = snap->vehicle_count();

    // Interleave x/y into one buffer and cos/sin into another. Snapshot
    // already stores them as parallel arrays — pack them tightly.
    std::vector<float> pos(n * 2);
    std::vector<float> rot(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        pos[i * 2 + 0] = snap->x[i];
        pos[i * 2 + 1] = snap->y[i];
        rot[i * 2 + 0] = snap->cos_a[i];
        rot[i * 2 + 1] = snap->sin_a[i];
    }

    m_posVbo.bind();
    m_posVbo.allocate(pos.data(), static_cast<int>(pos.size() * sizeof(float)));
    m_rotVbo.bind();
    m_rotVbo.allocate(rot.data(), static_cast<int>(rot.size() * sizeof(float)));
    m_colVbo.bind();
    m_colVbo.allocate(snap->rgba.data(), static_cast<int>(snap->rgba.size()));
    m_colVbo.release();

    m_instanceCount = static_cast<GLsizei>(n);
}

void VehicleLayer::draw(const float* projectionMatColMajor) {
    if (!m_program || m_instanceCount == 0) return;
    m_gl->glUseProgram(m_program);
    m_gl->glUniformMatrix4fv(m_locProj, 1, GL_FALSE, projectionMatColMajor);
    m_vao.bind();
    m_gl->glDrawArraysInstanced(GL_TRIANGLES, 0, 6, m_instanceCount);
    m_vao.release();
    m_gl->glUseProgram(0);
}
