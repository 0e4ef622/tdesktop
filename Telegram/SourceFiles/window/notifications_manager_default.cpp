/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2016 John Preston, https://desktop.telegram.org
*/
#include "stdafx.h"
#include "window/notifications_manager_default.h"

#include "platform/platform_notifications_manager.h"
#include "application.h"
#include "mainwindow.h"
#include "lang.h"
#include "ui/widgets/buttons.h"
#include "dialogs/dialogs_layout.h"
#include "styles/style_dialogs.h"
#include "styles/style_window.h"

namespace Window {
namespace Notifications {
namespace Default {
namespace {

NeverFreedPointer<Manager> ManagerInstance;

int notificationMaxHeight() {
	return st::notifyMinHeight + st::notifyReplyArea.heightMax + st::notifyBorderWidth;
}

QPoint notificationStartPosition() {
	auto r = psDesktopRect();
	auto isLeft = Notify::IsLeftCorner(Global::NotificationsCorner());
	auto isTop = Notify::IsTopCorner(Global::NotificationsCorner());
	auto x = (isLeft == rtl()) ? (r.x() + r.width() - st::notifyWidth - st::notifyDeltaX) : (r.x() + st::notifyDeltaX);
	auto y = isTop ? r.y() : (r.y() + r.height());
	return QPoint(x, y);
}

internal::Widget::Direction notificationShiftDirection() {
	auto isTop = Notify::IsTopCorner(Global::NotificationsCorner());
	return isTop ? internal::Widget::Direction::Down : internal::Widget::Direction::Up;
}

} // namespace

void start() {
	ManagerInstance.createIfNull();
}

Manager *manager() {
	return ManagerInstance.data();
}

void finish() {
	ManagerInstance.clear();
}

Manager::Manager() {
	subscribe(FileDownload::ImageLoaded(), [this] {
		for_const (auto notification, _notifications) {
			notification->updatePeerPhoto();
		}
	});
	subscribe(Global::RefNotifySettingsChanged(), [this](Notify::ChangeType change) {
		settingsChanged(change);
	});
	_inputCheckTimer.setTimeoutHandler([this] { checkLastInput(); });
}

bool Manager::hasReplyingNotification() const {
	for_const (auto notification, _notifications) {
		if (notification->isReplying()) {
			return true;
		}
	}
	return false;
}

void Manager::settingsChanged(Notify::ChangeType change) {
	if (change == Notify::ChangeType::Corner) {
		auto startPosition = notificationStartPosition();
		auto shiftDirection = notificationShiftDirection();
		for_const (auto notification, _notifications) {
			notification->updatePosition(startPosition, shiftDirection);
		}
		if (_hideAll) {
			_hideAll->updatePosition(startPosition, shiftDirection);
		}
	} else if (change == Notify::ChangeType::MaxCount) {
		int allow = Global::NotificationsCount();
		for (int i = _notifications.size(); i != 0;) {
			auto notification = _notifications[--i];
			if (notification->isUnlinked()) continue;
			if (--allow < 0) {
				notification->unlinkHistory();
			}
		}
		if (allow > 0) {
			for (int i = 0; i != allow; ++i) {
				showNextFromQueue();
			}
		}
	} else if (change == Notify::ChangeType::DemoIsShown) {
		auto demoIsShown = Global::NotificationsDemoIsShown();
		_demoMasterOpacity.start([this] { demoMasterOpacityCallback(); }, demoIsShown ? 1. : 0., demoIsShown ? 0. : 1., st::notifyFastAnim);
	}
}

void Manager::demoMasterOpacityCallback() {
	for_const (auto notification, _notifications) {
		notification->updateOpacity();
	}
	if (_hideAll) {
		_hideAll->updateOpacity();
	}
}

float64 Manager::demoMasterOpacity() const {
	return _demoMasterOpacity.current(Global::NotificationsDemoIsShown() ? 0. : 1.);
}

void Manager::checkLastInput() {
	auto replying = hasReplyingNotification();
	auto waiting = false;
	for_const (auto notification, _notifications) {
		if (!notification->checkLastInput(replying)) {
			waiting = true;
		}
	}
	if (waiting) {
		_inputCheckTimer.start(300);
	}
}

void Manager::startAllHiding() {
	if (!hasReplyingNotification()) {
		int notHidingCount = 0;
		for_const (auto notification, _notifications) {
			if (notification->isShowing()) {
				++notHidingCount;
			} else {
				notification->startHiding();
			}
		}
		notHidingCount += _queuedNotifications.size();
		if (_hideAll && notHidingCount < 2) {
			_hideAll->startHiding();
		}
	}
}

void Manager::stopAllHiding() {
	for_const (auto notification, _notifications) {
		notification->stopHiding();
	}
	if (_hideAll) {
		_hideAll->stopHiding();
	}
}

void Manager::showNextFromQueue() {
	if (!_queuedNotifications.isEmpty()) {
		int count = Global::NotificationsCount();
		for_const (auto notification, _notifications) {
			if (notification->isUnlinked()) continue;
			--count;
		}
		if (count > 0) {
			auto startPosition = notificationStartPosition();
			auto startShift = 0;
			auto shiftDirection = notificationShiftDirection();
			do {
				auto queued = _queuedNotifications.front();
				_queuedNotifications.pop_front();

				auto notification = std_::make_unique<Notification>(
					queued.history,
					queued.peer,
					queued.author,
					queued.item,
					queued.forwardedCount,
					startPosition, startShift, shiftDirection);
				Platform::Notifications::defaultNotificationShown(notification.get());
				_notifications.push_back(notification.release());
				--count;
			} while (count > 0 && !_queuedNotifications.isEmpty());

			_positionsOutdated = true;
			checkLastInput();
		}
	}
	if (_positionsOutdated) {
		moveWidgets();
	}
}

void Manager::moveWidgets() {
	auto shift = st::notifyDeltaY;
	int lastShift = 0, lastShiftCurrent = 0, count = 0;
	for (int i = _notifications.size(); i != 0;) {
		auto notification = _notifications[--i];
		if (notification->isUnlinked()) continue;

		notification->changeShift(shift);
		shift += notification->height() + st::notifyDeltaY;

		lastShiftCurrent = notification->currentShift();
		lastShift = shift;

		++count;
	}

	if (count > 1 || !_queuedNotifications.isEmpty()) {
		auto deltaY = st::notifyHideAll.height + st::notifyDeltaY;
		if (!_hideAll) {
			_hideAll = new HideAllButton(notificationStartPosition(), lastShiftCurrent, notificationShiftDirection());
		}
		_hideAll->changeShift(lastShift);
		_hideAll->stopHiding();
	} else if (_hideAll) {
		_hideAll->startHidingFast();
	}
}

void Manager::changeNotificationHeight(Notification *notification, int newHeight) {
	auto deltaHeight = newHeight - notification->height();
	if (!deltaHeight) return;

	notification->addToHeight(deltaHeight);
	auto index = _notifications.indexOf(notification);
	if (index > 0) {
		for (int i = 0; i != index; ++i) {
			auto notification = _notifications[i];
			if (notification->isUnlinked()) continue;

			notification->addToShift(deltaHeight);
		}
	}
	if (_hideAll) {
		_hideAll->addToShift(deltaHeight);
	}
}

void Manager::unlinkFromShown(Notification *remove) {
	if (remove) {
		if (remove->unlinkHistory()) {
			_positionsOutdated = true;
		}
	}
	showNextFromQueue();
}

void Manager::removeFromShown(Notification *remove) {
	if (remove) {
		auto index = _notifications.indexOf(remove);
		if (index >= 0) {
			_notifications.removeAt(index);
			_positionsOutdated = true;
		}
	}
	showNextFromQueue();
}

void Manager::removeHideAll(HideAllButton *remove) {
	if (remove == _hideAll) {
		_hideAll = nullptr;
	}
}
void Manager::doShowNotification(HistoryItem *item, int forwardedCount) {
	_queuedNotifications.push_back(QueuedNotification(item, forwardedCount));
	showNextFromQueue();
}

void Manager::doClearAll() {
	_queuedNotifications.clear();
	for_const (auto notification, _notifications) {
		notification->unlinkHistory();
	}
	showNextFromQueue();
}

void Manager::doClearAllFast() {
	_queuedNotifications.clear();
	auto notifications = base::take(_notifications);
	for_const (auto notification, notifications) {
		delete notification;
	}
	delete base::take(_hideAll);
}

void Manager::doClearFromHistory(History *history) {
	for (auto i = _queuedNotifications.begin(); i != _queuedNotifications.cend();) {
		if (i->history == history) {
			i = _queuedNotifications.erase(i);
		} else {
			++i;
		}
	}
	for_const (auto notification, _notifications) {
		if (notification->unlinkHistory(history)) {
			_positionsOutdated = true;
		}
	}
	showNextFromQueue();
}

void Manager::doClearFromItem(HistoryItem *item) {
	for (auto i = 0, queuedCount = _queuedNotifications.size(); i != queuedCount; ++i) {
		if (_queuedNotifications[i].item == item) {
			_queuedNotifications.removeAt(i);
			break;
		}
	}
	for_const (auto notification, _notifications) {
		// Calls unlinkFromShown() -> showNextFromQueue()
		notification->itemRemoved(item);
	}
}

void Manager::doUpdateAll() {
	for_const (auto notification, _notifications) {
		notification->updateNotifyDisplay();
	}
}

Manager::~Manager() {
	clearAllFast();
}

namespace internal {

Widget::Widget(QPoint startPosition, int shift, Direction shiftDirection) : TWidget(nullptr)
, _opacityDuration(st::notifyFastAnim)
, a_opacity(0, 1)
, a_func(anim::linear)
, _a_opacity(animation(this, &Widget::step_opacity))
, _startPosition(startPosition)
, _direction(shiftDirection)
, a_shift(shift)
, _a_shift(animation(this, &Widget::step_shift)) {
	setWindowOpacity(0.);

	setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint | Qt::BypassWindowManagerHint | Qt::NoDropShadowWindowHint);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_MacAlwaysShowToolWindow);

	_a_opacity.start();
}

void Widget::destroyDelayed() {
	hide();
	if (_deleted) return;
	_deleted = true;

	// Ubuntu has a lag if deleteLater() called immediately.
#if defined Q_OS_LINUX32 || defined Q_OS_LINUX64
	QTimer::singleShot(1000, [this] { delete this; });
#else // Q_OS_LINUX32 || Q_OS_LINUX64
	deleteLater();
#endif // Q_OS_LINUX32 || Q_OS_LINUX64
}

void Widget::step_opacity(float64 ms, bool timer) {
	float64 dt = ms / float64(_opacityDuration);
	if (dt >= 1) {
		a_opacity.finish();
		_a_opacity.stop();
		if (_hiding) {
			destroyDelayed();
		}
	} else {
		a_opacity.update(dt, a_func);
	}
	updateOpacity();
	update();
}

void Widget::step_shift(float64 ms, bool timer) {
	float64 dt = ms / float64(st::notifyFastAnim);
	if (dt >= 1) {
		a_shift.finish();
	} else {
		a_shift.update(dt, anim::linear);
	}
	moveByShift();
}

void Widget::hideSlow() {
	hideAnimated(st::notifySlowHide, anim::easeInCirc);
}

void Widget::hideFast() {
	hideAnimated(st::notifyFastAnim, anim::linear);
}

void Widget::hideStop() {
	if (_hiding) {
		_opacityDuration = st::notifyFastAnim;
		a_func = anim::linear;
		a_opacity.start(1);
		_hiding = false;
		_a_opacity.start();
	}
}

void Widget::hideAnimated(float64 duration, anim::transition func) {
	_opacityDuration = duration;
	a_func = func;
	a_opacity.start(0);
	_hiding = true;
	_a_opacity.start();
}

void Widget::updateOpacity() {
	if (auto manager = ManagerInstance.data()) {
		setWindowOpacity(a_opacity.current() * manager->demoMasterOpacity());
	}
}

void Widget::changeShift(int top) {
	a_shift.start(top);
	_a_shift.start();
}

void Widget::updatePosition(QPoint startPosition, Direction shiftDirection) {
	_startPosition = startPosition;
	_direction = shiftDirection;
	moveByShift();
}

void Widget::addToHeight(int add) {
	auto newHeight = height() + add;
	auto newPosition = computePosition(newHeight);
	updateGeometry(newPosition.x(), newPosition.y(), width(), newHeight);
	psUpdateOverlayed(this);
}

void Widget::updateGeometry(int x, int y, int width, int height) {
	setGeometry(x, y, width, height);
	update();
}

void Widget::addToShift(int add) {
	a_shift.add(add);
	moveByShift();
}

void Widget::moveByShift() {
	move(computePosition(height()));
}

QPoint Widget::computePosition(int height) const {
	auto realShift = a_shift.current();
	if (_direction == Direction::Up) {
		realShift = -realShift - height;
	}
	return QPoint(_startPosition.x(), _startPosition.y() + realShift);
}

Background::Background(QWidget *parent) : TWidget(parent) {
	setAttribute(Qt::WA_OpaquePaintEvent);
}

void Background::paintEvent(QPaintEvent *e) {
	Painter p(this);

	p.fillRect(rect(), st::notificationBg);
	p.fillRect(0, 0, st::notifyBorderWidth, height(), st::notifyBorder);
	p.fillRect(width() - st::notifyBorderWidth, 0, st::notifyBorderWidth, height(), st::notifyBorder);
	p.fillRect(st::notifyBorderWidth, height() - st::notifyBorderWidth, width() - 2 * st::notifyBorderWidth, st::notifyBorderWidth, st::notifyBorder);
}

Notification::Notification(History *history, PeerData *peer, PeerData *author, HistoryItem *msg, int forwardedCount, QPoint startPosition, int shift, Direction shiftDirection) : Widget(startPosition, shift, shiftDirection)
, _history(history)
, _peer(peer)
, _author(author)
, _item(msg)
, _forwardedCount(forwardedCount)
#if defined Q_OS_WIN && !defined Q_OS_WINRT
, _started(GetTickCount())
#endif // Q_OS_WIN && !Q_OS_WINRT
, _close(this, st::notifyClose)
, _reply(this, lang(lng_notification_reply), st::defaultBoxButton) {
	auto position = computePosition(st::notifyMinHeight);
	updateGeometry(position.x(), position.y(), st::notifyWidth, st::notifyMinHeight);

	_userpicLoaded = _peer ? _peer->userpicLoaded() : true;
	updateNotifyDisplay();

	_hideTimer.setSingleShot(true);
	connect(&_hideTimer, SIGNAL(timeout()), this, SLOT(onHideByTimer()));

	_close->setClickedCallback([this] {
		unlinkHistoryInManager();
	});
	_close->setAcceptBoth(true);
	_close->moveToRight(st::notifyClosePos.x(), st::notifyClosePos.y());
	_close->show();

	_reply->setClickedCallback([this] {
		showReplyField();
	});
	_replyPadding = st::notifyMinHeight - st::notifyPhotoPos.y() - st::notifyPhotoSize;
	_reply->moveToRight(_replyPadding, height() - _reply->height() - _replyPadding);
	_reply->hide();

	prepareActionsCache();

	show();
}

void Notification::prepareActionsCache() {
	auto replyCache = myGrab(_reply);
	auto fadeWidth = st::notifyFadeRight.width();
	auto actionsTop = st::notifyTextTop + st::msgNameFont->height;
	auto actionsCacheWidth = _reply->width() + _replyPadding + fadeWidth;
	auto actionsCacheHeight = height() - actionsTop;
	auto actionsCacheImg = QImage(actionsCacheWidth * cIntRetinaFactor(), actionsCacheHeight * cIntRetinaFactor(), QImage::Format_ARGB32_Premultiplied);
	actionsCacheImg.setDevicePixelRatio(cRetinaFactor());
	actionsCacheImg.fill(Qt::transparent);
	{
		Painter p(&actionsCacheImg);
		st::notifyFadeRight.fill(p, rtlrect(0, 0, fadeWidth, actionsCacheHeight, actionsCacheWidth));
		p.fillRect(rtlrect(fadeWidth, 0, actionsCacheWidth - fadeWidth, actionsCacheHeight, actionsCacheWidth), st::notificationBg);
		p.drawPixmapRight(_replyPadding, _reply->y() - actionsTop, actionsCacheWidth, replyCache);
	}
	_buttonsCache = App::pixmapFromImageInPlace(std_::move(actionsCacheImg));
}

bool Notification::checkLastInput(bool hasReplyingNotifications) {
	if (!_waitingForInput) return true;

	auto wasUserInput = true; // TODO
#if defined Q_OS_WIN && !defined Q_OS_WINRT
	LASTINPUTINFO lii;
	lii.cbSize = sizeof(LASTINPUTINFO);
	BOOL res = GetLastInputInfo(&lii);
	wasUserInput = (!res || lii.dwTime >= _started);
#endif // Q_OS_WIN && !Q_OS_WINRT
	if (wasUserInput) {
		_waitingForInput = false;
		if (!hasReplyingNotifications) {
			_hideTimer.start(st::notifyWaitLongHide);
		}
		return true;
	}
	return false;
}

void Notification::onReplyResize() {
	changeHeight(st::notifyMinHeight + _replyArea->height() + st::notifyBorderWidth);
}

void Notification::onReplySubmit(bool ctrlShiftEnter) {
	sendReply();
}

void Notification::onReplyCancel() {
	unlinkHistoryInManager();
}

void Notification::updateGeometry(int x, int y, int width, int height) {
	if (height > st::notifyMinHeight) {
		if (!_background) {
			_background = new Background(this);
		}
		_background->setGeometry(0, st::notifyMinHeight, width, height - st::notifyMinHeight);
	} else if (_background) {
		_background.destroy();
	}
	Widget::updateGeometry(x, y, width, height);
}

void Notification::paintEvent(QPaintEvent *e) {
	Painter p(this);
	p.setClipRect(e->rect());
	p.drawPixmap(0, 0, _cache);

	auto buttonsLeft = st::notifyPhotoPos.x() + st::notifyPhotoSize + st::notifyTextLeft;
	auto buttonsTop = st::notifyTextTop + st::msgNameFont->height;
	if (a_actionsOpacity.animating(getms())) {
		p.setOpacity(a_actionsOpacity.current());
		p.drawPixmapRight(0, buttonsTop, width(), _buttonsCache);
	} else if (_actionsVisible) {
		p.drawPixmapRight(0, buttonsTop, width(), _buttonsCache);
	}
}

void Notification::actionsOpacityCallback() {
	update();
	if (!a_actionsOpacity.animating() && _actionsVisible) {
		_reply->show();
	}
}

void Notification::updateNotifyDisplay() {
	if (!_history || !_peer || (!_item && _forwardedCount < 2)) return;

	auto options = Manager::getNotificationOptions(_item);
	_hideReplyButton = options.hideReplyButton;

	int32 w = width(), h = height();
	QImage img(w * cIntRetinaFactor(), h * cIntRetinaFactor(), QImage::Format_ARGB32_Premultiplied);
	if (cRetina()) img.setDevicePixelRatio(cRetinaFactor());
	img.fill(st::notificationBg->c);

	{
		Painter p(&img);
		p.fillRect(0, 0, w - st::notifyBorderWidth, st::notifyBorderWidth, st::notifyBorder);
		p.fillRect(w - st::notifyBorderWidth, 0, st::notifyBorderWidth, h - st::notifyBorderWidth, st::notifyBorder);
		p.fillRect(st::notifyBorderWidth, h - st::notifyBorderWidth, w - st::notifyBorderWidth, st::notifyBorderWidth, st::notifyBorder);
		p.fillRect(0, st::notifyBorderWidth, st::notifyBorderWidth, h - st::notifyBorderWidth, st::notifyBorder);

		if (!options.hideNameAndPhoto) {
			_history->peer->loadUserpic(true, true);
			_history->peer->paintUserpicLeft(p, st::notifyPhotoSize, st::notifyPhotoPos.x(), st::notifyPhotoPos.y(), width());
		} else {
			static QPixmap icon = App::pixmapFromImageInPlace(App::wnd()->iconLarge().scaled(st::notifyPhotoSize, st::notifyPhotoSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
			icon.setDevicePixelRatio(cRetinaFactor());
			p.drawPixmap(st::notifyPhotoPos.x(), st::notifyPhotoPos.y(), icon);
		}

		int32 itemWidth = w - st::notifyPhotoPos.x() - st::notifyPhotoSize - st::notifyTextLeft - st::notifyClosePos.x() - st::notifyClose.width;

		QRect rectForName(st::notifyPhotoPos.x() + st::notifyPhotoSize + st::notifyTextLeft, st::notifyTextTop, itemWidth, st::msgNameFont->height);
		if (!options.hideNameAndPhoto) {
			if (auto chatTypeIcon = Dialogs::Layout::ChatTypeIcon(_history->peer, false, false)) {
				chatTypeIcon->paint(p, rectForName.topLeft(), w);
				rectForName.setLeft(rectForName.left() + st::dialogsChatTypeSkip);
			}
		}

		if (!options.hideMessageText) {
			const HistoryItem *textCachedFor = 0;
			Text itemTextCache(itemWidth);
			QRect r(st::notifyPhotoPos.x() + st::notifyPhotoSize + st::notifyTextLeft, st::notifyItemTop + st::msgNameFont->height, itemWidth, 2 * st::dialogsTextFont->height);
			if (_item) {
				auto active = false, selected = false;
				_item->drawInDialog(p, r, active, selected, textCachedFor, itemTextCache);
			} else if (_forwardedCount > 1) {
				p.setFont(st::dialogsTextFont);
				if (_author) {
					itemTextCache.setText(st::dialogsTextFont, _author->name);
					p.setPen(st::dialogsTextFgService);
					itemTextCache.drawElided(p, r.left(), r.top(), r.width(), st::dialogsTextFont->height);
					r.setTop(r.top() + st::dialogsTextFont->height);
				}
				p.setPen(st::dialogsTextFg);
				p.drawText(r.left(), r.top() + st::dialogsTextFont->ascent, lng_forward_messages(lt_count, _forwardedCount));
			}
		} else {
			static QString notifyText = st::dialogsTextFont->elided(lang(lng_notification_preview), itemWidth);
			p.setFont(st::dialogsTextFont);
			p.setPen(st::dialogsTextFgService);
			p.drawText(st::notifyPhotoPos.x() + st::notifyPhotoSize + st::notifyTextLeft, st::notifyItemTop + st::msgNameFont->height + st::dialogsTextFont->ascent, notifyText);
		}

		p.setPen(st::dialogsNameFg);
		if (!options.hideNameAndPhoto) {
			_history->peer->dialogName().drawElided(p, rectForName.left(), rectForName.top(), rectForName.width());
		} else {
			p.setFont(st::msgNameFont);
			static QString notifyTitle = st::msgNameFont->elided(qsl("Telegram Desktop"), rectForName.width());
			p.drawText(rectForName.left(), rectForName.top() + st::msgNameFont->ascent, notifyTitle);
		}
	}

	_cache = App::pixmapFromImageInPlace(std_::move(img));
	if (!canReply()) {
		toggleActionButtons(false);
	}
	update();
}

void Notification::updatePeerPhoto() {
	if (_userpicLoaded || !_peer || !_peer->userpicLoaded()) {
		return;
	}
	_userpicLoaded = true;

	auto img = _cache.toImage();
	{
		Painter p(&img);
		_peer->paintUserpicLeft(p, st::notifyPhotoSize, st::notifyPhotoPos.x(), st::notifyPhotoPos.y(), width());
	}
	_cache = App::pixmapFromImageInPlace(std_::move(img));
	update();
}

void Notification::itemRemoved(HistoryItem *deleted) {
	if (_item && _item == deleted) {
		_item = nullptr;
		unlinkHistoryInManager();
	}
}

bool Notification::canReply() const {
	return !_hideReplyButton && (_item != nullptr) && !App::passcoded() && (Global::NotifyView() <= dbinvShowPreview);
}

void Notification::unlinkHistoryInManager() {
	if (auto manager = ManagerInstance.data()) {
		manager->unlinkFromShown(this);
	}
}

void Notification::toggleActionButtons(bool visible) {
	if (_actionsVisible != visible) {
		_actionsVisible = visible;
		a_actionsOpacity.start([this] { actionsOpacityCallback(); }, _actionsVisible ? 0. : 1., _actionsVisible ? 1. : 0., st::notifyActionsDuration);
		_reply->hide();
	}
}

void Notification::showReplyField() {
	activateWindow();

	if (_replyArea) {
		_replyArea->setFocus();
		return;
	}
	stopHiding();

	_background = new Background(this);
	_background->setGeometry(0, st::notifyMinHeight, width(), st::notifySendReply.height + st::notifyBorderWidth);
	_background->show();

	_replyArea = new InputArea(this, st::notifyReplyArea, lang(lng_message_ph), QString());
	_replyArea->resize(width() - st::notifySendReply.width - 2 * st::notifyBorderWidth, st::notifySendReply.height);
	_replyArea->moveToLeft(st::notifyBorderWidth, st::notifyMinHeight);
	_replyArea->show();
	_replyArea->setFocus();
	_replyArea->setMaxLength(MaxMessageSize);
	_replyArea->setCtrlEnterSubmit(CtrlEnterSubmitBoth);

	// Catch mouse press event to activate the window.
	Sandbox::installEventFilter(this);
	connect(_replyArea, SIGNAL(resized()), this, SLOT(onReplyResize()));
	connect(_replyArea, SIGNAL(submitted(bool)), this, SLOT(onReplySubmit(bool)));
	connect(_replyArea, SIGNAL(cancelled()), this, SLOT(onReplyCancel()));

	_replySend = new Ui::IconButton(this, st::notifySendReply);
	_replySend->moveToRight(st::notifyBorderWidth, st::notifyMinHeight);
	_replySend->show();
	_replySend->setClickedCallback([this] { sendReply(); });

	toggleActionButtons(false);

	onReplyResize();
	update();
}

void Notification::sendReply() {
	if (!_history) return;

	if (auto manager = ManagerInstance.data()) {
		auto peerId = _history->peer->id;
		auto msgId = _item ? _item->id : ShowAtUnreadMsgId;
		manager->notificationReplied(peerId, msgId, _replyArea->getLastText());

		manager->startAllHiding();
	}
}

void Notification::changeHeight(int newHeight) {
	if (auto manager = ManagerInstance.data()) {
		manager->changeNotificationHeight(this, newHeight);
	}
}

bool Notification::unlinkHistory(History *history) {
	auto unlink = _history && (history == _history || !history);
	if (unlink) {
		hideFast();
		_history = nullptr;
		_item = nullptr;
	}
	return unlink;
}

void Notification::enterEvent(QEvent *e) {
	if (!_history) return;
	if (auto manager = ManagerInstance.data()) {
		manager->stopAllHiding();
	}
	if (!_replyArea && canReply()) {
		toggleActionButtons(true);
	}
}

void Notification::leaveEvent(QEvent *e) {
	if (!_history) return;
	if (auto manager = ManagerInstance.data()) {
		manager->startAllHiding();
	}
	toggleActionButtons(false);
}

void Notification::startHiding() {
	if (!_history) return;
	hideSlow();
}

void Notification::mousePressEvent(QMouseEvent *e) {
	if (!_history) return;

	if (e->button() == Qt::RightButton) {
		unlinkHistoryInManager();
	} else {
		e->ignore();
		if (auto manager = ManagerInstance.data()) {
			auto peerId = _history->peer->id;
			auto msgId = _item ? _item->id : ShowAtUnreadMsgId;
			manager->notificationActivated(peerId, msgId);
		}
	}
}

bool Notification::eventFilter(QObject *o, QEvent *e) {
	if (e->type() == QEvent::MouseButtonPress) {
		if (auto receiver = qobject_cast<QWidget*>(o)) {
			if (isAncestorOf(receiver)) {
				activateWindow();
			}
		}
	}
	return false;
}

void Notification::stopHiding() {
	if (!_history) return;
	_hideTimer.stop();
	Widget::hideStop();
}

void Notification::onHideByTimer() {
	startHiding();
}

Notification::~Notification() {
	if (auto manager = ManagerInstance.data()) {
		manager->removeFromShown(this);
	}
}

HideAllButton::HideAllButton(QPoint startPosition, int shift, Direction shiftDirection) : Widget(startPosition, shift, shiftDirection) {
	setCursor(style::cur_pointer);

	auto position = computePosition(st::notifyHideAll.height);
	updateGeometry(position.x(), position.y(), st::notifyWidth, st::notifyHideAll.height);
	hide();
	createWinId();

	show();
}

void HideAllButton::startHiding() {
	hideSlow();
}

void HideAllButton::startHidingFast() {
	hideFast();
}

void HideAllButton::stopHiding() {
	hideStop();
}

HideAllButton::~HideAllButton() {
	if (auto manager = ManagerInstance.data()) {
		manager->removeHideAll(this);
	}
}

void HideAllButton::enterEvent(QEvent *e) {
	_mouseOver = true;
	update();
}

void HideAllButton::leaveEvent(QEvent *e) {
	_mouseOver = false;
	update();
}

void HideAllButton::mousePressEvent(QMouseEvent *e) {
	_mouseDown = true;
}

void HideAllButton::mouseReleaseEvent(QMouseEvent *e) {
	auto mouseDown = base::take(_mouseDown);
	if (mouseDown && _mouseOver) {
		if (auto manager = ManagerInstance.data()) {
			manager->clearAll();
		}
	}
}

void HideAllButton::paintEvent(QPaintEvent *e) {
	Painter p(this);
	p.setClipRect(e->rect());

	p.fillRect(rect(), _mouseOver ? st::notifyHideAll.textBgOver : st::notifyHideAll.textBg);
	p.fillRect(0, 0, width(), st::notifyBorderWidth, st::notifyBorder);
	p.fillRect(0, height() - st::notifyBorderWidth, width(), st::notifyBorderWidth, st::notifyBorder);
	p.fillRect(0, st::notifyBorderWidth, st::notifyBorderWidth, height() - 2 * st::notifyBorderWidth, st::notifyBorder);
	p.fillRect(width() - st::notifyBorderWidth, st::notifyBorderWidth, st::notifyBorderWidth, height() - 2 * st::notifyBorderWidth, st::notifyBorder);

	p.setFont(st::defaultLinkButton.font);
	p.setPen(st::defaultLinkButton.color);
	p.drawText(rect(), lang(lng_notification_hide_all), style::al_center);
}

} // namespace internal
} // namespace Default
} // namespace Notifications
} // namespace Window
