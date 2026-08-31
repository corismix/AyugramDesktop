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
#include "data/data_forum_topic.h"
#include "history/history_item.h"
#include "history/view/history_view_context_menu.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "storage/storage_account.h"

#include "styles/style_info.h"
#include "styles/style_layers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDate>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QMutex>

#include <algorithm>
#include <deque>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace Info::Media::BulkSave {

using Type = Storage::SharedMediaType;
class Manager;

enum class State : uchar {
	Running,
	Pausing,
	Paused,
	Cancelling,
	Finished,
};

enum class FailureCategory : uchar {
	Unknown,
	MissingMessage,
	Restricted,
	Unsupported,
	Prepare,
	Destination,
	Save,
};

struct Scope {
	PeerId peerId = 0;
	MsgId topicRootId = 0;
	PeerId monoforumPeerId = 0;
	PeerId migratedPeerId = 0;
};

enum class Layout : uchar {
	Flat,
	Type,
	YearMonth,
	Topic,
};

[[nodiscard]] inline Storage::SharedMediaTypesMask TypesFor(Type type) {
	if (type == Type::PhotoVideo) {
		return Storage::SharedMediaTypesMask{}
			.added(Type::Photo)
			.added(Type::Video);
	}
	return Storage::SharedMediaTypesMask{}.added(type);
}

struct Request {
	Scope scope;
	Storage::SharedMediaTypesMask types;
	QDate fromDate;
	QDate toDate;
	PeerId senderId = 0;
	int64 minimumSize = 0;
	int64 maximumSize = 0;
	Layout layout = Layout::Flat;
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
	FullMsgId id;
	Type type = Type::Photo;
	FailureCategory category = FailureCategory::Unknown;
	QString name;
	QString reason;
};

struct Progress {
	int discovered = 0;
	int saved = 0;
	int alreadySaved = 0;
	int skipped = 0;
	int filteredOut = 0;
	int failed = 0;
	std::optional<int> total;
	std::vector<DownloadProgress> active;
	std::vector<Failure> failures;
	std::vector<QString> recentlySaved;
	QString lastSavedPath;
	bool stopping = false;
	bool finished = false;
	bool cancelled = false;
	State state = State::Running;
};

class Manifest final {
public:
	struct Entry {
		QString path;
		qint64 size = 0;
		qint64 modified = 0;
	};

	explicit Manifest(QString root)
	: _root(QDir::cleanPath(std::move(root)))
	, _path(QDir(_root).filePath(u".ayu-bulk-save-manifest.json"_q)) {
	}

	void load() {
		_entries.clear();
		_loaded = true;
		auto file = QFile(_path);
		if (!file.exists()) {
			_valid = true;
			return;
		}
		if (!file.open(QIODevice::ReadOnly)) {
			_valid = false;
			return;
		}
		QJsonParseError error;
		const auto document = QJsonDocument::fromJson(file.readAll(), &error);
		if (error.error != QJsonParseError::NoError
			|| !document.isObject()) {
			_valid = false;
			return;
		}
		const auto object = document.object();
		if (object.value(u"version"_q).toInt() != 1
			|| !object.value(u"entries"_q).isObject()) {
			_valid = false;
			return;
		}
		const auto entries = object.value(u"entries"_q).toObject();
		for (auto i = entries.begin(); i != entries.end(); ++i) {
			const auto value = i.value().toObject();
			const auto relative = value.value(u"path"_q).toString();
			bool sizeOk = false;
			const auto size = value.value(u"size"_q)
				.toString().toLongLong(&sizeOk);
			if (value.value(u"status"_q).toString() != u"complete"_q
				|| !validRelativePath(relative)
				|| !sizeOk
				|| size < 0) {
				continue;
			}
			bool modifiedOk = false;
			const auto modified = value.value(u"modified"_q)
				.toString().toLongLong(&modifiedOk);
			_entries.emplace(i.key(), Entry{
				.path = relative,
				.size = size,
				.modified = modifiedOk ? modified : 0,
			});
		}
		_valid = true;
	}

	[[nodiscard]] bool contains(
			const QString &key) const {
		if (!_loaded) {
			return false;
		}
		const auto i = _entries.find(key);
		if (i == _entries.end()) {
			return false;
		}
		const auto path = absolutePath(i->second.path);
		return !path.isEmpty()
			&& QFileInfo::exists(path)
			&& QFileInfo(path).size() == i->second.size;
	}

	void record(const QString &key, const QString &path) {
		const auto relative = relativePath(path);
		if (relative.isEmpty() || !QFileInfo::exists(path)) {
			return;
		}
		static QMutex mutex;
		const QMutexLocker lock(&mutex);
		Manifest current(_root);
		current.load();
		if (!current._valid) {
			return;
		}
		current._entries[key] = Entry{
			.path = relative,
			.size = QFileInfo(path).size(),
			.modified = QFileInfo(path).lastModified().toSecsSinceEpoch(),
		};
		if (!current.write()) {
			return;
		}
		_entries = std::move(current._entries);
		_valid = true;
		_loaded = true;
	}

private:
	[[nodiscard]] bool validRelativePath(const QString &path) const {
		if (path.isEmpty() || QFileInfo(path).isAbsolute()) {
			return false;
		}
		return !absolutePath(path).isEmpty();
	}

	[[nodiscard]] QString absolutePath(const QString &relative) const {
		if (relative.isEmpty() || QFileInfo(relative).isAbsolute()) {
			return QString();
		}
		const auto root = QDir::cleanPath(_root);
		const auto path = QDir::cleanPath(QDir(root).filePath(relative));
		return (path == root || path.startsWith(root + '/'))
			? path
			: QString();
	}

	[[nodiscard]] QString relativePath(const QString &path) const {
		const auto absolute = QDir::cleanPath(path);
		const auto root = QDir::cleanPath(_root);
		if (absolute != root && !absolute.startsWith(root + '/')) {
			return QString();
		}
		const auto relative = QDir(root).relativeFilePath(absolute);
		return validRelativePath(relative) ? relative : QString();
	}

	[[nodiscard]] bool write() const {
		QJsonObject entries;
		for (const auto &[key, entry] : _entries) {
			entries.insert(key, QJsonObject{
				{ u"path"_q, entry.path },
				{ u"size"_q, QString::number(entry.size) },
				{ u"status"_q, u"complete"_q },
				{ u"modified"_q, QString::number(entry.modified) },
			});
		}
		const auto document = QJsonDocument(QJsonObject{
			{ u"version"_q, 1 },
			{ u"entries"_q, entries },
		});
		QSaveFile file(_path);
		return file.open(QIODevice::WriteOnly)
			&& file.write(document.toJson(QJsonDocument::Compact)) != -1
			&& file.commit();
	}

	QString _root;
	QString _path;
	std::map<QString, Entry> _entries;
	bool _loaded = false;
	bool _valid = false;
};

[[nodiscard]] inline QString Title(
	const Storage::SharedMediaTypesMask &types);

[[nodiscard]] inline QString StateText(State state) {
	switch (state) {
	case State::Running: return u"Running"_q;
	case State::Pausing: return u"Pausing"_q;
	case State::Paused: return u"Paused"_q;
	case State::Cancelling: return u"Cancelling"_q;
	case State::Finished: return u"Finished"_q;
	}
	return u"Unknown"_q;
}

[[nodiscard]] inline QString DestinationText(const QString &path) {
	return u"Saving to %1"_q.arg(QDir::toNativeSeparators(path));
}

[[nodiscard]] inline QString ProgressTitle(const Progress &progress) {
	if (progress.finished) {
		if (!progress.saved && !progress.alreadySaved && progress.failed) {
			return u"No media was saved"_q;
		}
		return progress.cancelled
			? u"Stopped saving media"_q
			: u"Finished saving media"_q;
	}
	if (progress.stopping) {
		return u"Finishing active downloads…"_q;
	}
	if (progress.state == State::Paused) {
		return u"Bulk media save paused"_q;
	}
	if (progress.state == State::Pausing) {
		return u"Pausing bulk media save…"_q;
	}
	if (progress.total) {
		return u"Saving %1 of %2 items"_q
			.arg(progress.saved + progress.alreadySaved)
			.arg(*progress.total);
	}
	return progress.discovered
		? u"Saving %1 items"_q.arg(progress.saved + progress.alreadySaved)
		: u"Finding media…"_q;
}

[[nodiscard]] inline QString ProgressDetails(const Progress &progress) {
	auto details = QString();
	if (!progress.finished && !progress.active.empty()) {
		details = u"%1 downloading"_q.arg(progress.active.size());
	} else if (!progress.finished && !progress.stopping) {
		details = u"%1 discovered"_q.arg(progress.discovered);
	}
	if (progress.alreadySaved || progress.skipped
		|| progress.filteredOut || progress.failed) {
		if (!details.isEmpty()) {
			details += u" • "_q;
		}
		details += u"%1 already saved • %2 skipped • %3 filtered • %4 failed"_q
			.arg(progress.alreadySaved)
			.arg(progress.skipped)
			.arg(progress.filteredOut)
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
				_progress.saved + _progress.alreadySaved
				+ _progress.skipped + _progress.failed)
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
		Window::SessionController *controller,
		Request request)
	: _session(session)
	, _controller(controller ? base::make_weak(controller) : nullptr)
	, _request(std::move(request))
	, _manifest(_request.destination)
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
		if (_started || _current.finished || _current.state == State::Cancelling) {
			return;
		}
		_started = true;
		_manifest.load();
		_current.state = State::Running;
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

		if (!_pending.empty()) {
			fillSlots();
		} else if (!_enumerationDone) {
			loadPage(_nextAroundId);
		}
		publish();
	}

	void pause() {
		if (_current.finished
			|| _current.state == State::Pausing
			|| _current.state == State::Paused
			|| _current.state == State::Cancelling) {
			return;
		}
		_current.state = State::Pausing;
		_pageLifetime.destroy();
		_pageLoading = false;
		if (_active.empty()) {
			_current.state = State::Paused;
		}
		publish();
	}

	void resume() {
		if (_current.finished
			|| (_current.state != State::Paused
				&& _current.state != State::Pausing)) {
			return;
		}
		_current.state = State::Running;
		if (!_started) {
			start();
		} else {
			fillSlots();
		}
		publish();
	}

	void cancel() {
		if (_current.stopping || _current.finished) {
			return;
		}
		_current.state = State::Cancelling;
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

	[[nodiscard]] const Request &request() const {
		return _request;
	}

	[[nodiscard]] State state() const {
		return _current.state;
	}

	[[nodiscard]] bool canRetry() const {
		return _current.finished && !_current.failures.empty();
	}

	void attachController(not_null<Window::SessionController*> controller) {
		_controller = base::make_weak(controller);
	}

	[[nodiscard]] std::vector<Failure> failures() const {
		return _current.failures;
	}

	void setShowDetailsCallback(Fn<void()> callback) {
		_showDetails = std::move(callback);
	}

	void setFinishedCallback(Fn<void(Progress)> callback) {
		_finishedCallback = std::move(callback);
	}

	void setChangedCallback(Fn<void()> callback) {
		_changedCallback = std::move(callback);
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
			.attach = RectPart::BottomLeft,
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
	friend class Manager;
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
		return (id.peer == _request.scope.peerId)
			? id.msg
			: (id.msg - ServerMaxMsgId);
	}

	[[nodiscard]] QString manifestKey(FullMsgId id, Type type) const {
		return u"%1:%2:%3:%4"_q
			.arg(_session->uniqueId())
			.arg(id.peer.value)
			.arg(id.msg.bare)
			.arg(type == Type::Photo ? u"photo"_q : u"video"_q);
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

	[[nodiscard]] QString messageIdSuffix(FullMsgId id) const {
		return u"_m%1"_q.arg(id.msg.bare);
	}

	[[nodiscard]] QString filenameWithMessageIdSuffix(
			QString filename,
			FullMsgId id,
			const QString &directory) const {
		const auto extension = QFileInfo(filename).completeSuffix();
		const auto extensionStart = extension.isEmpty()
			? filename.size()
			: filename.size() - extension.size() - 1;
		const auto result = filename.left(extensionStart)
			+ messageIdSuffix(id)
			+ filename.mid(extensionStart);
		return filedialogNextFilename(
			QFileInfo(result).fileName(),
			QString(),
			directory);
	}

	[[nodiscard]] QString directoryFor(
			not_null<HistoryItem*> item,
			Type type,
			TimeId date) const {
		auto directory = _request.destination;
		if (_request.layout == Layout::Type) {
			directory += (type == Type::Photo)
				? u"Photos/"_q
				: u"Videos/"_q;
		} else if (_request.layout == Layout::YearMonth) {
			directory += base::unixtime::parse(date).toString(u"yyyy-MM/"_q);
		} else if (_request.layout == Layout::Topic) {
			auto name = QString();
			if (const auto rootId = item->topicRootId()) {
				if (const auto topic = _session->data().peer(
					_request.scope.peerId)->forumTopicFor(rootId)) {
					name = topic->title();
				}
			}
			name = base::FileNameFromUserString(name);
			if (name.isEmpty()) {
				name = u"Topic-%1"_q.arg(item->topicRootId().bare);
			}
			directory += name + '/';
		}
		const auto root = QDir::cleanPath(_request.destination);
		const auto child = QDir::cleanPath(directory);
		if (child != root
			&& !child.startsWith(root + '/')) {
			return QString();
		}
		if (!QDir().mkpath(directory)) {
			return QString();
		}
		return directory;
	}

	[[nodiscard]] QString photoPath(
			FullMsgId id,
			const QString &directory) const {
		return filenameWithMessageIdSuffix(
			filedialogDefaultName(
				u"photo"_q,
				u".jpg"_q,
				directory),
			id,
			directory);
	}

	[[nodiscard]] QString documentPath(
			FullMsgId id,
			not_null<DocumentData*> document,
			const QString &directory) const {
		auto name = base::FileNameFromUserString(document->filename());
		if (name.isEmpty()) {
			name = u"video.mp4"_q;
		}
		const auto info = QFileInfo(name);
		auto prefix = info.completeBaseName();
		if (prefix.isEmpty()) {
			prefix = u"video"_q;
		}
		const auto suffix = info.completeSuffix();
		return filenameWithMessageIdSuffix(
			filedialogDefaultName(
				prefix,
				suffix.isEmpty() ? u".mp4"_q : (u"."_q + suffix),
				directory),
			id,
			directory);
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
					_request.scope.peerId,
					_request.scope.topicRootId,
					_request.scope.monoforumPeerId,
					_request.scope.migratedPeerId,
					aroundId),
				_request.types.contains(Type::Photo)
					&& _request.types.contains(Type::Video)
				? Type::PhotoVideo
				: _request.types.contains(Type::Video)
				? Type::Video
				: Type::Photo),
			kPageSize,
			1);
		const auto weak = weak_from_this();
		std::move(viewer
		) | rpl::filter([](const SparseIdsMergedSlice &slice) {
			return slice.fullCount().has_value()
				&& slice.skippedBefore().has_value()
				&& slice.skippedAfter().has_value();
		}) | rpl::take(1) | rpl::on_next([weak](SparseIdsMergedSlice slice) {
			crl::on_main([weak, slice = std::move(slice)]() mutable {
				if (const auto job = weak.lock()) {
					job->_pageLoading = false;
					job->handlePage(std::move(slice));
				}
			});
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
				enqueueItemOrItsGroup(slice[i - 1]);
			}
		} else {
			_enumerationDone = true;
		}
		publish();
		fillSlots();
	}

	void enqueueItemOrItsGroup(FullMsgId id) {
		const auto item = _session->data().message(id);
		if (item) {
			for (const auto groupId : _session->data().itemOrItsGroup(item)) {
				enqueue(groupId);
			}
		} else {
			enqueue(id);
		}
	}

	void enqueue(FullMsgId id) {
		const auto key = std::pair<uint64, int64>(
			id.peer.value,
			id.msg.bare);
		if (_seen.emplace(key).second) {
			_pending.push_back(id);
			++_current.discovered;
		}
	}

	StartResult startItem(FullMsgId id) {
		const auto item = _session->data().message(id);
		const auto peer = _session->data().peer(_request.scope.peerId);
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
		const auto date = item->date();
		const auto localDate = base::unixtime::parse(date).date();
		if ((!_request.fromDate.isNull() && localDate < _request.fromDate)
			|| (!_request.toDate.isNull() && localDate > _request.toDate)
			|| (_request.senderId && item->author()->id != _request.senderId)) {
			++_current.filteredOut;
			return StartResult::Terminal;
		}

		if (const auto photo = media->photo()) {
			if (!_request.types.contains(Type::Photo)) {
				++_current.skipped;
				return StartResult::Terminal;
			} else if (hasActivePhoto(photo)) {
				return StartResult::Deferred;
			}
			const auto size = photo->imageByteSize(Data::PhotoSize::Large);
			if ((_request.minimumSize > 0 && size < _request.minimumSize)
				|| (_request.maximumSize > 0 && size > _request.maximumSize)) {
				++_current.filteredOut;
				return StartResult::Terminal;
			}
			if (_manifest.contains(manifestKey(id, Type::Photo))) {
				++_current.alreadySaved;
				return StartResult::Terminal;
			}
			const auto view = photo->createMediaView();
			if (!view) {
				recordFailure(
					id,
					Type::Photo,
					FailureCategory::Prepare,
					u"photo"_q,
					u"Could not prepare the photo for download."_q);
				return StartResult::Terminal;
			}
			const auto mediaDate = photo->date() ? photo->date() : date;
			const auto directory = directoryFor(
				item,
				Type::Photo,
				mediaDate);
			if (directory.isEmpty()) {
				recordFailure(id, Type::Photo, FailureCategory::Destination,
					u"photo"_q, u"Could not create the destination folder."_q);
				return StartResult::Terminal;
			}
			const auto path = photoPath(id, directory);
			_active.push_back({
				.id = id,
				.name = QFileInfo(path).fileName(),
				.path = path,
				.date = mediaDate,
				.photo = photo,
				.photoView = view,
			});
			photo->clearFailed(Data::PhotoSize::Large);
			view->wanted(Data::PhotoSize::Large, id);
			return StartResult::Started;
		}

		if (const auto document = media->document()) {
			if (!_request.types.contains(Type::Video)
				|| !document->isVideoFile()) {
				++_current.skipped;
				return StartResult::Terminal;
			} else if (hasActiveDocument(document)) {
				return StartResult::Deferred;
			}
			const auto size = document->size;
			if ((_request.minimumSize > 0 && size < _request.minimumSize)
				|| (_request.maximumSize > 0 && size > _request.maximumSize)) {
				++_current.filteredOut;
				return StartResult::Terminal;
			}
			if (_manifest.contains(manifestKey(id, Type::Video))) {
				++_current.alreadySaved;
				return StartResult::Terminal;
			}
			const auto directory = directoryFor(item, Type::Video, date);
			if (directory.isEmpty()) {
				recordFailure(id, Type::Video, FailureCategory::Destination,
					u"video"_q, u"Could not create the destination folder."_q);
				return StartResult::Terminal;
			}
			const auto path = documentPath(id, document, directory);
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
		if (_current.stopping
			|| _current.state != State::Running
			|| _filling) {
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
				_manifest.record(
					manifestKey(i->id, i->photo ? Type::Photo : Type::Video),
					i->path);
				_current.lastSavedPath = i->path;
				++_current.saved;
				recordRecentlySaved(i->name);
			} else {
				recordFailure(
					i->id,
					i->photo ? Type::Photo : Type::Video,
					FailureCategory::Save,
					i->name,
					u"Could not save the downloaded file."_q);
			}
			i = _active.erase(i);
		}
		_checking = false;
		publish();

		if (_current.state == State::Pausing && _active.empty()) {
			_current.state = State::Paused;
		}
		if (refill
			&& changed
			&& !_filling
			&& !_current.stopping
			&& _current.state == State::Running) {
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
		_current.state = State::Finished;
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

	void recordFailure(
			FullMsgId id,
			Type type,
			FailureCategory category,
			QString name,
			QString reason) {
		++_current.failed;
		_current.failures.push_back({
			.id = id,
			.type = type,
			.category = category,
			.name = std::move(name),
			.reason = std::move(reason),
		});
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
		if (_changedCallback) {
			_changedCallback();
		}
	}

	const not_null<Main::Session*> _session;
	base::weak_ptr<Window::SessionController> _controller;
	const Request _request;
	Manifest _manifest;
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
	Fn<void()> _changedCallback;

};

class Manager final {
public:
	explicit Manager(not_null<Main::Session*> session)
	: _session(session)
	, _persistTimer([=] { persist(); }) {
		load();
	}

	~Manager() {
		persist();
	}

	[[nodiscard]] std::shared_ptr<Job> create(
			not_null<Window::SessionController*> controller,
			Request request) {
		auto job = std::make_shared<Job>(_session, controller, std::move(request));
		attach(job);
		_jobs.push_back(std::move(job));
		persistDelayed();
		return _jobs.back();
	}

	void show(not_null<Window::SessionController*> controller) {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(u"Bulk media saves"_q));
			for (auto i = 0; i != _jobs.size(); ++i) {
				const auto job = _jobs[i];
				job->attachController(controller);
				const auto row = box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					u"%1 — %2"_q.arg(Title(job->request().types))
						.arg(StateText(job->state())),
					st::boxLabel));
				job->progressValue() | rpl::on_next([row, job](const Progress &value) {
					row->setText(u"%1 — %2 (%3 saved, %4 already saved, %5 failed)"_q
						.arg(Title(job->request().types))
						.arg(StateText(value.state))
						.arg(value.saved)
						.arg(value.alreadySaved)
						.arg(value.failed));
				}, box->lifetime());
				box->addLeftButton(rpl::single(u"Show progress"_q),
					[=] {
						box->closeBox();
						showProgress(controller, job);
					});
				if (job->state() == State::Running) {
					box->addLeftButton(rpl::single(u"Pause"_q), [job] {
						job->pause();
					});
				} else if (job->state() == State::Paused) {
					box->addLeftButton(rpl::single(u"Resume"_q), [job] {
						job->resume();
					});
				}
				if (job->state() == State::Running
					|| job->state() == State::Pausing
					|| job->state() == State::Paused) {
					box->addLeftButton(rpl::single(u"Cancel"_q), [job] {
						job->cancel();
					});
				}
				if (job->canRetry()) {
					box->addLeftButton(rpl::single(u"Retry failed"_q), [=] {
						retry(controller, job);
					});
				}
				if (i + 1 != _jobs.size()) {
					box->addRow(object_ptr<Ui::RpWidget>(box));
				}
			}
			for (const auto &attention : _attention) {
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					u"Needs attention — %1"_q.arg(attention),
					st::boxLabel));
			}
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		}));
	}

private:
	static constexpr auto kKey = "ayu_bulk_media_save_jobs_v1";

	void attach(const std::shared_ptr<Job> &job) {
		job->setChangedCallback([this] { persistDelayed(); });
	}

	void retry(
			not_null<Window::SessionController*> controller,
			const std::shared_ptr<Job> &job) {
		auto request = job->request();
		const auto failed = job->failures();
		auto retryJob = std::make_shared<Job>(
			_session,
			controller,
			std::move(request));
		for (const auto &failure : failed) {
			if (failure.id) {
				retryJob->_pending.push_back(failure.id);
				retryJob->_seen.emplace(
					failure.id.peer.value,
					failure.id.msg.bare);
			}
		}
		retryJob->_enumerationDone = true;
		retryJob->_current.total = int(retryJob->_pending.size());
		attach(retryJob);
		_jobs.push_back(std::move(retryJob));
		_jobs.back()->start();
		persistDelayed();
	}

	void showProgress(
			not_null<Window::SessionController*> controller,
			const std::shared_ptr<Job> &job) {
		job->attachController(controller);
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(Title(job->request().types)));
			const auto progress = box->addRow(object_ptr<ProgressWidget>(
				box,
				job->request().destination));
			const auto pause = box->addButton(rpl::single(u"Pause"_q), [job] {
				job->pause();
			});
			job->progressValue() | rpl::on_next([pause, progress](
					const Progress &value) {
				progress->setProgress(value);
				pause->setText(rpl::single(
					(value.state == State::Paused)
						? u"Resume"_q
						: u"Pause"_q));
				pause->setDisabled(
					value.finished || value.state == State::Pausing);
			}, progress->lifetime());
			pause->setClickedCallback([job, pause] {
			if (job->state() == State::Paused) {
				job->resume();
			} else {
				job->pause();
			}
		});
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		}));
	}

	void persistDelayed() {
		_persistTimer.callOnce(crl::time(1000));
	}

	void persist() {
		QByteArray data;
		QDataStream stream(&data, QIODevice::WriteOnly);
		stream << quint32(2) << quint32(_jobs.size());
		for (const auto &job : _jobs) {
			const auto &request = job->request();
			stream << quint64(_session->uniqueId());
			stream << qint64(request.scope.peerId.value)
				<< qint64(request.scope.topicRootId.bare)
				<< qint64(request.scope.monoforumPeerId.value)
				<< qint64(request.scope.migratedPeerId.value);
			stream << quint8(request.types.contains(Type::Photo))
				<< quint8(request.types.contains(Type::Video))
				<< request.fromDate << request.toDate
				<< qint64(request.senderId.value)
				<< qint64(request.minimumSize)
				<< qint64(request.maximumSize)
				<< quint8(request.layout)
				<< request.destination;
			stream << quint8(job->state())
				<< qint64(job->_nextAroundId.bare)
				<< quint8(job->_enumerationDone)
				<< quint8(job->_started)
				<< quint32(job->_pending.size())
				<< quint32(job->_seen.size());
			for (const auto &id : job->_pending) {
				stream << qint64(id.peer.value) << qint64(id.msg.bare);
			}
			for (const auto &[peer, message] : job->_seen) {
				stream << quint64(peer) << qint64(message);
			}
			const auto &progress = job->_current;
			stream << qint32(progress.discovered)
				<< qint32(progress.saved)
				<< qint32(progress.skipped)
				<< qint32(progress.filteredOut)
				<< qint32(progress.failed);
			stream << quint32(progress.failures.size());
			for (const auto &failure : progress.failures) {
				stream << qint64(failure.id.peer.value)
					<< qint64(failure.id.msg.bare)
					<< quint8(failure.type)
					<< quint8(failure.category)
					<< failure.name << failure.reason;
			}
			stream << qint32(progress.alreadySaved);
		}
		if (stream.status() == QDataStream::Ok) {
			_session->local().writePrefGeneric(kKey, data);
		}
	}

	void load() {
		const auto data = _session->local().readPrefGeneric(kKey);
		if (!data) {
			return;
		}
		QDataStream stream(*data);
		quint32 version = 0;
		quint32 count = 0;
		stream >> version >> count;
		if ((version != 1 && version != 2) || count > 1000) {
			_attention.push_back(u"Invalid saved bulk-media job data"_q);
			return;
		}
		for (auto i = 0u; i != count && stream.status() == QDataStream::Ok; ++i) {
			quint64 sessionId = 0;
			stream >> sessionId;
			if (sessionId != _session->uniqueId()) {
				_attention.push_back(u"A saved bulk-media job belongs to another account"_q);
				return;
			}
			Request request;
			qint64 peer = 0;
			qint64 topic = 0;
			qint64 sublist = 0;
			qint64 migrated = 0;
			quint8 photo = 0;
			quint8 video = 0;
			quint8 layout = 0;
			stream >> peer >> topic >> sublist >> migrated
				>> photo >> video >> request.fromDate >> request.toDate;
			qint64 sender = 0;
			stream >> sender >> request.minimumSize >> request.maximumSize
				>> layout >> request.destination;
			request.scope = {
				.peerId = PeerId(peer),
				.topicRootId = MsgId(topic),
				.monoforumPeerId = PeerId(sublist),
				.migratedPeerId = PeerId(migrated),
			};
			request.types = {};
			if (photo) request.types.added(Type::Photo);
			if (video) request.types.added(Type::Video);
			request.senderId = PeerId(sender);
			request.layout = Layout(layout);
			quint8 state = 0;
			qint64 next = 0;
			quint8 done = 0;
			quint8 started = 0;
			quint32 pending = 0;
			quint32 seen = 0;
			stream >> state >> next >> done >> started >> pending >> seen;
			if (pending > 100000 || seen > 1000000) return;
			auto job = std::make_shared<Job>(_session, nullptr, request);
			job->_current.state = (State(state) == State::Finished)
				? State::Finished
				: State::Paused;
			job->_current.finished = (job->_current.state == State::Finished);
			job->_nextAroundId = MsgId(next);
			job->_enumerationDone = done;
			job->_started = false;
			for (auto j = 0u; j != pending; ++j) {
				qint64 p = 0; qint64 m = 0;
				stream >> p >> m;
				job->_pending.push_back({ PeerId(p), MsgId(m) });
			}
			for (auto j = 0u; j != seen; ++j) {
				quint64 p = 0; qint64 m = 0;
				stream >> p >> m;
				job->_seen.emplace(p, m);
			}
			stream >> job->_current.discovered
				>> job->_current.saved
				>> job->_current.skipped
				>> job->_current.filteredOut
				>> job->_current.failed;
			quint32 failures = 0;
			stream >> failures;
			for (auto j = 0u; j != failures; ++j) {
				Failure failure;
				qint64 p = 0; qint64 m = 0;
				quint8 type = 0; quint8 category = 0;
				stream >> p >> m >> type >> category
					>> failure.name >> failure.reason;
				failure.id = { PeerId(p), MsgId(m) };
				failure.type = Type(type);
				failure.category = FailureCategory(category);
				job->_current.failures.push_back(std::move(failure));
			}
			if (version >= 2) {
				stream >> job->_current.alreadySaved;
			}
			if (stream.status() != QDataStream::Ok) return;
			attach(job);
			_jobs.push_back(std::move(job));
		}
	}

	const not_null<Main::Session*> _session;
	std::vector<std::shared_ptr<Job>> _jobs;
	std::vector<QString> _attention;
	base::Timer _persistTimer;
};

[[nodiscard]] inline QString Title(
		const Storage::SharedMediaTypesMask &types) {
	if (types.contains(Type::Photo) && types.contains(Type::Video)) {
		return u"Save all media"_q;
	}
	return types.contains(Type::Video)
		? u"Save all videos"_q
		: u"Save all photos"_q;
}

[[nodiscard]] inline QString CompletionText(const Progress &progress) {
	if (!progress.saved && !progress.alreadySaved && !progress.failed) {
		return progress.cancelled
			? u"Saving stopped before any media was saved."_q
			: u"No saveable media found."_q;
	}
	if (!progress.saved && !progress.alreadySaved) {
		return u"No media was saved. %1 skipped • %2 filtered • %3 failed."_q
			.arg(progress.skipped)
			.arg(progress.filteredOut)
			.arg(progress.failed);
	}
	if (progress.cancelled) {
		return u"Stopped after saving %1 items • %2 already saved • %3 skipped • %4 filtered • %5 failed"_q
			.arg(progress.saved)
			.arg(progress.alreadySaved)
			.arg(progress.skipped)
			.arg(progress.filteredOut)
			.arg(progress.failed);
	}
	return u"Saved %1 items • %2 already saved • %3 skipped • %4 filtered • %5 failed"_q
		.arg(progress.saved)
		.arg(progress.alreadySaved)
		.arg(progress.skipped)
		.arg(progress.filteredOut)
		.arg(progress.failed);
}

inline void StartJob(
		not_null<Window::SessionController*> controller,
		Request request) {
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
		[weak, request = std::move(request)](QString &&result) mutable {
			const auto controller = weak.get();
			if (!controller || result.isEmpty()) {
				return;
			}
			request.destination = result.endsWith('/')
				? std::move(result)
				: (std::move(result) + '/');
			if (!QDir().mkpath(request.destination)) {
				controller->showToast(u"Could not create the destination folder."_q);
				return;
			}

			const auto title = Title(request.types);
			const auto destination = request.destination;
			const auto job = controller->session().bulkSave().create(
				controller,
				std::move(request));
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

inline void Start(
		not_null<Window::SessionController*> controller,
		Request request) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Save media"_q));
		const auto photos = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Photos"_q,
			request.types.contains(Type::Photo),
			st::defaultBoxCheckbox));
		const auto videos = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Videos"_q,
			request.types.contains(Type::Video),
			st::defaultBoxCheckbox));
		const auto from = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"From date (YYYY-MM-DD), optional"_q)));
		const auto to = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"To date (YYYY-MM-DD), optional"_q)));
		const auto minimum = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Minimum size in bytes, optional"_q)));
		const auto maximum = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Maximum size in bytes, optional"_q)));
		const auto sender = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Sender peer ID, optional"_q)));
		const auto layoutGroup = std::make_shared<Ui::RadioenumGroup<Layout>>(
			request.layout);
		box->addRow(object_ptr<Ui::Radioenum<Layout>>(
			box, layoutGroup, Layout::Flat, u"Flat"_q));
		box->addRow(object_ptr<Ui::Radioenum<Layout>>(
			box, layoutGroup, Layout::Type, u"By media type"_q));
		box->addRow(object_ptr<Ui::Radioenum<Layout>>(
			box, layoutGroup, Layout::YearMonth, u"By year and month"_q));
		box->addRow(object_ptr<Ui::Radioenum<Layout>>(
			box, layoutGroup, Layout::Topic, u"By topic"_q));
		box->addButton(rpl::single(u"Save"_q), [=] {
			if (!photos->checked() && !videos->checked()) {
				return;
			}
			const auto parseDate = [](const QString &text) {
				const auto value = text.trimmed();
				return value.isEmpty()
					? QDate()
					: QDate::fromString(value, Qt::ISODate);
			};
			const auto parseSize = [](const QString &text) {
				bool ok = false;
				const auto value = text.trimmed().toLongLong(&ok);
				return (ok && value > 0) ? value : int64(0);
			};
			const auto fromDate = parseDate(from->getLastText());
			const auto toDate = parseDate(to->getLastText());
			const auto minSize = parseSize(minimum->getLastText());
			const auto maxSize = parseSize(maximum->getLastText());
			bool senderOk = false;
			const auto senderValue = sender->getLastText().trimmed();
			const auto senderId = senderValue.isEmpty()
				? PeerId()
				: PeerId(PeerIdHelper(senderValue.toULongLong(&senderOk)));
			if ((!from->getLastText().trimmed().isEmpty() && !fromDate.isValid())
				|| (!to->getLastText().trimmed().isEmpty() && !toDate.isValid())
				|| (fromDate.isValid() && toDate.isValid() && fromDate > toDate)) {
				from->showError();
				to->showError();
				return;
			}
			if ((!minimum->getLastText().trimmed().isEmpty() && !minSize)
				|| (!maximum->getLastText().trimmed().isEmpty() && !maxSize)
				|| (minSize && maxSize && minSize > maxSize)) {
				minimum->showError();
				maximum->showError();
				return;
			}
			if (!senderValue.isEmpty() && !senderOk) {
				sender->showError();
				return;
			}
			request.types = Storage::SharedMediaTypesMask{};
			if (photos->checked()) {
				request.types.added(Type::Photo);
			}
			if (videos->checked()) {
				request.types.added(Type::Video);
			}
			request.fromDate = fromDate;
			request.toDate = toDate;
			request.minimumSize = minSize;
			request.maximumSize = maxSize;
			request.senderId = senderId;
			request.layout = layoutGroup->current();
			box->closeBox();
			StartJob(controller, std::move(request));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace Info::Media::BulkSave
