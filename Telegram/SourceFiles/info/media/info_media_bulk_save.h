/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/base_file_utilities.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_shared_media.h"
#include "data/data_file_origin.h"
#include "history/history_item.h"
#include "history/view/history_view_context_menu.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_info.h"
#include "styles/style_layers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace Info::Media::BulkSave {

using Type = Storage::SharedMediaType;

struct Scope {
	PeerId peerId = 0;
	MsgId topicRootId = 0;
	PeerId monoforumPeerId = 0;
	PeerId migratedPeerId = 0;
	Type type = Type::PhotoVideo;
	QString destination;
};

struct DownloadProgress {
	QString name;
	int64 ready = 0;
	int64 total = 0;
	float64 progress = 0.;
	bool waitingForExisting = false;
};

struct Failure {
	QString name;
	QString reason;
};

struct Progress {
	int discovered = 0;
	int saved = 0;
	int skipped = 0;
	int failed = 0;
	std::optional<int> total;
	std::vector<DownloadProgress> active;
	std::vector<Failure> failures;
	std::vector<QString> recentlySaved;
	QString lastSavedPath;
	bool stopping = false;
	bool finished = false;
	bool cancelled = false;
};

[[nodiscard]] inline QString DestinationText(const QString &path) {
	return u"Saving to %1"_q.arg(QDir::toNativeSeparators(path));
}

[[nodiscard]] inline QString ProgressTitle(const Progress &progress) {
	if (progress.finished) {
		if (!progress.saved && progress.failed) {
			return u"No media was saved"_q;
		}
		return progress.cancelled
			? u"Stopped saving media"_q
			: u"Finished saving media"_q;
	}
	if (progress.stopping) {
		return u"Finishing active downloads…"_q;
	}
	if (progress.total) {
		return u"Saving %1 of %2 items"_q
			.arg(progress.saved)
			.arg(*progress.total);
	}
	return progress.discovered
		? u"Saving %1 items"_q.arg(progress.saved)
		: u"Finding media…"_q;
}

[[nodiscard]] inline QString ProgressDetails(const Progress &progress) {
	auto details = QString();
	if (!progress.finished && !progress.active.empty()) {
		details = u"%1 downloading"_q.arg(progress.active.size());
	} else if (!progress.finished && !progress.stopping) {
		details = u"%1 discovered"_q.arg(progress.discovered);
	}
	if (progress.skipped || progress.failed) {
		if (!details.isEmpty()) {
			details += u" • "_q;
		}
		details += u"%1 skipped • %2 failed"_q
			.arg(progress.skipped)
			.arg(progress.failed);
	}
	return details;
}

class ProgressWidget final : public Ui::RpWidget {
public:
	explicit ProgressWidget(QWidget *parent, QString destination)
	: RpWidget(parent)
	, _destination(std::move(destination)) {
	}

	void setProgress(Progress progress) {
		_progress = std::move(progress);
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (newWidth <= 0) {
			return 0;
		}
		const auto rows = std::min(int(_progress.active.size()), kVisibleRows);
		const auto overflow = int(_progress.active.size()) > kVisibleRows;
		const auto recent = std::min(
			int(_progress.recentlySaved.size()),
			kVisibleRecent);
		return st::infoBulkSaveTitleHeight
			+ st::infoBulkSaveDetailsHeight
			+ st::infoBulkSaveProgressSkip
			+ st::infoBulkSaveProgressHeight
			+ st::infoBulkSaveDestinationSkip
			+ st::infoBulkSaveDestinationHeight
			+ (rows ? (st::infoBulkSaveActiveTitleSkip
				+ st::infoBulkSaveActiveTitleHeight
				+ rows * st::infoBulkSaveActiveRowHeight
				+ (rows - 1) * st::infoBulkSaveActiveRowSkip) : 0)
			+ (overflow ? st::infoBulkSaveOverflowHeight : 0)
			+ (recent ? (st::infoBulkSaveRecentTitleSkip
				+ st::infoBulkSaveRecentTitleHeight
				+ recent * st::infoBulkSaveRecentRowHeight) : 0);
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		auto top = 0;
		p.setFont(st::semiboldFont);
		p.setPen(st::windowBoldFg);
		p.drawTextLeft(0, top, width(), ProgressTitle(_progress));
		top += st::infoBulkSaveTitleHeight;

		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawTextLeft(0, top, width(), ProgressDetails(_progress));
		top += st::infoBulkSaveDetailsHeight + st::infoBulkSaveProgressSkip;

		paintProgress(p, top, aggregateProgress());
		top += st::infoBulkSaveProgressHeight
			+ st::infoBulkSaveDestinationSkip;

		p.drawTextLeft(
			0,
			top,
			width(),
			st::normalFont->elided(DestinationText(_destination), width()));
		top += st::infoBulkSaveDestinationHeight;

		const auto count = std::min(int(_progress.active.size()), kVisibleRows);
		if (count) {
			top += st::infoBulkSaveActiveTitleSkip;
			p.setFont(st::semiboldFont);
			p.setPen(st::windowBoldFg);
			p.drawTextLeft(0, top, width(), u"Downloading"_q);
			top += st::infoBulkSaveActiveTitleHeight;

			for (auto i = 0; i != count; ++i) {
				paintActive(p, top, _progress.active[i]);
				top += st::infoBulkSaveActiveRowHeight;
				if (i + 1 != count) {
					top += st::infoBulkSaveActiveRowSkip;
				}
			}
			const auto overflow = int(_progress.active.size()) - count;
			if (overflow > 0) {
				p.setFont(st::normalFont);
				p.setPen(st::windowSubTextFg);
				p.drawTextLeft(
					0,
					top,
					width(),
					u"+ %1 more downloading"_q.arg(overflow));
				top += st::infoBulkSaveOverflowHeight;
			}
		}
		paintRecentlySaved(p, top);
	}

private:
	static constexpr auto kVisibleRows = 4;
	static constexpr auto kVisibleRecent = 2;

	[[nodiscard]] std::optional<float64> aggregateProgress() const {
		if (!_progress.total || !*_progress.total) {
			return std::nullopt;
		}
		return std::clamp(
			float64(
				_progress.saved + _progress.skipped + _progress.failed)
				/ *_progress.total,
			0.,
			1.);
	}

	[[nodiscard]] std::optional<float64> itemProgress(
			const DownloadProgress &progress) const {
		if (progress.total > 0) {
			return std::clamp(
				float64(progress.ready) / progress.total,
				0.,
				1.);
		}
		if (progress.progress > 0.) {
			return std::clamp(progress.progress, 0., 1.);
		}
		return std::nullopt;
	}

	void paintProgress(
			Painter &p,
			int top,
			std::optional<float64> progress) const {
		p.fillRect(
			0,
			top,
			width(),
			st::infoBulkSaveProgressHeight,
			st::infoBulkSaveProgressBg);
		const auto filled = progress
			? qRound(*progress * width())
			: width() / 3;
		if (filled > 0) {
			p.fillRect(
				0,
				top,
				filled,
				st::infoBulkSaveProgressHeight,
				st::infoBulkSaveProgressFg);
		}
	}

	void paintActive(
			Painter &p,
			int top,
			const DownloadProgress &progress) const {
		auto status = QString();
		if (progress.waitingForExisting) {
			status = u"Finishing existing download…"_q;
		} else if (progress.total > 0 && progress.ready > 0) {
			status = Ui::FormatProgressText(progress.ready, progress.total);
		} else if (progress.progress > 0.) {
			status = u"%1%"_q.arg(int(progress.progress * 100.));
		} else {
			status = u"Downloading…"_q;
		}
		p.setFont(st::normalFont);
		p.setPen(st::windowBoldFg);
		const auto statusWidth = st::normalFont->width(status);
		p.drawTextLeft(
			0,
			top,
			width() - statusWidth - st::infoBulkSaveNameSkip,
			st::normalFont->elided(
				progress.name,
				width() - statusWidth - st::infoBulkSaveNameSkip));
		p.setPen(st::windowSubTextFg);
		p.drawTextRight(0, top, width(), status);
		paintProgress(
			p,
			top + st::infoBulkSaveActiveProgressTop,
			itemProgress(progress));
	}

	void paintRecentlySaved(Painter &p, int top) const {
		const auto count = std::min(
			int(_progress.recentlySaved.size()),
			kVisibleRecent);
		if (!count) {
			return;
		}
		top += st::infoBulkSaveRecentTitleSkip;
		p.setFont(st::semiboldFont);
		p.setPen(st::windowBoldFg);
		p.drawTextLeft(0, top, width(), u"Just saved"_q);
		top += st::infoBulkSaveRecentTitleHeight;
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		for (auto i = 0; i != count; ++i) {
			p.drawTextLeft(
				0,
				top,
				width(),
				st::normalFont->elided(_progress.recentlySaved[i], width()));
			top += st::infoBulkSaveRecentRowHeight;
		}
	}

	Progress _progress;
	QString _destination;

};

class Job final : public std::enable_shared_from_this<Job> {
public:
	Job(
		not_null<Main::Session*> session,
		not_null<Window::SessionController*> controller,
		Scope scope)
	: _session(session)
	, _controller(base::make_weak(controller))
	, _scope(std::move(scope))
	, _recentlySavedTimer([=] {
		_recentlySaved.clear();
		_current.recentlySaved.clear();
		publish();
	})
	, _stopCheckTimer([=] {
		checkActive(false);
	}) {
	}

	void start() {
		if (_started || _current.finished) {
			return;
		}
		_started = true;
		_keepAlive = shared_from_this();

		const auto weak = weak_from_this();
		_session->data().photoLoadProgress(
		) | rpl::on_next([weak](not_null<PhotoData*> photo) {
			if (const auto job = weak.lock()) {
				if (job->hasActivePhoto(photo)) {
					job->checkActive();
				}
			}
		}, _lifetime);
		_session->data().documentLoadProgress(
		) | rpl::on_next([weak](not_null<DocumentData*> document) {
			if (const auto job = weak.lock()) {
				if (job->hasActiveDocument(document)) {
					job->checkActive();
				}
			}
		}, _lifetime);
		_session->downloaderTaskFinished(
		) | rpl::on_next([weak] {
			if (const auto job = weak.lock()) {
				job->checkActive();
			}
		}, _lifetime);

		loadPage(ServerMaxMsgId - 1);
		publish();
	}

	void cancel() {
		if (_current.stopping || _current.finished) {
			return;
		}
		_current.stopping = true;
		_pageLifetime.destroy();
		_pageLoading = false;
		_pending.clear();
		publish();
		checkActive(false);
		if (!_current.finished && !_active.empty()) {
			_stopCheckTimer.callEach(kStopCheckInterval);
		}
		finishIfDone();
	}

	[[nodiscard]] bool finished() const {
		return _current.finished;
	}

	[[nodiscard]] rpl::producer<Progress> progressValue() const {
		return rpl::single(_current) | rpl::then(_updates.events());
	}

	[[nodiscard]] Progress currentProgress() const {
		return _current;
	}

	void setShowDetailsCallback(Fn<void()> callback) {
		_showDetails = std::move(callback);
	}

	void setFinishedCallback(Fn<void(Progress)> callback) {
		_finishedCallback = std::move(callback);
	}

	void showBackgroundStatus() {
		if (_current.finished || _backgroundToast.get()) {
			return;
		}
		const auto controller = _controller.get();
		if (!controller) {
			return;
		}
		auto text = tr::marked(u"Saving media in the background. "_q);
		text.append(tr::link(
			u"Show progress"_q,
			u"internal:show_bulk_media_progress"_q));
		const auto weak = weak_from_this();
		_backgroundToast = controller->showToast(Ui::Toast::Config{
			.text = std::move(text),
			.filter = [weak](const ClickHandlerPtr &, Qt::MouseButton) {
				if (const auto job = weak.lock()) {
					job->hideBackgroundStatus();
					if (job->_showDetails) {
						job->_showDetails();
					}
				}
				return false;
			},
			.infinite = true,
		});
	}

	void hideBackgroundStatus() {
		if (const auto toast = _backgroundToast.get()) {
			toast->hideAnimated();
		}
		_backgroundToast = nullptr;
	}

private:
	static constexpr auto kPageSize = 64;
	static constexpr auto kMaxActive = 12;

	struct Active {
		FullMsgId id;
		QString name;
		QString path;
		TimeId date = 0;
		PhotoData *photo = nullptr;
		DocumentData *document = nullptr;
		std::shared_ptr<Data::PhotoMedia> photoView;
		bool waitingForExisting = false;
	};

	enum class StartResult {
		Started,
		Terminal,
		Deferred,
	};

	[[nodiscard]] MsgId universalId(FullMsgId id) const {
		return (id.peer == _scope.peerId)
			? id.msg
			: (id.msg - ServerMaxMsgId);
	}

	[[nodiscard]] bool hasActivePhoto(not_null<PhotoData*> photo) const {
		return ranges::any_of(_active, [&](const Active &active) {
			return active.photo == photo;
		});
	}

	[[nodiscard]] bool hasActiveDocument(
			not_null<DocumentData*> document) const {
		return ranges::any_of(_active, [&](const Active &active) {
			return active.document == document;
		});
	}

	[[nodiscard]] QString idSuffix(FullMsgId id) const {
		return QString::number(id.peer.value)
			+ u"_"_q
			+ QString::number(id.msg.bare);
	}

	[[nodiscard]] QString photoPath(FullMsgId id) const {
		return filedialogDefaultName(
			u"photo_"_q + idSuffix(id),
			u".jpg"_q,
			_scope.destination);
	}

	[[nodiscard]] QString documentPath(
			FullMsgId id,
			not_null<DocumentData*> document) const {
		auto name = base::FileNameFromUserString(document->filename());
		if (name.isEmpty()) {
			name = u"video_"_q + idSuffix(id) + u".mp4"_q;
		}
		const auto info = QFileInfo(name);
		auto prefix = info.completeBaseName();
		if (prefix.isEmpty()) {
			prefix = u"video_"_q + idSuffix(id);
		}
		const auto suffix = info.completeSuffix();
		return filedialogDefaultName(
			prefix,
			suffix.isEmpty() ? u".mp4"_q : (u"."_q + suffix),
			_scope.destination);
	}

	void setFileDates(const QString &path, TimeId date) const {
		if (date <= 0) {
			return;
		}
		auto file = QFile(path);
		if (!file.open(QIODevice::ReadWrite)) {
			return;
		}
		const auto when = base::unixtime::parse(date);
		file.setFileTime(when, QFileDevice::FileModificationTime);
		file.setFileTime(when, QFileDevice::FileAccessTime);
	}

	void loadPage(MsgId aroundId) {
		if (_current.stopping || _enumerationDone || _pageLoading) {
			return;
		}
		_pageLoading = true;
		_pageLifetime.destroy();

		auto viewer = SharedMediaMergedViewer(
			_session,
			SharedMediaMergedKey(
				SparseIdsMergedSlice::Key(
					_scope.peerId,
					_scope.topicRootId,
					_scope.monoforumPeerId,
					_scope.migratedPeerId,
					aroundId),
				_scope.type),
			kPageSize,
			1);
		const auto weak = weak_from_this();
		std::move(viewer
		) | rpl::filter([](const SparseIdsMergedSlice &slice) {
			return slice.fullCount().has_value()
				&& slice.skippedBefore().has_value()
				&& slice.skippedAfter().has_value();
		}) | rpl::take(1) | rpl::on_next([weak](SparseIdsMergedSlice slice) {
			if (const auto job = weak.lock()) {
				job->_pageLoading = false;
				job->handlePage(std::move(slice));
			}
		}, _pageLifetime);
	}

	void handlePage(SparseIdsMergedSlice slice) {
		if (_current.stopping) {
			return;
		}
		if (const auto count = slice.fullCount()) {
			_current.total = *count;
		}
		const auto skippedBefore = slice.skippedBefore().value_or(0);
		_enumerationDone = (skippedBefore == 0);

		if (slice.size() > 0) {
			_nextAroundId = universalId(slice[0]);
			for (auto i = slice.size(); i != 0; --i) {
				const auto id = slice[i - 1];
				const auto key = std::pair<uint64, int64>(
					id.peer.value,
					id.msg.bare);
				if (_seen.emplace(key).second) {
					_pending.push_back(id);
					++_current.discovered;
				}
			}
		} else {
			_enumerationDone = true;
		}
		publish();
		fillSlots();
	}

	StartResult startItem(FullMsgId id) {
		const auto item = _session->data().message(id);
		const auto peer = _session->data().peer(_scope.peerId);
		if (!item
			|| !peer->allowsForwarding()
			|| item->forbidsForward()
			|| item->forbidsSaving()
			|| HistoryView::ItemHasTtl(item)) {
			++_current.skipped;
			return StartResult::Terminal;
		}
		const auto media = item->media();
		if (!media) {
			++_current.skipped;
			return StartResult::Terminal;
		}

		if (const auto photo = media->photo()) {
			if (_scope.type == Type::Video) {
				++_current.skipped;
				return StartResult::Terminal;
			} else if (hasActivePhoto(photo)) {
				return StartResult::Deferred;
			}
			const auto view = photo->createMediaView();
			if (!view) {
				recordFailure(
					QFileInfo(photoPath(id)).fileName(),
					u"Could not prepare the photo for download."_q);
				return StartResult::Terminal;
			}
			const auto path = photoPath(id);
			const auto date = photo->date() ? photo->date() : item->date();
			_active.push_back({
				.id = id,
				.name = QFileInfo(path).fileName(),
				.path = path,
				.date = date,
				.photo = photo,
				.photoView = view,
			});
			photo->clearFailed(Data::PhotoSize::Large);
			view->wanted(Data::PhotoSize::Large, id);
			return StartResult::Started;
		}

		if (const auto document = media->document()) {
			if (_scope.type == Type::Photo || !document->isVideoFile()) {
				++_current.skipped;
				return StartResult::Terminal;
			} else if (hasActiveDocument(document)) {
				return StartResult::Deferred;
			}
			const auto path = documentPath(id, document);
			const auto waitingForExisting = document->loading();
			_active.push_back({
				.id = id,
				.name = QFileInfo(path).fileName(),
				.path = path,
				.date = item->date(),
				.document = document,
				.waitingForExisting = waitingForExisting,
			});
			if (!waitingForExisting) {
				document->save(id, path);
			}
			return StartResult::Started;
		}

		++_current.skipped;
		return StartResult::Terminal;
	}

	void fillSlots() {
		if (_current.stopping || _filling) {
			return;
		}
		_filling = true;
		auto attempts = int(_pending.size());
		while (_active.size() < kMaxActive
			&& !_pending.empty()
			&& attempts-- > 0) {
			const auto id = _pending.front();
			_pending.pop_front();
			if (startItem(id) == StartResult::Deferred) {
				_pending.push_back(id);
			}
		}
		_filling = false;

		checkActive(false);
		if (!_current.stopping
			&& _pending.empty()
			&& _active.size() < kMaxActive
			&& !_enumerationDone
			&& !_pageLoading) {
			loadPage(_nextAroundId);
		}
		finishIfDone();
		publish();
	}

	void checkActive(bool refill = true) {
		if (_checking) {
			return;
		}
		_checking = true;
		auto changed = false;
		for (auto i = _active.begin(); i != _active.end();) {
			auto saved = false;
			auto failed = false;
			if (i->photo) {
				if (!i->photo->loading()) {
					saved = i->photoView->saveToFile(i->path);
					failed = !saved;
				}
			} else if (i->document && !i->document->loading()) {
				if (i->waitingForExisting) {
					i->waitingForExisting = false;
					i->document->save(i->id, i->path);
					if (i->document->loading()) {
						++i;
						continue;
					}
				}
				saved = QFileInfo::exists(i->path);
				failed = !saved;
			}
			if (!saved && !failed) {
				++i;
				continue;
			}
			changed = true;
			if (saved) {
				setFileDates(i->path, i->date);
				_current.lastSavedPath = i->path;
				++_current.saved;
				recordRecentlySaved(i->name);
			} else {
				recordFailure(
					i->name,
					u"Could not save the downloaded file."_q);
			}
			i = _active.erase(i);
		}
		_checking = false;
		publish();

		if (refill && changed && !_filling && !_current.stopping) {
			fillSlots();
		} else {
			finishIfDone();
		}
	}

	void finishIfDone() {
		if (_current.finished || _pageLoading || !_active.empty()) {
			return;
		}
		if (_current.stopping) {
			_current.cancelled = true;
			finish();
			return;
		}
		if (!_enumerationDone || !_pending.empty()) {
			return;
		}
		finish();
	}

	void finish() {
		_current.finished = true;
		publish();
		if (_backgroundToast.get() && _finishedCallback) {
			_finishedCallback(_current);
		}
		hideBackgroundStatus();
		_lifetime.destroy();
		_stopCheckTimer.cancel();
		const auto weak = weak_from_this();
		crl::on_main([weak] {
			if (const auto job = weak.lock()) {
				job->_keepAlive = nullptr;
			}
		});
	}

	void recordFailure(QString name, QString reason) {
		++_current.failed;
		_current.failures.push_back({ std::move(name), std::move(reason) });
	}

	void recordRecentlySaved(QString name) {
		_recentlySaved.push_back(std::move(name));
		if (_recentlySaved.size() > kMaxRecent) {
			_recentlySaved.erase(begin(_recentlySaved));
		}
		_current.recentlySaved = _recentlySaved;
		_recentlySavedTimer.callOnce(kRecentVisibleDuration);
	}

	void publish() {
		_current.active.clear();
		_current.active.reserve(_active.size());
		for (const auto &active : _active) {
			auto progress = DownloadProgress();
			progress.name = active.name;
			progress.waitingForExisting = active.waitingForExisting;
			if (active.photo) {
				progress.ready = active.photo->loadOffset();
				progress.total = active.photo->imageByteSize(
					Data::PhotoSize::Large);
				progress.progress = active.photo->progress();
			} else if (active.document) {
				progress.ready = active.document->loadOffset();
				progress.total = active.document->size;
				progress.progress = active.document->progress();
			}
			_current.active.push_back(std::move(progress));
		}
		_updates.fire_copy(_current);
	}

	const not_null<Main::Session*> _session;
	const base::weak_ptr<Window::SessionController> _controller;
	const Scope _scope;
	Progress _current;
	std::deque<FullMsgId> _pending;
	std::vector<Active> _active;
	std::set<std::pair<uint64, int64>> _seen;
	MsgId _nextAroundId = ServerMaxMsgId - 1;
	bool _started = false;
	bool _enumerationDone = false;
	bool _pageLoading = false;
	bool _filling = false;
	bool _checking = false;
	static constexpr auto kMaxRecent = 2;
	static constexpr auto kRecentVisibleDuration = crl::time(3000);
	static constexpr auto kStopCheckInterval = crl::time(500);
	rpl::event_stream<Progress> _updates;
	rpl::lifetime _pageLifetime;
	rpl::lifetime _lifetime;
	base::Timer _recentlySavedTimer;
	base::Timer _stopCheckTimer;
	std::vector<QString> _recentlySaved;
	std::shared_ptr<Job> _keepAlive;
	base::weak_ptr<Ui::Toast::Instance> _backgroundToast;
	Fn<void()> _showDetails;
	Fn<void(Progress)> _finishedCallback;

};

[[nodiscard]] inline QString Title(Type type) {
	switch (type) {
	case Type::Photo: return u"Save all photos"_q;
	case Type::Video: return u"Save all videos"_q;
	case Type::PhotoVideo: return u"Save all media"_q;
	default: return u"Save all media"_q;
	}
}

[[nodiscard]] inline QString CompletionText(const Progress &progress) {
	if (!progress.saved && !progress.failed) {
		return progress.cancelled
			? u"Saving stopped before any media was saved."_q
			: u"No saveable media found."_q;
	}
	if (!progress.saved) {
		return u"No media was saved. %1 downloads failed."_q
			.arg(progress.failed);
	}
	if (progress.cancelled) {
		return u"Stopped after saving %1 items • %2 skipped • %3 failed"_q
			.arg(progress.saved)
			.arg(progress.skipped)
			.arg(progress.failed);
	}
	return u"Saved %1 items • %2 skipped • %3 failed"_q
		.arg(progress.saved)
		.arg(progress.skipped)
		.arg(progress.failed);
}

inline void Start(
		not_null<Window::SessionController*> controller,
		Scope scope) {
	const auto initialPath = [] {
		const auto path = Core::App().settings().downloadPath();
		if (!path.isEmpty() && path != FileDialog::Tmp()) {
			return path.left(path.size() - (path.endsWith('/') ? 1 : 0));
		}
		return QString();
	}();
	const auto weak = base::make_weak(controller);
	FileDialog::GetFolder(
		controller->window().widget().get(),
		tr::lng_download_path_choose(tr::now),
		initialPath,
		[weak, scope = std::move(scope)](QString &&result) mutable {
			const auto controller = weak.get();
			if (!controller || result.isEmpty()) {
				return;
			}
			scope.destination = result.endsWith('/')
				? std::move(result)
				: (std::move(result) + '/');
			if (!QDir().mkpath(scope.destination)) {
				controller->showToast(u"Could not create the destination folder."_q);
				return;
			}

			const auto title = Title(scope.type);
			const auto destination = scope.destination;
			const auto job = std::make_shared<Job>(
				&controller->session(),
				controller,
				std::move(scope));
			const auto weakJob = std::weak_ptr<Job>(job);
			const auto showDetails = [weak, weakJob, title, destination] {
				const auto controller = weak.get();
				const auto job = weakJob.lock();
				if (!controller || !job) {
					return;
				}
				job->hideBackgroundStatus();
				controller->show(Box([=](not_null<Ui::GenericBox*> box) {
					box->setTitle(rpl::single(title));
					const auto progress = box->addRow(object_ptr<ProgressWidget>(
						box,
						destination));
					box->addLeftButton(
						rpl::single(u"Show in Folder"_q),
						[destination] { File::ShowInFolder(destination); });
					const auto reviewFailures = box->addLeftButton(
						rpl::single(u"Review failures"_q),
						[weak, weakJob] {
							const auto controller = weak.get();
							const auto job = weakJob.lock();
							if (!controller || !job) {
								return;
							}
							const auto failures = job->currentProgress().failures;
							controller->show(Box([failures](
									not_null<Ui::GenericBox*> box) {
								box->setTitle(rpl::single(u"Failed media"_q));
								for (const auto &failure : failures) {
									box->addRow(object_ptr<Ui::FlatLabel>(
										box,
										failure.name + u" — "_q + failure.reason,
										st::boxLabel));
								}
								box->addButton(tr::lng_close(), [=] { box->closeBox(); });
							}));
						});
					reviewFailures->hide();
					const auto stop = box->addButton(
						rpl::single(u"Stop after active downloads"_q),
						[job] { job->cancel(); });
					job->progressValue(
					) | rpl::on_next([=](const Progress &value) {
						progress->setProgress(value);
						reviewFailures->setVisible(!value.failures.empty());
						if (value.stopping && !value.finished) {
							stop->setText(rpl::single(
								u"Finishing active downloads…"_q));
							stop->setDisabled(true);
						}
					}, progress->lifetime());
					job->progressValue(
					) | rpl::filter([](const Progress &value) {
						return value.finished;
					}) | rpl::take(1) | rpl::on_next([=](const Progress &) {
						stop->setText(tr::lng_close());
						stop->setDisabled(false);
						stop->setClickedCallback([=] { box->closeBox(); });
					}, box->lifetime());
					box->boxClosing(
					) | rpl::on_next(
						[job] { job->showBackgroundStatus(); },
						box->lifetime());
				}));
			};
			job->setShowDetailsCallback(showDetails);
			job->setFinishedCallback([weak, destination](Progress progress) {
				if (const auto controller = weak.get()) {
					const auto reveal = progress.lastSavedPath.isEmpty()
						? destination
						: progress.lastSavedPath;
					auto text = tr::marked(CompletionText(progress));
					if (progress.saved > 0) {
						text.append(u'\n').append(tr::link(
							u"Show in Folder"_q,
							u"internal:show_bulk_media_saved"_q));
					}
					controller->showToast(Ui::Toast::Config{
						.text = std::move(text),
						.filter = [reveal](
								const ClickHandlerPtr &,
								Qt::MouseButton) {
							File::ShowInFolder(reveal);
							return false;
						},
					});
				}
			});
			job->start();
			showDetails();
		});
}

} // namespace Info::Media::BulkSave
