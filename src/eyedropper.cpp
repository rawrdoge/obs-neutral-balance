/*
 * obs-neutral-balance - manual white balance correction filter for OBS Studio
 * Copyright (C) 2026 obs-neutral-balance contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Frozen-frame eyedropper dialog (PRD FR-08 .. FR-15).
 *
 * OBS's built-in color picker is a modal desktop dialog and cannot sample the
 * video preview, so the filter captures one frame of its own input on the
 * render thread and hands it here. This dialog only ever touches a CPU-side
 * copy - no GPU access happens on the UI thread.
 *
 * Without Qt (NB_HAVE_QT undefined) this file compiles to stub
 * implementations so the plugin still builds and the automatic methods work.
 */

#include "eyedropper.h"

#ifdef NB_HAVE_QT

#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

/* Widget that displays the frozen frame with a crosshair, a magnified loupe
 * and click-to-sample behaviour. Frame coordinates are kept separate from
 * widget coordinates; the frame is scaled to fit while preserving aspect. */
class SampleArea : public QWidget {
	Q_OBJECT

public:
	SampleArea(const QImage &frame, int radius, QWidget *parent = nullptr)
		: QWidget(parent), frame_(frame), radius_(radius)
	{
		setMouseTracking(true);
		setMinimumSize(640, 360);
		setCursor(Qt::CrossCursor);
	}

	void setRadius(int radius) { radius_ = radius; }
	bool hasSelection() const { return has_selection_; }
	void selectionRgb(float rgb[3]) const
	{
		rgb[0] = selection_[0];
		rgb[1] = selection_[1];
		rgb[2] = selection_[2];
	}

signals:
	void selectionChanged(float r, float g, float b);

protected:
	void paintEvent(QPaintEvent *event) override
	{
		Q_UNUSED(event);
		QPainter p(this);
		p.fillRect(rect(), Qt::black);

		const QRectF target = frameTarget();
		if (frame_.isNull() || target.isEmpty())
			return;

		p.drawImage(target, frame_);

		if (!cursor_valid_)
			return;

		const QPointF widget_pos = frameToWidget(cursor_);

		/* Crosshair */
		p.setPen(QPen(QColor(255, 64, 64), 1));
		p.drawLine(QPointF(0.0, widget_pos.y()), QPointF(width(), widget_pos.y()));
		p.drawLine(QPointF(widget_pos.x(), 0.0), QPointF(widget_pos.x(), height()));

		drawLoupe(p);
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		cursor_ = widgetToFrame(event->pos());
		cursor_valid_ = frameRect().contains(cursor_);
		update();
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton)
			return;
		const QPoint pos = widgetToFrame(event->pos());
		if (!frameRect().contains(pos))
			return;

		averageAt(pos, selection_);
		has_selection_ = true;
		emit selectionChanged(selection_[0], selection_[1], selection_[2]);
		update();
	}

	void leaveEvent(QEvent *event) override
	{
		Q_UNUSED(event);
		cursor_valid_ = false;
		update();
	}

private:
	QRect frameRect() const { return QRect(QPoint(0, 0), frame_.size()); }

	QRectF frameTarget() const
	{
		if (frame_.isNull())
			return QRectF();
		const qreal s = qMin(qreal(width()) / frame_.width(), qreal(height()) / frame_.height());
		const QSizeF disp(frame_.width() * s, frame_.height() * s);
		return QRectF(QPointF((width() - disp.width()) / 2.0, (height() - disp.height()) / 2.0), disp);
	}

	QPoint widgetToFrame(const QPoint &pos) const
	{
		const QRectF target = frameTarget();
		return QPoint(int((pos.x() - target.x()) * frame_.width() / target.width()),
			      int((pos.y() - target.y()) * frame_.height() / target.height()));
	}

	QPointF frameToWidget(const QPoint &pos) const
	{
		const QRectF target = frameTarget();
		return QPointF(target.x() + pos.x() * target.width() / frame_.width(),
			       target.y() + pos.y() * target.height() / frame_.height());
	}

	void averageAt(const QPoint &pos, float rgb[3]) const
	{
		const QRect area = QRect(pos.x() - radius_, pos.y() - radius_, 2 * radius_ + 1, 2 * radius_ + 1)
					   .intersected(frameRect());
		double r = 0.0, g = 0.0, b = 0.0;
		const qint64 n = qint64(area.width()) * area.height();
		if (n <= 0) {
			rgb[0] = rgb[1] = rgb[2] = 0.0f;
			return;
		}
		for (int y = area.top(); y <= area.bottom(); y++) {
			for (int x = area.left(); x <= area.right(); x++) {
				const QRgb px = frame_.pixel(x, y);
				r += qRed(px);
				g += qGreen(px);
				b += qBlue(px);
			}
		}
		rgb[0] = float(r / n / 255.0);
		rgb[1] = float(g / n / 255.0);
		rgb[2] = float(b / n / 255.0);
	}

	void drawLoupe(QPainter &p) const
	{
		const int src = 9;   /* source pixels per loupe side (odd) */
		const int zoom = 14; /* magnification factor */

		QRect src_rect(cursor_.x() - src / 2, cursor_.y() - src / 2, src, src);
		src_rect = src_rect.intersected(frameRect());
		if (src_rect.isEmpty())
			return;

		QImage zoomed = frame_.copy(src_rect).scaled(src * zoom, src * zoom, Qt::IgnoreAspectRatio,
							     Qt::FastTransformation);
		const QRect loupe(width() - src * zoom - 16, 16, src * zoom, src * zoom);

		p.fillRect(loupe.adjusted(-3, -3, 3, 3), QColor(0, 0, 0, 200));
		p.drawImage(loupe, zoomed);

		/* Center marker of the loupe marks the sampled pixel. */
		p.setPen(QPen(QColor(255, 64, 64), 1));
		const QPoint c = loupe.center();
		p.drawLine(c + QPoint(-6, 0), c + QPoint(6, 0));
		p.drawLine(c + QPoint(0, -6), c + QPoint(0, 6));

		/* Sampled/hovered average under the loupe (PRD FR-13 feedback). */
		float rgb[3];
		averageAt(cursor_, rgb);
		p.setPen(Qt::white);
		p.drawText(QPoint(loupe.left(), loupe.bottom() + 16),
			   QStringLiteral("avg R %1 G %2 B %3")
				   .arg(int(rgb[0] * 255.0))
				   .arg(int(rgb[1] * 255.0))
				   .arg(int(rgb[2] * 255.0)));
	}

	QImage frame_;
	QPoint cursor_;
	bool cursor_valid_ = false;
	int radius_;
	bool has_selection_ = false;
	float selection_[3] = {0.0f, 0.0f, 0.0f};
};

} // namespace

bool nb_eyedropper_available(void)
{
	return true;
}

bool nb_eyedropper_sample_rgba(const uint8_t *data, uint32_t width, uint32_t height, int radius, float out_rgb[3])
{
	QImage frame = QImage(data, int(width), int(height), int(width) * 4, QImage::Format_RGBA8888).copy();
	if (frame.isNull())
		return false;

	QDialog dialog;
	dialog.setWindowTitle(QStringLiteral("Pick Neutral — click a neutral area"));

	auto *layout = new QVBoxLayout(&dialog);
	auto *hint = new QLabel(QStringLiteral(
		"Click a gray or white area of this frozen frame. The sampled area is mapped to the "
		"target neutral; adjust hue and luminance afterwards to bias the correction."),
				&dialog);
	hint->setWordWrap(true);
	layout->addWidget(hint);

	auto *area = new SampleArea(frame, radius, &dialog);
	layout->addWidget(area, 1);

	auto *info = new QLabel(QStringLiteral("No sample selected."), &dialog);
	layout->addWidget(info);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
	layout->addWidget(buttons);

	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(area, &SampleArea::selectionChanged, &dialog,
			 [info, buttons](float r, float g, float b) {
				 info->setText(QStringLiteral("Sample: R %1  G %2  B %3").arg(int(r * 255.0)).arg(int(g * 255.0)).arg(
					 int(b * 255.0)));
				 buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
			 });

	if (dialog.exec() != QDialog::Accepted || !area->hasSelection())
		return false;

	area->selectionRgb(out_rgb);
	return true;
}

#include "eyedropper.moc"

#else /* !NB_HAVE_QT */

bool nb_eyedropper_available(void)
{
	return false;
}

bool nb_eyedropper_sample_rgba(const uint8_t *data, uint32_t width, uint32_t height, int radius, float out_rgb[3])
{
	(void)data;
	(void)width;
	(void)height;
	(void)radius;
	(void)out_rgb;
	return false;
}

#endif /* NB_HAVE_QT */
