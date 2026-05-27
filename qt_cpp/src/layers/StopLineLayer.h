#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>
#include <memory>

struct NetworkGeometry;

// Renders white stop bars perpendicular to each TLS-controlled approach
// lane, at the lane endpoint. Static geometry (one quad per stopline).
class StopLineLayer {
public:
    StopLineLayer();
    ~StopLineLayer();

    void initGL(QOpenGLFunctions_3_3_Core* gl);
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);
    void draw(const float* projectionMatColMajor);

private:
    void rebuild();

    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
    GLuint                     m_program = 0;
    int                        m_locProj  = -1;
    int                        m_locColor = -1;

    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer            m_vbo{QOpenGLBuffer::VertexBuffer};
    GLsizei                  m_vertexCount = 0;

    std::shared_ptr<NetworkGeometry> m_ng;
    bool                             m_dirty = false;
};
