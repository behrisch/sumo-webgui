#pragma once

#include <array>

// Simple 2D orthographic camera over a SUMO-XY world (meters). Tracks the
// world coordinate at the centre of the viewport and a zoom expressed in
// pixels-per-meter. Provides a column-major mat4 mapping world XY to NDC.
class Camera {
public:
    void setViewport(int wPx, int hPx) noexcept;
    [[nodiscard]] int viewportWidth()  const noexcept { return m_wPx; }
    [[nodiscard]] int viewportHeight() const noexcept { return m_hPx; }

    void setCenter(double x, double y) noexcept { m_cx = x; m_cy = y; }
    [[nodiscard]] double centerX() const noexcept { return m_cx; }
    [[nodiscard]] double centerY() const noexcept { return m_cy; }

    // Pixels per meter (or per world unit). Higher = more zoomed in.
    void setPixelsPerUnit(double ppu) noexcept;
    [[nodiscard]] double pixelsPerUnit() const noexcept { return m_ppu; }

    // Zoom about a pixel-space anchor (so the world point under the cursor
    // stays put). `factor` < 1 zooms out, > 1 zooms in.
    void zoomAtPixel(double pxX, double pxY, double factor) noexcept;

    // Pan in pixel space (positive dx moves the view right, i.e. world
    // contents move left under the viewport).
    void panPixels(double dxPx, double dyPx) noexcept;

    // Reframe to fit a world-space bbox with `padding` fraction (0.05 = 5%).
    void fitBounds(double minX, double minY,
                   double maxX, double maxY,
                   double padding = 0.05) noexcept;

    // Convert pixel coordinates (origin top-left, y down) to world XY.
    void pixelToWorld(double pxX, double pxY,
                      double& outX, double& outY) const noexcept;

    // Column-major 4x4 mat: world XY -> NDC ([-1,1]). Z is set to 0.
    [[nodiscard]] std::array<float, 16> projection() const noexcept;

private:
    int    m_wPx = 1;
    int    m_hPx = 1;
    double m_cx  = 0.0;
    double m_cy  = 0.0;
    double m_ppu = 1.0;
};
