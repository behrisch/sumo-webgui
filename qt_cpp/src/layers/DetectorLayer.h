#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>
#include <memory>

struct NetworkGeometry;

// Renders induction-loop and lane-area detectors. Loops draw as a single
// short perpendicular bar; lane-area as a longer band on top of the lane.
class DetectorLayer {
public:
    DetectorLayer();
    ~DetectorLayer();

    void initGL(QOpenGLFunctions_3_3_Core* gl);
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);
    void draw(const float* projectionMatColMajor);

private:
    void rebuild();

    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
    GLuint                     m_program = 0;
    int                        m_locProj = -1;

    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer            m_vbo{QOpenGLBuffer::VertexBuffer};
    GLsizei                  m_vertexCount = 0;

    std::shared_ptr<NetworkGeometry> m_ng;
    bool                             m_dirty = false;
};
