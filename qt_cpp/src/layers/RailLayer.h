#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>
#include <memory>

struct NetworkGeometry;

// Renders rail lanes (lane_kind == 1) as two parallel steel-coloured rails
// with periodic perpendicular sleeper marks.
class RailLayer {
public:
    RailLayer();
    ~RailLayer();

    void initGL(QOpenGLFunctions_3_3_Core* gl);
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);
    void draw(const float* projectionMatColMajor);

private:
    void rebuild();

    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
    GLuint                     m_program = 0;
    int                        m_locProj  = -1;
    int                        m_locColor = -1;

    QOpenGLVertexArrayObject m_railVao;
    QOpenGLBuffer            m_railVbo{QOpenGLBuffer::VertexBuffer};
    GLsizei                  m_railVerts = 0;

    QOpenGLVertexArrayObject m_sleeperVao;
    QOpenGLBuffer            m_sleeperVbo{QOpenGLBuffer::VertexBuffer};
    GLsizei                  m_sleeperVerts = 0;

    std::shared_ptr<NetworkGeometry> m_ng;
    bool                             m_dirty = false;
};
