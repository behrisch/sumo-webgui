#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>
#include <memory>

struct NetworkGeometry;

// Phase 1 network drawing: each lane is rendered as a CPU-extruded
// triangle strip (one quad per polyline segment). Junctions are filled as
// triangle fans about their centroid. Both use a single solid color uniform.
// Edge-data coloring + rail sleepers + crossings styling land in later
// phases.
class NetworkLayer {
public:
    NetworkLayer();
    ~NetworkLayer();

    NetworkLayer(const NetworkLayer&)            = delete;
    NetworkLayer& operator=(const NetworkLayer&) = delete;

    // Called once we have a current GL context.
    void initGL(QOpenGLFunctions_3_3_Core* gl);

    // Re-uploads geometry. Safe to call when ng is null (clears buffers).
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);

    // Pass projection matrix (column-major 4x4) and draw.
    void draw(const float* projectionMatColMajor);

private:
    void rebuildBuffers();

    QOpenGLFunctions_3_3_Core* m_gl = nullptr;

    GLuint m_program  = 0;
    GLint  m_locProj  = -1;
    GLint  m_locColor = -1;

    QOpenGLVertexArrayObject m_laneVao;
    QOpenGLBuffer            m_laneVbo{QOpenGLBuffer::VertexBuffer};
    GLsizei                  m_laneVertexCount = 0;

    QOpenGLVertexArrayObject m_juncVao;
    QOpenGLBuffer            m_juncVbo{QOpenGLBuffer::VertexBuffer};
    // For junctions we draw multiple triangle fans; remember (first,count) per
    // junction.
    std::vector<GLint>   m_juncFirsts;
    std::vector<GLsizei> m_juncCounts;

    std::shared_ptr<NetworkGeometry> m_ng;
    bool m_dirty = false;
};
