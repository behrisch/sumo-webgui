#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>
#include <cstdint>
#include <memory>
#include <vector>

struct NetworkGeometry;

// Overlay drawn on top of NetworkLayer's road fill when an edge-attribute
// colour mode is active. Re-extrudes lanes once and exposes a fast
// setLaneColors() that uploads one rgba8 per lane.
class EdgeColorLayer {
public:
    EdgeColorLayer();
    ~EdgeColorLayer();

    void initGL(QOpenGLFunctions_3_3_Core* gl);
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);

    // rgba_per_lane.size() must equal 4 * lane_count(). If alpha is 0 for
    // a lane its triangles are still drawn but invisible (effectively
    // skipped). Pass empty vector to hide the layer entirely.
    void setLaneColors(const std::vector<std::uint8_t>& rgba_per_lane);

    void draw(const float* projectionMatColMajor);

    void setActive(bool v) { m_active = v; }
    [[nodiscard]] bool isActive() const { return m_active; }

private:
    void rebuildGeometry();

    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
    GLuint                     m_program = 0;
    int                        m_locProj = -1;

    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer            m_posVbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer            m_colVbo{QOpenGLBuffer::VertexBuffer};

    std::vector<GLint>   m_firsts;
    std::vector<GLsizei> m_counts;
    std::vector<std::uint32_t> m_laneFirst;   // first vertex index per lane
    std::vector<std::uint32_t> m_laneCount;   // vertex count per lane
    GLsizei              m_totalVerts = 0;

    std::shared_ptr<NetworkGeometry> m_ng;
    bool m_dirty = false;
    bool m_active = false;
    bool m_hasColors = false;
};
