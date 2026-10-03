// SPDX-License-Identifier: MPL-2.0

#include "SimulatorWidget.h"

#include <QAbstractButton>
#include <QAbstractSocket>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>


#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>
#include <iterator>

namespace
{

QString safeServerName(const oxr::protocol::ServerAnnounce& announce)
{
    return QString::fromUtf8(
        announce.serverName,
        static_cast<int>(strnlen(announce.serverName, sizeof(announce.serverName))));
}

QString platformSimulatorDeviceName()
{
#if defined(Q_OS_MACOS)
    return "OXRSys Qt Simulator macOS";
#elif defined(Q_OS_WIN)
    return "OXRSys Qt Simulator Windows";
#elif defined(Q_OS_LINUX)
    return "OXRSys Qt Simulator Linux";
#else
    return "OXRSys Qt Simulator";
#endif
}

QLabel* makeSecondaryLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet("color: #9aa0a6;");
    return label;
}

QFrame* makePanel(QWidget* parent)
{
    auto* panel = new QFrame(parent);
    panel->setFrameShape(QFrame::StyledPanel);
    panel->setStyleSheet("QFrame { background: #171a20; border: 1px solid #30343c; border-radius: 8px; }");
    return panel;
}

struct Quaternion
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quaternion multiply(const Quaternion& lhs, const Quaternion& rhs)
{
    return {
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    };
}

Quaternion axisAngle(float x, float y, float z, float angle)
{
    const float halfAngle = angle * 0.5f;
    const float sine = std::sin(halfAngle);
    return {x * sine, y * sine, z * sine, std::cos(halfAngle)};
}

Quaternion headQuaternion(float yaw, float pitch, float roll)
{
    return multiply(multiply(axisAngle(0.0f, 1.0f, 0.0f, yaw),
                            axisAngle(1.0f, 0.0f, 0.0f, pitch)),
                    axisAngle(0.0f, 0.0f, 1.0f, roll));
}

float radiansToDegrees(float radians)
{
    return radians * 57.2957795131f;
}

// Per-tick increments of a running total, the last 10 s at the 250 ms stats rate.
struct Sparkline
{
    std::deque<quint64> deltas;
    quint64 last = 0;
    bool primed = false;

    void sample(quint64 total)
    {
        deltas.push_back(primed && total >= last ? total - last : 0);
        last = total;
        primed = true;
        if (deltas.size() > 40)
        {
            deltas.pop_front();
        }
    }

    // Each segment is coloured by its own sample, so a fault shows while it lasts and clears with it.
    // The scale is 1.5x the 90th percentile; a spike above it clips to the top instead of flattening the rest.
    void draw(QPainter& painter, const QRectF& area, bool faultSeries) const
    {
        painter.setPen(QPen(QColor(65, 78, 94), 1.0));
        painter.drawLine(area.bottomLeft(), area.bottomRight());
        if (deltas.size() < 2)
        {
            return;
        }
        std::vector<quint64> sorted(deltas.begin(), deltas.end());
        std::sort(sorted.begin(), sorted.end());
        const quint64 scale = std::max<quint64>(1, sorted[(sorted.size() - 1) * 9 / 10] * 3 / 2);
        const qreal step = area.width() / 39.0;
        const qreal x0 = area.right() - step * static_cast<qreal>(deltas.size() - 1);
        const auto point = [&](size_t i) {
            const qreal level = std::min<qreal>(1.0, static_cast<qreal>(deltas[i]) / static_cast<qreal>(scale));
            return QPointF(x0 + step * static_cast<qreal>(i), area.bottom() - area.height() * level);
        };
        for (size_t i = 1; i < deltas.size(); ++i)
        {
            const bool fault = faultSeries && deltas[i] > 0;
            const bool clipped = deltas[i] > scale;
            const QColor colour = fault ? QColor(240, 98, 98) : clipped ? QColor(242, 204, 96) : QColor(126, 231, 135);
            painter.setPen(QPen(colour, 1.25));
            painter.drawLine(point(i - 1), point(i));
        }
    }
};

} // namespace

// The Tracking panel's counters as labelled sparklines, one row each, sampled at the stats rate.
class SparklineStrip final : public QWidget
{
public:
    SparklineStrip(QStringList labels, QWidget* parent)
        : QWidget(parent)
        , labels_(std::move(labels))
        , sparks_(static_cast<size_t>(labels_.size()))
    {
        setMinimumHeight(18 * labels_.size());
    }

    void sample(const std::vector<quint64>& totals)
    {
        for (size_t i = 0; i < sparks_.size() && i < totals.size(); ++i)
        {
            sparks_[i].sample(totals[i]);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal row = static_cast<qreal>(height()) / static_cast<qreal>(sparks_.size());
        qreal labelWidth = 0.0;
        for (const QString& label : labels_)
        {
            labelWidth = std::max<qreal>(labelWidth, painter.fontMetrics().horizontalAdvance(label));
        }
        for (size_t i = 0; i < sparks_.size(); ++i)
        {
            const QRectF line(0.0, row * static_cast<qreal>(i) + 2.0, width(), row - 4.0);
            painter.setPen(QColor(154, 160, 166));
            painter.drawText(line, Qt::AlignLeft | Qt::AlignVCenter, labels_[static_cast<int>(i)]);
            const QString& label = labels_[static_cast<int>(i)];
            sparks_[i].draw(painter, line.adjusted(labelWidth + 8.0, 0.0, 0.0, 0.0),
                            label.contains("drop") || label.contains("fec") || label.contains("error"));
        }
    }

private:
    QStringList labels_;
    std::vector<Sparkline> sparks_;
};

class SimulatorPreviewWidget final : public QWidget
{
public:
    explicit SimulatorPreviewWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(220);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
    }

    void setPose(float yaw,
                 float pitch,
                 float roll,
                 const float position[3],
                 bool mouseCaptured,
                 bool streaming)
    {
        yaw_ = yaw;
        pitch_ = pitch;
        roll_ = roll;
        position_[0] = position[0];
        position_[1] = position[1];
        position_[2] = position[2];
        mouseCaptured_ = mouseCaptured;
        streaming_ = streaming;
        update();
        publishOverlay();
    }

    // Takes the live pose into the badge text; called at the stats rate.
    void latchBadgePose()
    {
        shownYaw_ = yaw_;
        shownPitch_ = pitch_;
        std::copy(std::begin(position_), std::end(position_), std::begin(shownPosition_));
        update();
        publishOverlay();
    }

    void setVideoView(QWidget* view, PyroWaveDecoder* decoder)
    {
        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(view);
        videoView_ = view;
        decoder_ = decoder;
        videoView_->setFocusPolicy(Qt::NoFocus);
        videoView_->installEventFilter(this);
        videoView_->hide();
    }

    void setVideoActive(bool active)
    {
        if (videoView_ != nullptr && videoView_->isVisible() != active)
        {
            videoView_->setVisible(active);
            publishOverlay();
        }
    }

    void setStatusOverlay(const QString& videoStatus,
                          quint64 videoPackets,
                          quint64 videoFrames,
                          quint64 videoDrops,
                          quint64 fecRecoveries,
                          quint64 decodeErrors)
    {
        videoStatus_ = videoStatus;
        const quint64 totals[] = {videoPackets, videoFrames, videoDrops, fecRecoveries, decodeErrors};
        for (size_t i = 0; i < sparks_.size(); ++i)
        {
            sparks_[i].sample(totals[i]);
        }
        update();
        publishOverlay();
    }

protected:
    // Focus stays here; on the container it would try to activate the view window, which refuses focus.
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == videoView_ && event->type() == QEvent::FocusIn)
        {
            setFocus(Qt::OtherFocusReason);
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        publishOverlay();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF bounds = rect();
        {
            const QColor sky(18, 24, 31);
            const QColor floor(13, 17, 22);
            painter.fillRect(bounds, sky);

            const float horizonY = static_cast<float>(bounds.height()) * 0.48f + pitch_ * 72.0f;
            const float yawOffset = std::sin(yaw_) * 95.0f;
            const QPointF vanishing(bounds.center().x() + yawOffset, horizonY);

            QPainterPath floorPath;
            floorPath.moveTo(0.0, horizonY);
            floorPath.lineTo(bounds.width(), horizonY);
            floorPath.lineTo(bounds.width(), bounds.height());
            floorPath.lineTo(0.0, bounds.height());
            floorPath.closeSubpath();
            painter.fillPath(floorPath, floor);

            QPen gridPen(QColor(65, 78, 94, 120), 1.0);
            painter.setPen(gridPen);
            for (int i = -8; i <= 8; ++i)
            {
                const float x = static_cast<float>(bounds.center().x()) +
                    static_cast<float>(i) * 42.0f + yawOffset * 0.25f;
                painter.drawLine(QPointF(x, bounds.height()), vanishing);
            }
            for (int i = 0; i < 8; ++i)
            {
                const float t = static_cast<float>(i + 1) / 8.0f;
                const float y = horizonY + (static_cast<float>(bounds.height()) - horizonY) * t * t;
                painter.drawLine(QPointF(0, y), QPointF(bounds.width(), y));
            }

            painter.setPen(QPen(QColor(126, 231, 135, streaming_ ? 230 : 150), 2.0));
            painter.drawLine(QPointF(0, horizonY), QPointF(bounds.width(), horizonY));

            painter.setPen(QColor(242, 244, 248, 220));
            QFont statusFont = painter.font();
            statusFont.setPointSize(statusFont.pointSize() + 1);
            statusFont.setBold(true);
            painter.setFont(statusFont);
            painter.drawText(bounds.adjusted(24, 24, -24, -24),
                             Qt::AlignCenter,
                             videoStatus_.isEmpty() ? "Waiting for video" : videoStatus_);
        }

        drawReticle(painter, reticleCenter(bounds));
        drawPoseBadge(painter, poseBadgeRect(bounds));
        drawCaptureBadge(painter, captureBadgeRect(bounds));
        drawVideoBadge(painter, videoBadgeRect(bounds));
    }

private:
    QPointF reticleCenter(const QRectF& bounds) const
    {
        return QPointF(bounds.center().x() + std::sin(yaw_) * 42.0f, bounds.center().y() - std::sin(pitch_) * 85.0f);
    }

    static QRectF poseBadgeRect(const QRectF& bounds)
    {
        return QRectF(bounds.left() + 14.0, bounds.bottom() - 58.0, 260.0, 42.0);
    }

    static QRectF captureBadgeRect(const QRectF& bounds)
    {
        return QRectF(bounds.right() - 234.0, bounds.top() + 14.0, 220.0, 32.0);
    }

    static QRectF videoBadgeRect(const QRectF& bounds)
    {
        return QRectF(bounds.right() - 374.0, bounds.bottom() - 58.0, 360.0, 42.0);
    }

    static void drawReticle(QPainter& painter, const QPointF& reticle)
    {
        painter.setPen(QPen(QColor(242, 244, 248, 210), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(reticle + QPointF(-16, 0), reticle + QPointF(-4, 0));
        painter.drawLine(reticle + QPointF(4, 0), reticle + QPointF(16, 0));
        painter.drawLine(reticle + QPointF(0, -16), reticle + QPointF(0, -4));
        painter.drawLine(reticle + QPointF(0, 4), reticle + QPointF(0, 16));
        painter.drawEllipse(reticle, 5.0, 5.0);
    }

    void drawPoseBadge(QPainter& painter, const QRectF& badge) const
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(8, 11, 16, 205));
        painter.drawRoundedRect(badge, 8.0, 8.0);
        painter.setPen(QColor(242, 244, 248));
        painter.drawText(badge.adjusted(12, 6, -12, -22),
                         QString("Yaw %1  Pitch %2")
                             .arg(radiansToDegrees(shownYaw_), 0, 'f', 1)
                             .arg(radiansToDegrees(shownPitch_), 0, 'f', 1));
        painter.setPen(QColor(154, 160, 166));
        painter.drawText(badge.adjusted(12, 22, -12, -6),
                         QString("Position %1, %2, %3")
                             .arg(shownPosition_[0], 0, 'f', 2)
                             .arg(shownPosition_[1], 0, 'f', 2)
                             .arg(shownPosition_[2], 0, 'f', 2));
    }

    void drawCaptureBadge(QPainter& painter, const QRectF& badge) const
    {
        painter.setBrush(mouseCaptured_ ? QColor(42, 145, 72, 220) : QColor(52, 59, 68, 220));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(badge, 8.0, 8.0);
        painter.setPen(QColor(242, 244, 248));
        painter.drawText(badge, Qt::AlignCenter, mouseCaptured_ ? "Mouse captured, right-click releases" : "Mouse free");
    }

    void drawVideoBadge(QPainter& painter, const QRectF& badge) const
    {
        painter.setBrush(QColor(8, 11, 16, 205));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(badge, 8.0, 8.0);
        painter.setPen(QColor(242, 244, 248));
        painter.drawText(badge.adjusted(12, 6, -12, -22),
                         videoStatus_.isEmpty() ? "Waiting for video" : videoStatus_);
        static const char* const labels[] = {"pkt", "fps", "drop", "fec", "err"};
        QFont small = painter.font();
        small.setPointSizeF(small.pointSizeF() * 0.8);
        painter.setFont(small);
        const qreal cell = (badge.width() - 24.0) / static_cast<qreal>(sparks_.size());
        qreal labelWidth = 0.0;
        for (const char* label : labels)
        {
            labelWidth = std::max<qreal>(labelWidth, painter.fontMetrics().horizontalAdvance(label));
        }
        for (size_t i = 0; i < sparks_.size(); ++i)
        {
            const QRectF area(badge.left() + 12.0 + cell * static_cast<qreal>(i), badge.top() + 23.0, cell - 6.0, 13.0);
            painter.setPen(QColor(154, 160, 166));
            painter.drawText(area, Qt::AlignLeft | Qt::AlignVCenter, labels[i]);
            sparks_[i].draw(painter, area.adjusted(labelWidth + 4.0, 0.0, 0.0, 0.0), i >= 2);
        }
    }

    // Badges render to opaque images over the backdrop colour; the decoder blits them over the video.
    template <typename Draw>
    PyroWaveDecoder::OverlayBadge renderBadge(const QRectF& badge, qreal dpr, Draw draw) const
    {
        QImage image((badge.size() * dpr).toSize(), QImage::Format_RGBA8888);
        image.setDevicePixelRatio(dpr);
        image.fill(QColor(4, 6, 9));
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setFont(font());
        painter.translate(-badge.topLeft());
        draw(painter, badge);
        painter.end();
        return {(badge.topLeft() * dpr).toPoint(), image};
    }

    // The status changes with every packet, so the overlay is only marked here and drawn once per frame.
    void publishOverlay()
    {
        overlayDirty_ = true;
    }

public:
    void flushOverlay()
    {
        if (!overlayDirty_ || decoder_ == nullptr || videoView_ == nullptr || !videoView_->isVisible())
        {
            return;
        }
        overlayDirty_ = false;
        const qreal dpr = devicePixelRatioF();
        const QRectF bounds = rect();
        std::vector<PyroWaveDecoder::OverlayBadge> badges;
        badges.push_back(renderBadge(poseBadgeRect(bounds), dpr,
                                     [this](QPainter& p, const QRectF& r) { drawPoseBadge(p, r); }));
        badges.push_back(renderBadge(captureBadgeRect(bounds), dpr,
                                     [this](QPainter& p, const QRectF& r) { drawCaptureBadge(p, r); }));
        badges.push_back(renderBadge(videoBadgeRect(bounds), dpr,
                                     [this](QPainter& p, const QRectF& r) { drawVideoBadge(p, r); }));

        // Over video the game already turns the view, so the reticle stays at the centre.
        const QPointF c = bounds.center();
        const QRectF segments[] = {
            {c.x() - 16, c.y() - 0.75, 12, 1.5}, {c.x() + 4, c.y() - 0.75, 12, 1.5},
            {c.x() - 0.75, c.y() - 16, 1.5, 12}, {c.x() - 0.75, c.y() + 4, 1.5, 12},
            {c.x() - 5, c.y() - 5, 10, 1.5},     {c.x() - 5, c.y() + 3.5, 10, 1.5},
            {c.x() - 5, c.y() - 5, 1.5, 10},     {c.x() + 3.5, c.y() - 5, 1.5, 10},
        };
        std::vector<QRect> lines;
        for (const QRectF& segment : segments)
        {
            const QRectF scaled(segment.topLeft() * dpr, segment.size() * dpr);
            lines.push_back(scaled.toAlignedRect());
        }
        decoder_->setOverlay(std::move(badges), std::move(lines));
    }

private:


    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float roll_ = 0.0f;
    float position_[3] = {0.0f, 1.6f, 0.0f};
    float shownYaw_ = 0.0f;
    float shownPitch_ = 0.0f;
    float shownPosition_[3] = {0.0f, 1.6f, 0.0f};
    QWidget* videoView_ = nullptr;
    PyroWaveDecoder* decoder_ = nullptr;
    bool overlayDirty_ = false;
    QString videoStatus_ = "Waiting for video";
    std::array<Sparkline, 5> sparks_;
    bool mouseCaptured_ = false;
    bool streaming_ = false;
};

SimulatorWidget::SimulatorWidget(QWidget* parent)
    : QWidget(parent)
{
    discoverySocket_ = new QUdpSocket(this);
    videoSocket_ = new QUdpSocket(this);
    controlSocket_ = new QUdpSocket(this);
    trackingSocket_ = new QUdpSocket(this);
    trackingTimer_ = new QTimer(this);
    trackingTimer_->setInterval(1000 / 90);
    trackingTimer_->setTimerType(Qt::PreciseTimer);

    buildUi();

    connect(searchButton_, &QPushButton::clicked, this, &SimulatorWidget::startDiscovery);
    connect(connectButton_, &QPushButton::clicked, this, &SimulatorWidget::connectToDiscoveredRuntime);
    connect(disconnectButton_, &QPushButton::clicked, this, &SimulatorWidget::disconnectFromRuntime);
    // Switching to another program releases the mouse, as a 3D game does; clicking the preview takes it back.
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive)
        {
            setMouseCaptured(false, "application inactive");
            resetInputState();
        }
    });
    connect(simulatorFovSlider_, &QSlider::valueChanged, this, [this](int value) {
        simulatorFovDegrees_ = value;
        simulatorFovValueLabel_->setText(QString("%1 deg").arg(value));
        updateTelemetrySummary();
    });
    connect(discoverySocket_, &QUdpSocket::readyRead, this, &SimulatorWidget::readPendingDiscoveryDatagrams);
    connect(videoSocket_, &QUdpSocket::readyRead, this, &SimulatorWidget::readPendingVideoDatagrams);
    connect(trackingTimer_, &QTimer::timeout, this, &SimulatorWidget::sendTrackingSample);
    // Counters and pose text change on every packet; showing them at a fixed rate keeps them readable.
    statsTimer_ = new QTimer(this);
    statsTimer_->setInterval(250);
    connect(statsTimer_, &QTimer::timeout, this, &SimulatorWidget::refreshStats);
    statsTimer_->start();

    poseClock_.start();
    trackingTimer_->start();
    setState(State::Disconnected, "Disconnected");

    // Searches once the event loop starts, so a caller can turn auto-connect off first.
    QTimer::singleShot(0, this, [this]() {
        if (autoConnect_)
        {
            startDiscovery();
        }
    });
}

void SimulatorWidget::setAutoConnect(bool enabled)
{
    autoConnect_ = enabled;
}

void SimulatorWidget::setSnapshotPath(const QString& path)
{
    snapshotPath_ = path;
}

SimulatorWidget::~SimulatorWidget()
{
    trackingTimer_->stop();
    disconnectFromRuntime();
#if OXRSYS_QT_SIMULATOR_HAS_VIDEO
    resetVideoDecoder();
#endif
}

QString SimulatorWidget::stateText() const
{
    switch (state_)
    {
        case State::Disconnected:
            return "Disconnected";
        case State::Discovering:
            return "Searching";
        case State::Discovered:
            return "Runtime discovered";
        case State::Streaming:
            return "Connected";
    }
    return "Unknown";
}

bool SimulatorWidget::isConnected() const
{
    return state_ == State::Streaming;
}

void SimulatorWidget::startDiscovery()
{
    disconnectFromRuntime();
    discoveredServer_ = {};
    serverAddress_ = {};
    trackingPacketsSent_ = 0;

    discoverySocket_->close();
    const bool bound = discoverySocket_->bind(QHostAddress::AnyIPv4,
                                              oxr::protocol::DISCOVERY_PORT,
                                              QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound)
    {
        setState(State::Disconnected,
                 QString("Failed to bind discovery UDP port %1: %2")
                     .arg(oxr::protocol::DISCOVERY_PORT)
                     .arg(discoverySocket_->errorString()));
        return;
    }

    setState(State::Discovering, "Searching for OXRSys runtime...");
    updateServerSummary();
}

void SimulatorWidget::disconnectFromRuntime()
{
    if (state_ == State::Streaming)
    {
        // Frees the runtime's client slot and resumes its announce, as the headset client does on leaving.
        const oxr::protocol::MessageType disconnect = oxr::protocol::MessageType::ServerDisconnect;
        controlSocket_->writeDatagram(reinterpret_cast<const char*>(&disconnect),
                                      static_cast<qint64>(sizeof(disconnect)),
                                      serverAddress_,
                                      oxr::protocol::CONTROL_PORT);
    }
    discoverySocket_->close();
    stopVideoReceiver();
    controlSocket_->close();
    trackingSocket_->close();
    if (state_ != State::Disconnected)
    {
        setState(State::Disconnected, "Disconnected");
    }
    updateTelemetrySummary();
}

void SimulatorWidget::readPendingDiscoveryDatagrams()
{
    while (discoverySocket_->hasPendingDatagrams())
    {
        QByteArray datagram;
        datagram.resize(static_cast<int>(discoverySocket_->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        discoverySocket_->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        Q_UNUSED(senderPort);

        if (datagram.size() < static_cast<int>(sizeof(oxr::protocol::ServerAnnounce)))
        {
            continue;
        }

        oxr::protocol::ServerAnnounce announce = {};
        std::memcpy(&announce, datagram.constData(), sizeof(announce));
        if (announce.type != oxr::protocol::MessageType::ServerAnnounce)
        {
            continue;
        }

        // The runtime keeps announcing while it streams; only a new runtime restarts the connection.
        if (state_ == State::Streaming && sender.isEqual(serverAddress_))
        {
            continue;
        }

        discoveredServer_ = announce;
        serverAddress_ = sender;
        setState(State::Discovered, "Runtime discovered");
        updateServerSummary();
        if (autoConnect_)
        {
            QTimer::singleShot(0, this, &SimulatorWidget::connectToDiscoveredRuntime);
        }
    }
}

void SimulatorWidget::readPendingVideoDatagrams()
{
    while (videoSocket_->hasPendingDatagrams())
    {
        QByteArray datagram;
        datagram.resize(static_cast<int>(videoSocket_->pendingDatagramSize()));
        videoSocket_->readDatagram(datagram.data(), datagram.size());

        if (datagram.size() < static_cast<int>(sizeof(oxr::protocol::VideoPacketHeader)))
        {
            continue;
        }

        oxr::protocol::VideoPacketHeader header = {};
        std::memcpy(&header, datagram.constData(), sizeof(header));
        const char* payload = datagram.constData() + sizeof(header);
        const qsizetype availablePayloadSize = datagram.size() - static_cast<qsizetype>(sizeof(header));
        const qsizetype payloadSize =
            std::min<qsizetype>(static_cast<qsizetype>(header.payloadSize), availablePayloadSize);

        ++videoPacketsReceived_;
        handleVideoPacket(header, payload, payloadSize, monotonicNowNs());
    }
}

void SimulatorWidget::connectToDiscoveredRuntime()
{
    if (state_ != State::Discovered)
    {
        return;
    }

    oxr::protocol::ClientConnect connectPacket = {};
    connectPacket.type = oxr::protocol::MessageType::ClientConnect;
    connectPacket.versionMajor = 1;
    connectPacket.versionMinor = 0;
    connectPacket.preferredCodec = static_cast<uint32_t>(oxr::protocol::VideoCodec::PyroWave);
    connectPacket.maxBitrateMbps = oxr::protocol::CLIENT_MAX_BITRATE_USE_SERVER_CONFIG;
    connectPacket.refreshRateHz = std::max<uint32_t>(discoveredServer_.refreshRateHz, 60);
    const QByteArray deviceName = platformSimulatorDeviceName().toUtf8();
    std::strncpy(connectPacket.deviceName, deviceName.constData(), sizeof(connectPacket.deviceName) - 1);

    if (!startVideoReceiver())
    {
        setState(State::Discovered, "Video receiver unavailable");
        return;
    }

    controlSocket_->writeDatagram(
        reinterpret_cast<const char*>(&connectPacket),
        static_cast<qint64>(sizeof(connectPacket)),
        serverAddress_,
        oxr::protocol::CONTROL_PORT);

    trackingPacketsSent_ = 0;
    updatePreviewStatus();
    setState(State::Streaming, "Connected; sending synthetic head tracking");
    updateTelemetrySummary();
}

void SimulatorWidget::sendTrackingSample()
{
    const qint64 elapsedMilliseconds = poseClock_.restart();
    const float deltaTime = std::clamp(static_cast<float>(elapsedMilliseconds) / 1000.0f,
                                       0.001f,
                                       0.050f);
    advanceSimulation(deltaTime);
    processAssembledVideoFrames(videoAssembler_.expirePendingFrame(
        monotonicNowNs(),
        500'000'000));

    if (previewWidget_ != nullptr)
    {
        previewWidget_->setPose(trackingPose_.yaw,
                                trackingPose_.pitch,
                                trackingPose_.roll,
                                trackingPose_.headPosition,
                                mouseCaptured_,
                                state_ == State::Streaming);
    }

    if (state_ != State::Streaming)
    {
        return;
    }

    oxr::protocol::TrackingPacket packet = {};
    fillTrackingPacket(packet);
    for (const int key : releaseAfterSend_)
    {
        pressedKeys_.remove(key);
    }
    releaseAfterSend_.clear();
    pressedSinceSend_.clear();

    trackingSocket_->writeDatagram(
        reinterpret_cast<const char*>(&packet),
        static_cast<qint64>(sizeof(packet)),
        serverAddress_,
        oxr::protocol::TRACKING_PORT);

    ++trackingPacketsSent_;
    updateTelemetrySummary();
}

void SimulatorWidget::buildUi()
{
    setMinimumSize(720, 520);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet("SimulatorWidget { background: #0b0d10; color: #f2f4f8; }"
                  "QPushButton { padding: 6px 12px; }"
                  "QLabel { color: #f2f4f8; }");

    // Panels in a left column and the view on the right: the eye is nearly square, so a wide window
    // gives the view its full height and the panels the width it cannot use.
    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(14, 14, 14, 14);
    rootLayout->setSpacing(14);

    auto* side = new QWidget(this);
    side->setFixedWidth(340);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(10);

    titleLabel_ = new QLabel("OXRSys Simulator", side);
    titleLabel_->setStyleSheet("font-size: 20px; font-weight: 700;");
    sideLayout->addWidget(titleLabel_);
    hintLabel_ = makeSecondaryLabel("Synthetic desktop client: discover a runtime, connect, and stream head tracking.", side);
    sideLayout->addWidget(hintLabel_);
    statusLabel_ = new QLabel(side);
    statusLabel_->setWordWrap(true);
    statusLabel_->setStyleSheet("font-weight: 600; color: #7ee787;");
    sideLayout->addWidget(statusLabel_);

    auto* buttonLayout = new QHBoxLayout();
    searchButton_ = new QPushButton("Search", side);
    connectButton_ = new QPushButton("Connect", side);
    disconnectButton_ = new QPushButton("Disconnect", side);
    buttonLayout->addWidget(searchButton_);
    buttonLayout->addWidget(connectButton_);
    buttonLayout->addWidget(disconnectButton_);
    sideLayout->addLayout(buttonLayout);

    auto* serverPanel = makePanel(side);
    auto* serverLayout = new QVBoxLayout(serverPanel);
    serverLayout->addWidget(makeSecondaryLabel("Runtime", serverPanel));
    serverLabel_ = new QLabel(serverPanel);
    serverLabel_->setWordWrap(true);
    serverLayout->addWidget(serverLabel_);
    sideLayout->addWidget(serverPanel);

    auto* telemetryPanel = makePanel(side);
    auto* telemetryLayout = new QVBoxLayout(telemetryPanel);
    telemetryLayout->addWidget(makeSecondaryLabel("Tracking", telemetryPanel));
    telemetrySparks_ = new SparklineStrip({"tracking", "video packets", "frames", "drops", "fec", "decode errors"}, telemetryPanel);
    telemetryLayout->addWidget(telemetrySparks_);
    telemetryLabel_ = new QLabel(telemetryPanel);
    telemetryLabel_->setWordWrap(true);
    telemetryLayout->addWidget(telemetryLabel_);
    sideLayout->addWidget(telemetryPanel);

    auto* simulatorPanel = makePanel(side);
    auto* simulatorLayout = new QVBoxLayout(simulatorPanel);
    simulatorLayout->addWidget(makeSecondaryLabel("Simulator", simulatorPanel));
    auto* fovRow = new QHBoxLayout();
    auto* fovLabel = new QLabel("Vertical FOV", simulatorPanel);
    simulatorFovSlider_ = new QSlider(Qt::Horizontal, simulatorPanel);
    simulatorFovSlider_->setRange(60, 150);
    simulatorFovSlider_->setValue(simulatorFovDegrees_);
    simulatorFovValueLabel_ = new QLabel(QString("%1 deg").arg(simulatorFovDegrees_), simulatorPanel);
    simulatorFovValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    simulatorFovValueLabel_->setMinimumWidth(56);
    fovRow->addWidget(fovLabel);
    fovRow->addWidget(simulatorFovSlider_, 1);
    fovRow->addWidget(simulatorFovValueLabel_);
    simulatorLayout->addLayout(fovRow);
    // Off, the headset has no controllers and the dashboard falls back to gaze and the headset button.
    auto* controllers = new QCheckBox("Controllers", simulatorPanel);
    controllers->setChecked(controllersPresent_);
    connect(controllers, &QCheckBox::toggled, this, [this](bool on) { controllersPresent_ = on; });
    simulatorLayout->addWidget(controllers);
    sideLayout->addWidget(simulatorPanel);
    sideLayout->addStretch(1);
    rootLayout->addWidget(side);

    previewWidget_ = new SimulatorPreviewWidget(this);
    previewWidget_->installEventFilter(this);
    rootLayout->addWidget(previewWidget_, 1);
}

void SimulatorWidget::setState(State state, const QString& status)
{
    state_ = state;
    statusLabel_->setText(status);
    updateControls();
    updateTelemetrySummary();
    updatePreviewStatus();
    emit stateTextChanged(stateText());
}

bool SimulatorWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != previewWidget_)
    {
        return QWidget::eventFilter(watched, event);
    }

    switch (event->type())
    {
        case QEvent::MouseButtonPress:
        {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            previewWidget_->setFocus(Qt::MouseFocusReason);
            setFocus(Qt::MouseFocusReason);
            lastMousePosition_ = mouseEvent->position();
            hasLastMousePosition_ = true;
            if (mouseEvent->button() == Qt::RightButton)
            {
                setMouseCaptured(!mouseCaptured_, "right click");
                event->accept();
                return true;
            }
            // With controllers the middle button is the controller's system button, which toggles the
            // dashboard; without them it is the headset button, which opens it and selects by gaze.
            if (mouseEvent->button() == Qt::MiddleButton)
            {
                middleKey_ = controllersPresent_ ? int(Qt::Key_M) : oxrsys::qt_simulator::HeadsetButtonKey;
                setKeyPressed(middleKey_, true);
                event->accept();
                return true;
            }
            if (mouseEvent->button() == Qt::LeftButton && !mouseCaptured_)
            {
                setMouseCaptured(true, "left click");
                event->accept();
                return true;
            }
            if (mouseEvent->button() == Qt::LeftButton)
            {
                setKeyPressed(oxrsys::qt_simulator::TriggerMouseKey, true);
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease:
        {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            lastMousePosition_ = mouseEvent->position();
            hasLastMousePosition_ = true;
            if (mouseEvent->button() == Qt::MiddleButton)
            {
                setKeyPressed(middleKey_, false);
            }
            if (mouseEvent->button() == Qt::LeftButton)
            {
                setKeyPressed(oxrsys::qt_simulator::TriggerMouseKey, false);
            }
            break;
        }
        case QEvent::MouseMove:
        {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (!hasLastMousePosition_)
            {
                lastMousePosition_ = mouseEvent->position();
                hasLastMousePosition_ = true;
                return true;
            }

            const QPointF delta = mouseEvent->position() - lastMousePosition_;
            lastMousePosition_ = mouseEvent->position();
            if (mouseCaptured_ || (mouseEvent->buttons() & Qt::LeftButton))
            {
                accumulateMouseDelta(delta);
            }
            if (mouseCaptured_)
            {
                ++captureMoves_;
                recentreCapturedCursor();
            }
            event->accept();
            return true;
        }
        case QEvent::Wheel:
        {
            auto* wheelEvent = static_cast<QWheelEvent*>(event);
            const float wheelSteps = static_cast<float>(wheelEvent->angleDelta().y()) / 120.0f;
            const float forwardX = -std::sin(trackingPose_.yaw);
            const float forwardZ = -std::cos(trackingPose_.yaw);
            trackingPose_.headPosition[0] += forwardX * wheelSteps * 0.25f;
            trackingPose_.headPosition[2] += forwardZ * wheelSteps * 0.25f;
            event->accept();
            return true;
        }
        case QEvent::KeyPress:
            keyPressEvent(static_cast<QKeyEvent*>(event));
            return true;
        case QEvent::KeyRelease:
            keyReleaseEvent(static_cast<QKeyEvent*>(event));
            return true;
        case QEvent::FocusOut:
            if (!focusStaysInside())
            {
                setMouseCaptured(false, "preview lost focus");
                resetInputState();
            }
            break;
        default:
            break;
    }

    return QWidget::eventFilter(watched, event);
}

void SimulatorWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        setMouseCaptured(false, "escape");
        event->accept();
        return;
    }
    if (!event->isAutoRepeat())
    {
        setKeyPressed(oxrsys::qt_simulator::simulatorKeyIdentifier(*event), true);
    }
    event->accept();
}

void SimulatorWidget::keyReleaseEvent(QKeyEvent* event)
{
    if (!event->isAutoRepeat())
    {
        setKeyPressed(oxrsys::qt_simulator::simulatorKeyIdentifier(*event), false);
    }
    event->accept();
}

void SimulatorWidget::focusOutEvent(QFocusEvent* event)
{
    if (!focusStaysInside())
    {
        setMouseCaptured(false, "simulator lost focus");
        resetInputState();
    }
    QWidget::focusOutEvent(event);
}

// Focus moving between the simulator and its preview must not release capture; leaving the window does.
bool SimulatorWidget::focusStaysInside() const
{
    const QWidget* focused = QApplication::focusWidget();
    return isActiveWindow() && focused != nullptr && (focused == this || isAncestorOf(focused));
}

void SimulatorWidget::updateControls()
{
    searchButton_->setEnabled(state_ == State::Disconnected);
    connectButton_->setEnabled(state_ == State::Discovered);
    disconnectButton_->setEnabled(state_ == State::Discovering ||
                                  state_ == State::Discovered ||
                                  state_ == State::Streaming);
}

void SimulatorWidget::updateServerSummary()
{
    if (state_ == State::Discovering)
    {
        serverLabel_->setText("Waiting for UDP discovery broadcast.");
        return;
    }
    if (state_ == State::Disconnected || serverAddress_.isNull())
    {
        serverLabel_->setText("No runtime discovered.");
        return;
    }

    serverLabel_->setText(QString("%1\n%2\n%3 x %4 @ %5 Hz")
                              .arg(discoveredServerName())
                              .arg(serverAddress_.toString())
                              .arg(discoveredServer_.encodedWidth)
                              .arg(discoveredServer_.encodedHeight)
                              .arg(discoveredServer_.refreshRateHz));
}

void SimulatorWidget::updatePreviewStatus()
{
    previewStatusDirty_ = true;
}

void SimulatorWidget::updateTelemetrySummary()
{
    telemetryDirty_ = true;
}

void SimulatorWidget::refreshStats()
{
    previewStatusDirty_ = false;
    pushPreviewStatus();
    telemetrySparks_->sample({trackingPacketsSent_, videoPacketsReceived_, videoFramesDecoded_, videoFramesDropped_,
                              videoFecRecoveries_, decodeErrors_});
    if (telemetryDirty_)
    {
        telemetryDirty_ = false;
        pushTelemetrySummary();
    }
    if (previewWidget_ != nullptr)
    {
        previewWidget_->latchBadgePose();
    }
}

void SimulatorWidget::pushPreviewStatus()
{
    if (previewWidget_ == nullptr)
    {
        return;
    }

    QString status;
#if !OXRSYS_QT_SIMULATOR_HAS_VIDEO
    status = "Video preview unavailable";
#else
    if (state_ == State::Streaming && videoFramesDecoded_ == 0)
    {
        status = "Waiting for video";
    }
    else if (videoFramesDecoded_ > 0)
    {
        status = "Video";
    }
    else
    {
        status = "Waiting for video";
    }
#endif
    previewWidget_->setStatusOverlay(status,
                                     videoPacketsReceived_,
                                     videoFramesDecoded_,
                                     videoFramesDropped_,
                                     videoFecRecoveries_,
                                     decodeErrors_);
}

void SimulatorWidget::advanceSimulation(float deltaTime)
{
    const QPointF mouseDelta = pendingMouseDelta_;
    pendingMouseDelta_ = {};
    oxrsys::qt_simulator::advanceSimulatorTracking(
        trackingPose_,
        mouseDelta,
        pressedKeys_,
        deltaTime);
}

void SimulatorWidget::fillTrackingPacket(oxr::protocol::TrackingPacket& packet) const
{
    oxrsys::qt_simulator::fillSimulatorTrackingPacket(
        trackingPose_,
        pressedKeys_,
        monotonicNowNs(),
        static_cast<float>(simulatorFovDegrees_),
        simulatorPerEyeAspect(),
        packet,
        controllersPresent_);
}

float SimulatorWidget::simulatorPerEyeAspect() const
{
    if (discoveredServer_.renderWidth > 0 && discoveredServer_.renderHeight > 0)
    {
        return std::max(0.1f,
                        static_cast<float>(discoveredServer_.renderWidth) * 0.5f /
                            static_cast<float>(discoveredServer_.renderHeight));
    }
    if (discoveredServer_.encodedWidth > 0 && discoveredServer_.encodedHeight > 0)
    {
        return std::max(0.1f,
                        static_cast<float>(discoveredServer_.encodedWidth) * 0.5f /
                            static_cast<float>(discoveredServer_.encodedHeight));
    }
    return 1.0f;
}

bool SimulatorWidget::startVideoReceiver()
{
#if !OXRSYS_QT_SIMULATOR_HAS_VIDEO
    videoAssembler_.reset();
    videoPacketsReceived_ = 0;
    videoFramesDecoded_ = 0;
    videoFramesDropped_ = 0;
    videoFecRecoveries_ = 0;
    decodeErrors_ = 0;
    updatePreviewStatus();
    return true;
#else
    stopVideoReceiver();
    videoAssembler_.reset();
    videoPacketsReceived_ = 0;
    videoFramesDecoded_ = 0;
    videoFramesDropped_ = 0;
    videoFecRecoveries_ = 0;
    decodeErrors_ = 0;
    consecutiveDecodeErrors_ = 0;
    lastKeyframeRequestTimeNs_ = 0;
    updatePreviewStatus();

    const bool bound = videoSocket_->bind(QHostAddress::AnyIPv4,
                                          oxr::protocol::VIDEO_PORT,
                                          QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound)
    {
        setState(State::Discovered,
                 QString("Failed to bind video UDP port %1: %2")
                     .arg(oxr::protocol::VIDEO_PORT)
                     .arg(videoSocket_->errorString()));
        return false;
    }
    // Only a bound socket takes the option, and one frame is about 80 datagrams.
    videoSocket_->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 8 * 1024 * 1024);
    return ensureVideoDecoder();
#endif
}

void SimulatorWidget::stopVideoReceiver()
{
    if (videoSocket_ != nullptr)
    {
        videoSocket_->close();
    }
    if (previewWidget_ != nullptr)
    {
        previewWidget_->setVideoActive(false);
    }
    videoAssembler_.reset();
    videoPacketsReceived_ = 0;
    videoFramesDecoded_ = 0;
    videoFramesDropped_ = 0;
    videoFecRecoveries_ = 0;
    decodeErrors_ = 0;
    consecutiveDecodeErrors_ = 0;
    lastKeyframeRequestTimeNs_ = 0;
    updatePreviewStatus();
#if OXRSYS_QT_SIMULATOR_HAS_VIDEO
    resetVideoDecoder();
#endif
}

void SimulatorWidget::handleVideoPacket(const oxr::protocol::VideoPacketHeader& header,
                                        const char* payload,
                                        qsizetype payloadSize,
                                        int64_t receiveTimeNs)
{
    processAssembledVideoFrames(
        videoAssembler_.addPacket(header, payload, payloadSize, receiveTimeNs));
}

void SimulatorWidget::processAssembledVideoFrames(const QList<AssembledVideoFrame>& frames)
{
    const quint64 previousDrops = videoFramesDropped_;
    videoFramesDropped_ = videoAssembler_.droppedFrames();
    videoFecRecoveries_ = videoAssembler_.fecRecoveries();
    if (videoFramesDropped_ > previousDrops)
    {
        sendKeyframeRequest(oxr::protocol::KEYFRAME_REASON_FRAME_LOSS,
                            static_cast<uint32_t>(videoFramesDropped_ - previousDrops));
    }
    if (frames.isEmpty())
    {
        updatePreviewStatus();
        return;
    }

    for (const AssembledVideoFrame& frame : frames)
    {
#if OXRSYS_QT_SIMULATOR_HAS_VIDEO
        const int64_t decodeStartNs = monotonicNowNs();
        if (decodeVideoFrame(frame))
        {
            consecutiveDecodeErrors_ = 0;
            sendLatencyReport(frame, decodeStartNs, monotonicNowNs());
        }
        else
        {
            ++decodeErrors_;
            ++consecutiveDecodeErrors_;
            if (consecutiveDecodeErrors_ >= 3)
            {
                sendKeyframeRequest(oxr::protocol::KEYFRAME_REASON_DECODE_STALL,
                                    static_cast<uint32_t>(consecutiveDecodeErrors_));
                consecutiveDecodeErrors_ = 0;
            }
        }
#else
        Q_UNUSED(frame);
#endif
    }
    updatePreviewStatus();
}

void SimulatorWidget::sendKeyframeRequest(uint32_t reasonFlags, uint32_t detail)
{
    if (state_ != State::Streaming)
    {
        return;
    }

    const uint64_t nowNs = static_cast<uint64_t>(monotonicNowNs());
    constexpr uint64_t CooldownNs = 1'000'000'000;
    if (lastKeyframeRequestTimeNs_ != 0 &&
        nowNs - lastKeyframeRequestTimeNs_ < CooldownNs)
    {
        return;
    }
    lastKeyframeRequestTimeNs_ = nowNs;

    oxr::protocol::RequestKeyframe request = {};
    request.reasonFlags = reasonFlags;
    request.detail = detail;
    controlSocket_->writeDatagram(
        reinterpret_cast<const char*>(&request),
        static_cast<qint64>(sizeof(request)),
        serverAddress_,
        oxr::protocol::CONTROL_PORT);
}

void SimulatorWidget::sendLatencyReport(const AssembledVideoFrame& frame,
                                        int64_t decodeStartNs,
                                        int64_t decodeEndNs)
{
    if (state_ != State::Streaming)
    {
        return;
    }

    oxr::protocol::LatencyReport report = {};
    report.receiveToDecoderSubmitMs =
        static_cast<float>(std::max<int64_t>(0, decodeStartNs - frame.receiveTimeNs)) /
        1'000'000.0f;
    report.decodeLatencyMs =
        static_cast<float>(std::max<int64_t>(0, decodeEndNs - decodeStartNs)) /
        1'000'000.0f;
    report.compositorLatencyMs = 0.0f;
    report.totalClientLatencyMs = report.receiveToDecoderSubmitMs + report.decodeLatencyMs;

    controlSocket_->writeDatagram(
        reinterpret_cast<const char*>(&report),
        static_cast<qint64>(sizeof(report)),
        serverAddress_,
        oxr::protocol::CONTROL_PORT);
}

int64_t SimulatorWidget::monotonicNowNs() const
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clock::now().time_since_epoch())
        .count();
}

bool SimulatorWidget::ensureVideoDecoder()
{
    QString error;
    if (!pyrowave_.initialize(&error))
    {
        setState(State::Discovered, error);
        return false;
    }
    if (!videoViewCreated_ && previewWidget_ != nullptr)
    {
        previewWidget_->setVideoView(QWidget::createWindowContainer(pyrowave_.createView(), previewWidget_),
                                     &pyrowave_);
        videoViewCreated_ = true;
    }
    return true;
}

void SimulatorWidget::resetVideoDecoder()
{
    pyrowave_.reset();
}

bool SimulatorWidget::decodeVideoFrame(const AssembledVideoFrame& frame)
{
    if (!ensureVideoDecoder() || frame.nalUnit.isEmpty())
    {
        return false;
    }
    if (previewWidget_ != nullptr)
    {
        previewWidget_->flushOverlay();
    }
    if (!pyrowave_.decode(frame.nalUnit, frame.presentationTimeNs))
    {
        return false;
    }
    if (previewWidget_ != nullptr)
    {
        previewWidget_->setVideoActive(true);
    }
    const quint64 before = videoFramesDecoded_;
    ++videoFramesDecoded_;
    if (before == 0)
    {
        qInfo("PyroWave: first frame decoded, %dx%d", pyrowave_.decodedSize().width(), pyrowave_.decodedSize().height());
    }
    if (!snapshotPath_.isEmpty() && videoFramesDecoded_ == 90)
    {
        qInfo("PyroWave: snapshot %s", pyrowave_.snapshot().save(snapshotPath_) ? "saved" : "failed");
    }
    return true;
}

void SimulatorWidget::setMouseCaptured(bool captured, const char* reason)
{
    if (mouseCaptured_ == captured)
    {
        return;
    }

    // One span per capture, from the input that took it to the one that released it.
    if (captured)
    {
        captureSpan_.start();
        captureStartReason_ = reason;
        captureMoves_ = 0;
        captureRecentres_ = 0;
    }
    else
    {
        qInfo("span mouse_capture start=\"%s\" end=\"%s\" duration_ms=%lld moves=%d recentres=%d", captureStartReason_,
              reason, static_cast<long long>(captureSpan_.elapsed()), captureMoves_, captureRecentres_);
    }
    mouseCaptured_ = captured;
    hasLastMousePosition_ = false;
    pendingMouseDelta_ = {};
    if (previewWidget_ != nullptr)
    {
        if (mouseCaptured_)
        {
            previewWidget_->grabMouse();
            previewWidget_->setCursor(Qt::BlankCursor);
            previewWidget_->setFocus(Qt::MouseFocusReason);
            recentreCapturedCursor();
        }
        else
        {
            previewWidget_->releaseMouse();
            previewWidget_->setCursor(Qt::CrossCursor);
        }
    }
}

// Captured look warps the cursor back to the preview's centre, so turning never stops at a screen edge.
void SimulatorWidget::recentreCapturedCursor()
{
    if (!isActiveWindow())
    {
        setMouseCaptured(false, "window inactive");
        return;
    }
    const QPoint centre = previewWidget_->rect().center();
    if (lastMousePosition_.toPoint() != centre || !hasLastMousePosition_)
    {
        QCursor::setPos(previewWidget_->mapToGlobal(centre));
        ++captureRecentres_;
    }
    lastMousePosition_ = centre;
    hasLastMousePosition_ = true;
}

void SimulatorWidget::accumulateMouseDelta(const QPointF& delta)
{
    pendingMouseDelta_ += delta;
}

void SimulatorWidget::setKeyPressed(int key, bool pressed)
{
    // A click shorter than one tracking interval still reaches one packet before its release.
    if (pressed)
    {
        pressedKeys_.insert(key);
        pressedSinceSend_.insert(key);
    }
    else if (pressedSinceSend_.contains(key))
    {
        releaseAfterSend_.insert(key);
    }
    else
    {
        pressedKeys_.remove(key);
    }
}

void SimulatorWidget::resetInputState()
{
    pressedKeys_ = pressedSinceSend_;
    releaseAfterSend_ = pressedSinceSend_;
    pendingMouseDelta_ = {};
    hasLastMousePosition_ = false;
}

void SimulatorWidget::pushTelemetrySummary()
{
    if (state_ != State::Streaming)
    {
        telemetryLabel_->setText("Idle. Connect to start 90 Hz synthetic head tracking.");
        return;
    }

    telemetryLabel_->setText(QString("Head pose: %1, %2, %3\nYaw/pitch: %4 / %5 deg\nIPD: 0.064 m  FOV: %6 deg")
                                 .arg(trackingPose_.headPosition[0], 0, 'f', 2)
                                 .arg(trackingPose_.headPosition[1], 0, 'f', 2)
                                 .arg(trackingPose_.headPosition[2], 0, 'f', 2)
                                 .arg(radiansToDegrees(trackingPose_.yaw), 0, 'f', 1)
                                 .arg(radiansToDegrees(trackingPose_.pitch), 0, 'f', 1)
                                 .arg(simulatorFovDegrees_));
}

QString SimulatorWidget::discoveredServerName() const
{
    const QString name = safeServerName(discoveredServer_);
    return name.isEmpty() ? "OXRSys Runtime" : name;
}
