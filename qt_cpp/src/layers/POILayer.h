#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>
#include <memory>

struct NetworkGeometry;

// Points of Interest. Coloured disc per POI; positions + colors are static,
// uploaded once at setGeometry time.
class POILayer {
public:
    POILayer();
    ~POILayer();

    void initGL(QOpenGLFunctions_3_3_Core* gl);
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);
    void draw(const float* projectionMatColMajor);

private:
    void upload();

    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
    GLuint m_program = 0;
    GLint  m_locProj = -1;

    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_quadVbo {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_posVbo  {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_colVbo  {QOpenGLBuffer::VertexBuffer};

    std::shared_ptr<NetworkGeometry> m_ng;
    GLsizei m_instanceCount = 0;
    bool    m_dirty = false;
};
