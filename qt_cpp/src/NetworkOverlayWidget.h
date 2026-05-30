#pragma once

#include <QtWidgets/QWidget>

class NetworkView;

// Transparent child widget that paints all NetworkView overlays (scale bar,
// legend, pick info box) with QPainter. RhiWidgetBase's render path is GPU-
// only; QPainter-over-QRhiWidget is unverified on the Qt 6.4 backport, so
// we composite via a sibling widget instead.
//
// Owns no state; reads from the parent NetworkView via friend access. The
// parent triggers repaints by calling `update()` after each render.
class NetworkOverlayWidget : public QWidget {
    Q_OBJECT
public:
    explicit NetworkOverlayWidget(NetworkView* view);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    void drawScaleBar(QPainter& p);
    void drawLegend(QPainter& p);
    void drawInfoBox(QPainter& p);

    NetworkView* m_view = nullptr;
};
