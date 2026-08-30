/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/base_file_utilities.h"
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
#include "history/history_item.h"
#include "history/view/history_view_context_menu.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <deque>
#include <set>

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
};

struct Progress {
	int discovered = 0;
	int saved = 0;
	int skipped = 0;
	int failed = 0;
	std::optional<int> total;
	std::vector<DownloadProgress> active;
	QString lastSavedPath;
	bool finished = false;
	bool cancelled = false;
};

class Job final {
public:
	Job(not_null<Main::Session*> session, Scope scope)
	: _session(session)
	, _scope(std::move(scope)) {
	}

	void start() {
		if (_started || _cancelled) {
			return;
		}
		_started = true;

		_session->data().photoLoadProgress(
		) | rpl::on_next([=](not_null<PhotoData*> photo) {
			if (hasActivePhoto(photo)) {
				checkActive();
			}
		}, _lifetime);
		_session->data().documentLoadProgress(
		) | rpl::on_next([=](not_null<DocumentData*> document) {
			if (hasActiveDocument(document)) {
				checkActive();
			}
		}, _lifetime);
		_session->downloaderTaskFinished(
		) | rpl::on_next([=] {
			checkActive();
		}, _lifetime);

		loadPage(ServerMaxMsgId - 1);
		publish();
	}

	void cancel() {
		if (_cancelled || _current.finished) {
			return;
		}
		_cancelled = true;
		_pageLifetime.destroy();
		_pending.clear();
		_current.cancelled = true;
		publish();
		_lifetime.destroy();
	}

	[[nodiscard]] bool finished() const {
		return _current.finished || _cancelled;
	}

	[[nodiscard]] rpl::producer<Progress> progressValue() const {
		return rpl::single(_current) | rpl::then(_updates.events());
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
			suffix.isEmpty() ? QString() : (u"."_q + suffix),
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
		if (_cancelled || _enumerationDone || _pageLoading) {
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
		std::move(viewer
		) | rpl::filter([](const SparseIdsMergedSlice &slice) {
			return slice.fullCount().has_value()
				&& slice.skippedBefore().has_value()
				&& slice.skippedAfter().has_value();
		}) | rpl::take(1) | rpl::on_next([=](SparseIdsMergedSlice slice) {
			_pageLoading = false;
			handlePage(std::move(slice));
		}, _pageLifetime);
	}

	void handlePage(SparseIdsMergedSlice slice) {
		if (_cancelled) {
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
		if (!item
			|| item->forbidsForward()
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
				++_current.failed;
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
			_active.push_back({
				.id = id,
				.name = QFileInfo(path).fileName(),
				.path = path,
				.date = item->date(),
				.document = document,
			});
			document->save(id, path);
			return StartResult::Started;
		}

		++_current.skipped;
		return StartResult::Terminal;
	}

	void fillSlots() {
		if (_cancelled || _filling) {
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

		checkActive();
		if (!_cancelled
			&& _pending.empty()
			&& _active.size() < kMaxActive
			&& !_enumerationDone
			&& !_pageLoading) {
			loadPage(_nextAroundId);
		}
		finishIfDone();
		publish();
	}

	void checkActive() {
		if (_cancelled || _checking) {
			return;
		}
		_checking = true;
		for (auto i = _active.begin(); i != _active.end();) {
			auto saved = false;
			auto failed = false;
			if (i->photo) {
				if (!i->photo->loading()) {
					saved = i->photoView->saveToFile(i->path);
					failed = !saved;
				}
			} else if (i->document) {
				if (!i->document->loading()) {
					saved = QFileInfo::exists(i->path);
					failed = !saved;
				}
			}
			if (!saved && !failed) {
				++i;
				continue;
			}
			if (saved) {
				setFileDates(i->path, i->date);
				_current.lastSavedPath = i->path;
				++_current.saved;
			} else {
				++_current.failed;
			}
			i = _active.erase(i);
		}
		_checking = false;
		publish();

		if (!_filling) {
			fillSlots();
		}
	}

	void finishIfDone() {
		if (_cancelled
			|| _current.finished
			|| !_enumerationDone
			|| _pageLoading
			|| !_pending.empty()
			|| !_active.empty()) {
			return;
		}
		_current.finished = true;
		publish();
	}

	void publish() {
		_current.active.clear();
		_current.active.reserve(_active.size());
		for (const auto &active : _active) {
			auto progress = DownloadProgress();
			progress.name = active.name;
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
	const Scope _scope;
	Progress _current;
	std::deque<FullMsgId> _pending;
	std::vector<Active> _active;
	std::set<std::pair<uint64, int64>> _seen;
	MsgId _nextAroundId = ServerMaxMsgId - 1;
	bool _started = false;
	bool _cancelled = false;
	bool _enumerationDone = false;
	bool _pageLoading = false;
	bool _filling = false;
	bool _checking = false;
	rpl::event_stream<Progress> _updates;
	rpl::lifetime _pageLifetime;
	rpl::lifetime _lifetime;

};

[[nodiscard]] inline QString Title(Type type) {
	switch (type) {
	case Type::Photo: return u"Save all photos"_q;
	case Type::Video: return u"Save all videos"_q;
	case Type::PhotoVideo: return u"Save all media"_q;
	default: return u"Save all media"_q;
	}
}

[[nodiscard]] inline QString Summary(Progress progress) {
	const auto count = progress.total;
	auto result = count
		? u"%1 / %2 saved"_q.arg(progress.saved).arg(*count)
		: u"%1 saved • %2 discovered"_q
			.arg(progress.saved)
			.arg(progress.discovered);
	if (progress.skipped || progress.failed) {
		result += u"\n%1 skipped • %2 failed"_q
			.arg(progress.skipped)
			.arg(progress.failed);
	}
	return result;
}

[[nodiscard]] inline QString ActiveText(Progress progress) {
	if (progress.active.empty()) {
		return progress.finished
			? QString()
			: u"Preparing downloads…"_q;
	}
	constexpr auto kVisible = 4;
	auto lines = QStringList();
	const auto count = std::min(int(progress.active.size()), kVisible);
	for (auto i = 0; i != count; ++i) {
		const auto &active = progress.active[i];
		auto status = QString();
		if (active.total > 0 && active.ready > 0) {
			status = Ui::FormatProgressText(active.ready, active.total);
		} else if (active.progress > 0.) {
			status = u"%1%"_q.arg(int(active.progress * 100.));
		} else {
			status = u"downloading…"_q;
		}
		lines.push_back(active.name + u" — "_q + status);
	}
	if (progress.active.size() > kVisible) {
		lines.push_back(u"+ %1 more downloading"_q.arg(
			progress.active.size() - kVisible));
	}
	return lines.join(u'\n');
}

[[nodiscard]] inline QString CompletionText(const Progress &progress) {
	if (!progress.saved && !progress.failed) {
		return u"No saveable media found."_q;
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

			const auto job = std::make_shared<Job>(
				&controller->session(),
				std::move(scope));
			const auto folder = job->progressValue(
				) | rpl::take(1) | rpl::map([=](const Progress &) {
					return QString();
				});
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				box->setTitle(Title(scope.type));
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					job->progressValue() | rpl::map(Summary),
					st::boxLabel));
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					job->progressValue() | rpl::map(ActiveText),
					st::boxLabel));
				job->progressValue(
				) | rpl::filter([](const Progress &progress) {
					return progress.finished;
				}) | rpl::take(1) | rpl::on_next([=](const Progress &progress) {
					if (const auto controller = weak.get()) {
						const auto reveal = progress.lastSavedPath.isEmpty()
							? scope.destination
							: progress.lastSavedPath;
						controller->showToast({
							.text = CompletionText(progress),
							.filter = [reveal](const auto ...) {
								File::ShowInFolder(reveal);
								return false;
							},
						});
					}
					box->closeBox();
				}, box->lifetime());
				box->addButton(tr::lng_cancel(), [=] {
					job->cancel();
					box->closeBox();
				});
				box->lifetime().add([job] {
					if (!job->finished()) {
						job->cancel();
					}
				});
			}));
			job->start();
		});
}

} // namespace Info::Media::BulkSave
