# Bulk Media Save — Implementation Plan

## Goal

Add a fast bulk-save feature to AyuGram Desktop that can save photos and videos from a Telegram conversation scope without using Telegram's slow **Export Chat History** download path.

The feature should reuse Telegram Desktop's normal media download/save pipeline—the same family of code used by **Save As** / **Download selected**—so normal downloader concurrency, CDN handling, Premium behavior, cache reuse, and existing media restrictions continue to apply.

Primary target: macOS, while keeping the implementation cross-platform unless a platform-specific file-dialog detail requires otherwise.

---

## Why this is a separate feature from Export Chat History

Do **not** implement this by modifying `export/export_api_wrap.cpp` as the first approach.

The exporter uses Telegram's takeout/export path and serializes file-part requests. Normal media downloads use a separate downloader with adaptive pipelining and multiple sessions. The existing selected-media saver already reaches that normal download path.

The desired architecture is therefore:

```text
conversation / topic scope
        ↓
Shared Media enumeration
        ↓
filter to saveable photos/videos
        ↓
bounded bulk-save queue
        ↓
existing PhotoMedia / DocumentData save APIs
        ↓
normal Telegram downloader
        ↓
destination folder
```

This feature is a media downloader/saver, **not** a chat-history exporter. It should not generate HTML/JSON, message text, contacts, metadata archives, or other export artifacts.

---

## Existing code to build on

### Existing selected-media saver

`Telegram/SourceFiles/menu/menu_item_download_files.cpp`

This already contains the important save behavior we want to reuse:

- selected `HistoryItem`s are classified as photos or documents;
- photos use `PhotoData::createMediaView()` and `PhotoMedia::wanted(...)`;
- downloaded photos are written with `PhotoMedia::saveToFile(...)`;
- documents/videos use `DocumentData::save(...)` or `DocumentSaveClickHandler::SaveAndTrack(...)`;
- one destination folder can be selected for the whole operation;
- file dates can be restored from Telegram media/message dates;
- protected/non-forwardable media is rejected by the existing selection path.

Do not create a new MTProto file downloader.

### Shared Media scope

`Telegram/SourceFiles/info/profile/tabs/adapters/info_profile_tab_media.cpp`

`MediaTabAdapter` already owns the scope information needed for the initial feature:

- `_context.peer` — current chat/group/channel;
- `_topicRootId` — current forum topic when present, otherwise zero;
- `_monoforumPeerId` — saved-message/sublist scope when applicable;
- `_context.migrated` — migrated peer support;
- `_type` — `Photo`, `Video`, or combined `PhotoVideo` media tab.

This makes the Shared Media tab's overflow menu the lowest-risk MVP entry point.

### Shared Media enumeration

Relevant files:

```text
Telegram/SourceFiles/data/data_shared_media.h
Telegram/SourceFiles/data/data_shared_media.cpp
Telegram/SourceFiles/storage/storage_shared_media.h
Telegram/SourceFiles/storage/storage_facade.h
```

Useful existing APIs/types include:

```text
Storage::SharedMediaType
Storage::SharedMediaKey
Storage::SharedMediaQuery
SharedMediaViewer(...)
SharedMediaMergedViewer(...)
SharedMediaWithLastViewer(...)
```

The implementation should follow the same viewer/loading path already used by Telegram's Shared Media UI so missing history slices are fetched from Telegram when necessary.

**Do not treat `Storage::Facade::snapshot(...)` as a complete-chat result.** The local sparse list may contain only currently loaded slices.

---

# MVP

## User-visible behavior

Add **Save all…** to the overflow menu of the Shared Media grid.

Its meaning is determined by the current tab:

| Current Shared Media tab | MVP action |
| --- | --- |
| Photos | Save all photos in this scope |
| Videos | Save all videos in this scope |
| Media / PhotoVideo | Save all photos and videos in this scope |

The scope must match what the user is currently viewing:

- private chat;
- basic group;
- supergroup;
- channel;
- current forum topic when inside a topic;
- whole peer when not scoped to a topic;
- migrated history where Telegram's existing Shared Media view includes it.

The user chooses one destination folder. The operation then discovers and saves every eligible item from that scope using the normal downloader.

### Required MVP UI

1. User opens Shared Media.
2. User opens the overflow menu.
3. User chooses **Save all…**.
4. Show a confirmation/folder-selection flow that clearly names the current media type and scope.
5. Save into one chosen directory.
6. Show a compact progress UI while running that includes aggregate batch progress **and visibly shows the media currently downloading**, with per-active-download progress when the underlying media APIs expose it.
7. Provide **Cancel**.
8. On completion, show saved / skipped / failed counts and an action to reveal the destination folder.

Do not require the user to scroll through Shared Media first.

---

## Exact MVP boundary

> **MVP STOPS HERE:** save every photo and/or video represented by the current Shared Media tab, from the current peer/topic scope, into one destination folder, with bounded downloading, a progress UI that visibly shows active downloads, cancellation, sane filenames, restriction checks, and a completion/error summary.

The following are **explicitly not required for MVP**:

- a new action directly in the main chat three-dot menu;
- a custom filter dialog;
- arbitrary date ranges;
- "all topics" recursion from inside a forum;
- GIFs, generic files, music, voice messages, round videos, or stickers;
- pause/resume;
- persistence across app restarts;
- a download-job history screen;
- duplicate-content hashing;
- folder organization by year/month/type;
- custom filename templates;
- HTML/JSON/message-text export;
- optimizing `export_api_wrap.cpp`;
- bypassing protected-content, TTL, or other Telegram restrictions.

Do not expand the first implementation beyond this boundary just because the architecture can support more.

---

# MVP technical design

## 1. Add a reusable bulk-save job instead of selecting every message in UI

Do not simulate scrolling, UI selection, or "Select all".

Create a small non-visual job/controller responsible for:

```text
Discover → Validate → Queue → Download/Save → Finish
                  ↘ Cancel
```

Suggested names, not mandatory:

```text
BulkMediaSaveJob
BulkMediaSaveScope
BulkMediaSaveOptions
BulkMediaSaveProgress
```

Suggested home:

```text
Telegram/SourceFiles/info/media/info_media_bulk_save.h
Telegram/SourceFiles/info/media/info_media_bulk_save.cpp
```

If dependency direction makes `info/media` inappropriate, place the reusable job next to the existing media-save/download helpers instead. Keep the orchestration independent from the Shared Media widget.

### Scope object

The job should receive enough information to describe the existing Shared Media scope without depending on widget lifetime:

```text
peer / peerId
topicRootId
monoforumPeerId
migratedPeer / migratedPeerId
SharedMediaType or SharedMediaTypesMask
destination directory
```

Do not retain raw widget pointers as job state.

---

## 2. Enumerate media through Shared Media's existing data layer

The job must enumerate message IDs through the same Shared Media machinery used by Telegram's media UI.

Requirements:

- support unloaded/remote slices;
- preserve topic filtering via `topicRootId`;
- preserve migrated history behavior;
- work when the Shared Media grid has never been scrolled;
- page incrementally instead of materializing a giant chat into memory;
- avoid duplicate IDs when merged/migrated slices overlap or update during the run.

Prefer adapting the established `SharedMediaMergedViewer(...)` / `SharedMediaWithLastViewer(...)` pattern rather than issuing raw `messages.search` / MTProto requests from the new feature.

### Pagination target

Use a moderate page size, e.g. 50–200 IDs, then feed eligible items into a bounded save queue. The exact number should be chosen after inspecting the viewer's existing usage patterns; it should not be a magic performance assumption.

### Stable run semantics

For MVP, treat the run as a snapshot-like traversal of the history available while enumeration proceeds. New media arriving during the save does not need to be chased indefinitely.

Use a monotonic cursor/message-ID traversal and a de-duplication set for IDs already scheduled.

---

## 3. Reuse normal save primitives

Before implementing the bulk job, refactor the reusable parts of:

`Telegram/SourceFiles/menu/menu_item_download_files.cpp`

The goal is for **Download selected** and **Save all…** to share media classification and individual save behavior instead of creating two subtly different implementations.

Good extraction candidates:

- classify a `HistoryItem` as a saveable photo/document;
- resolve media/message date;
- start/save a photo through `PhotoMedia`;
- start/save a document/video through `DocumentData`;
- filename sanitization/collision handling;
- the common restriction check.

Do not change the downloader protocol or duplicate `storage/download_manager_mtproto.*`.

### Media types for MVP

Allow only:

- `Storage::SharedMediaType::Photo`;
- `Storage::SharedMediaType::Video`;
- `Storage::SharedMediaType::PhotoVideo`.

A `PhotoVideo` run should accept photos plus documents that the Shared Media layer classifies as video. It must not accidentally include every generic document.

---

## 4. Keep the queue bounded

The existing selected-media code can reasonably schedule a user-selected set all at once. A whole channel may contain tens of thousands of items, so the bulk job must not do this.

MVP requirement:

- enumerate incrementally;
- keep only a bounded number of not-yet-finished media active;
- let Telegram's existing downloader manage network-level concurrency;
- refill the queue as active items finish;
- release media views/task state after completion.

Initial implementation target: approximately 8–32 active save items, but choose the final bound based on behavior during testing rather than trying to out-tune Telegram's downloader.

The job's concurrency bound is about memory/lifecycle control, **not** about replacing Telegram's network scheduler.

---

## 5. Completion and transfer-progress tracking

Do not treat "scheduled" as "saved".

Track at least:

```text
discovered
eligible
queued
saved
skipped
failed
active
```

For each active item, expose enough state for the MVP progress UI to identify the current download and, where supported by Telegram's existing media/download APIs, show transfer progress. Prefer existing reactive/loading progress values rather than building a second byte-accounting layer.

Suggested per-active-item presentation state:

```text
FullMsgId / stable job item id
media type
output/display filename
loaded bytes or normalized progress, if available
total bytes, if known
terminal/error state
```

The UI does **not** need exact byte progress for a media type if the existing normal save primitive does not expose it cleanly. In that case it must still visibly list the item as actively downloading and use an indeterminate indicator. Do not replace the normal downloader merely to obtain progress metrics.

If several downloads are active concurrently, the UI should be able to show several active rows rather than pretending only one file is downloading.

Use existing downloader/media state signals where possible. `Main::Session::downloaderTaskFinished()` can wake the job to re-check active media, but the job should determine which of its own items actually completed.

Do not cancel, attribute progress from, or claim completion for unrelated downloads occurring elsewhere in the app.

Each item should eventually reach one terminal state:

```text
saved | skipped | failed
```

---

## 6. Cancellation semantics

MVP Cancel must:

- stop enumerating additional pages;
- stop scheduling new items;
- discard queued-but-not-started work;
- release the job UI/state cleanly.

Cancel **must not** cancel unrelated Telegram downloads.

If the existing downloader does not provide a safe way to cancel only requests owned by this job, it is acceptable for already-started media downloads to finish in the background while the bulk job stops saving/scheduling new items. Document that behavior in code and UI rather than introducing global cancellation.

---

## 7. Filenames and collisions

### Documents/videos

Prefer the Telegram document's original filename after existing sanitization.

If the filename is empty or unusable, generate a stable fallback containing the message ID, for example:

```text
video_123456.mp4
```

Use the known/derived extension when available.

### Photos

Do not use `photo_1.jpg`, `photo_2.jpg`, etc. across independently paged batches because numbering becomes fragile and collisions are likely across repeated runs.

Use a deterministic message-based fallback, for example:

```text
photo_123456.jpg
```

For migrated histories where message IDs can overlap, include enough peer identity to keep the fallback unique if necessary.

### Existing destination file

For MVP, preserve existing Telegram-style collision avoidance: do not silently overwrite an unrelated file. Generate a safe alternative name using existing file-dialog/file-name helpers.

Content hashing / exact duplicate detection is post-MVP.

### File timestamps

Preserve Telegram media/message time using the same logic currently used when saving selected photos where practical.

---

## 8. Restrictions and unsupported messages

The feature must honor the same restrictions as existing manual save/download behavior.

At minimum, do not bypass:

- protected/no-forward content;
- TTL/self-destruct media;
- media that current Telegram UI refuses to save;
- invalid/deleted messages encountered during enumeration.

A restricted or vanished item should increment **skipped**, not abort the whole run.

Do not weaken Telegram's checks to make the count match the Shared Media total.

---

## 9. MVP entry point

Modify:

`Telegram/SourceFiles/info/profile/tabs/adapters/info_profile_tab_media.cpp`

`MediaTabAdapter::fillMenu(...)` already builds the overflow menu for photo/video grids. Add a **Save all…** action after the existing zoom/calendar actions.

It should construct an immutable save scope from the adapter's current context and launch the bulk-save flow.

The action should only appear for:

```text
Photo
Video
PhotoVideo
```

Do not add it to links/polls/etc. in MVP.

This entry point automatically gives us the correct current topic vs whole-chat behavior because the adapter already stores `_topicRootId`.

---

## 10. MVP progress UI

Keep v1 UI small. A dedicated heavyweight download manager is not needed, but **the active downloads themselves must be visible in the progress UI for MVP**.

Minimum state to display:

```text
Saving media…
37 / 412 saved
3 skipped • 1 failed

Downloading
holiday-video.mp4        46.2 MB / 180.0 MB   26%
photo_183492.jpg         1.8 MB / 4.1 MB      44%
clip_2026-08-14.mp4      downloading…

[Cancel]
```

The exact layout should follow existing Telegram/AyuGram UI conventions; the example is behavioral, not a visual specification.

MVP requirements for the active-download area:

- show every item currently owned by the bulk job's active queue, within a reasonable visible-row limit if needed;
- identify each item by a useful filename or deterministic fallback name;
- show determinate per-file progress when existing Telegram media/download state exposes loaded/total bytes or an equivalent normalized progress value;
- otherwise show an indeterminate **downloading…** state rather than hiding the file;
- update rows reactively while downloads advance;
- remove or transition a row when that item reaches `saved`, `skipped`, or `failed`;
- if more items are active than comfortably fit, show a compact overflow indicator such as `+ 6 more downloading` rather than expanding the dialog without bound.

A single aggregate progress bar/count by itself is **not sufficient for MVP**.

Do not add a separate network transfer implementation just to calculate progress. If Telegram's existing downloader exposes only coarse progress for a particular media type, use that coarse progress.

If the full batch total is not yet known, show discovered/saved progress without fabricating a denominator, then switch to a known total when the Shared Media data layer provides one.

At completion:

```text
Saved 408 items
3 skipped • 1 failed
[Show in Folder]
```

If every item fails, show a clear error rather than a success toast.

Use existing Telegram/AyuGram box/toast components and translation keys rather than hard-coded English in final implementation.

---

# Suggested implementation sequence

## Step 0 — Baseline and instrumentation

Before feature changes:

1. Build the current `dev` branch on macOS.
2. Verify existing single **Save As** and multi-selection **Download selected** behavior.
3. Record rough throughput for a representative large video.
4. Add temporary debug logging if needed to prove the new feature uses the normal downloader and never enters export/takeout code.

Do not keep noisy debug logging in the final commit.

## Step 1 — Extract reusable save primitives

Refactor `menu_item_download_files.*` so the existing selected-media action calls reusable save/classification helpers.

Acceptance condition: existing **Download selected** behavior is unchanged.

## Step 2 — Build Shared Media enumerator

Implement scope-based paged discovery for one `SharedMediaType`/scope.

Initially log/collect IDs only; do not download them yet.

Acceptance condition: counts and IDs match the current Shared Media tab for:

- a normal chat;
- a channel;
- a forum topic;
- a chat with enough media to require multiple pages.

## Step 3 — Add bounded bulk-save job

Connect enumerated IDs to the reusable normal-save primitives.

Add counters, per-active-item transfer state, bounded active work, error isolation, and lifecycle cleanup.

Acceptance condition: saving hundreds of mixed photos/videos does not require scrolling, memory does not scale with the whole chat history, and the job can report which items are currently downloading plus available per-item progress.

## Step 4 — Add folder picker + progress/cancel UI

Wire `MediaTabAdapter::fillMenu(...)` to the job.

The progress UI must render the active-download rows described in **MVP progress UI**, not only aggregate counters.

Acceptance condition: end-to-end MVP is usable without developer tools and the user can see which files are actively downloading and their progress where available.

## Step 5 — Harden filenames, restrictions, migration, failure paths

Cover repeated filenames, deleted messages, protected content, disconnect/reconnect, migrated history, cancellation, and destination write failures.

## Step 6 — MVP cleanup

- add translation keys;
- remove debug-only code;
- document non-obvious lifecycle choices;
- keep diffs small and upstream-rebase-friendly;
- verify macOS build and, if practical, one other desktop platform build/CI target.

**Ship MVP after Step 6. Do not begin Phase 2 before the MVP is independently usable and tested.**

---

# MVP acceptance checklist

**Completed 2026-08-31.** The MVP was built and manually verified by the user.

### Functional

- [x] Shared Media overflow shows **Save all…** on Photo/Video/PhotoVideo tabs.
- [x] Whole-chat photo save works without scrolling the grid.
- [x] Whole-chat video save works without scrolling the grid.
- [x] Combined PhotoVideo save saves both kinds and no generic files.
- [x] Current forum topic saves only that topic.
- [x] Non-topic chat saves the whole peer's matching media.
- [x] Migrated history behaves consistently with the existing Shared Media view.
- [x] Destination folder is requested once.
- [x] Existing files are not silently overwritten.
- [x] Protected/unsupported items are skipped rather than bypassed.
- [x] One failed item does not abort the batch.
- [x] Progress UI visibly shows media currently downloading.
- [x] Each visible active item shows per-file progress when the existing downloader exposes it, otherwise an indeterminate downloading state.
- [x] Multiple concurrent active downloads can be represented without unbounded UI growth.
- [x] Cancel stops discovery/new scheduling.
- [x] Completion summary reports saved/skipped/failed.

### Architecture

- [x] No call into `export/export_api_wrap.cpp`.
- [x] No new raw MTProto file downloader.
- [x] Normal `PhotoMedia` / `DocumentData` save path is reused.
- [x] Shared Media data/viewer layer performs history discovery.
- [x] Queue size is bounded.
- [x] Whole history is not materialized into `HistoryItem`/media objects at once.
- [x] The job does not depend on the media widget remaining alive.
- [x] Active-download progress is derived from existing Telegram media/downloader state rather than a parallel transfer implementation.
- [x] Existing **Download selected** still works.

### Performance

- [x] Large-video throughput is in the same general class as normal **Save As**, not Export Chat History.
- [x] Hundreds/thousands of items do not cause unbounded memory growth.
- [x] Progress updates do not materially degrade download throughput or UI responsiveness.
- [x] UI remains responsive during discovery and download.

---

# Post-MVP roadmap

## Phase 2 — Make bulk save available directly from chats/topics (complete)

**Completed 2026-08-31.** The chat/topic menu now opens a Photos/Videos chooser
and launches the MVP bulk-save job with the active peer, topic, Saved Messages
sublist, and migrated-history scope.

Add **Save media…** to a chat/topic-level menu so the user does not need to enter Shared Media first.

Suggested dialog:

```text
Save media from: [Current topic / This chat]

Types
[x] Photos
[x] Videos
[ ] GIFs
[ ] Files

Range
(o) All time
( ) Date range

Destination: …

[Cancel] [Save]
```

For the first Phase 2 increment, expose only Photos/Videos even if the dialog architecture anticipates more types.

Reuse the exact same `BulkMediaSaveScope` and job. The entry point should contain no downloader logic.

### Chat/topic menu integration

Investigate the current history/header menu context and derive:

- peer;
- active forum topic root ID;
- migrated peer where applicable.

Do not duplicate topic rules already represented by Shared Media context objects if a reusable scope builder can be extracted.

---

## Phase 3 — Filters and organization

Add optional filters without changing the downloader core:

- date from/to;
- photos only / videos only / both;
- sender filter where meaningful;
- minimum/maximum file size;
- current topic vs all topics;
- optionally only items not already saved.

Add organization options:

```text
Flat folder
By media type
By year/month
By topic
```

Add filename templates only if there is a real need. Keep message ID available as a stable uniqueness component.

---

## Phase 4 — More Telegram media types

Extend the same framework to:

- GIFs/animations;
- generic files;
- music/audio;
- voice messages;
- round video messages.

Treat each Shared Media type as an explicit adapter/classification case. Do not assume every `DocumentData` is interchangeable from a UX or filename perspective.

Be cautious with voice-message bulk saving on macOS and test it independently before enabling it.

---

## Phase 5 — Better job control

Add:

- Pause / Resume;
- Retry failed;
- persistent job state across application restart;
- resumable enumeration cursor;
- a small bulk-download jobs/history surface;
- background completion notifications where appropriate.

Persist identifiers/options, not large media object graphs.

On resume, re-resolve media from message IDs through the session data layer.

---

## Phase 6 — Duplicate awareness and manifests

Optional advanced behavior:

- skip exact files already present using size/hash where available;
- write a lightweight manifest mapping `FullMsgId → output path`;
- allow "resume this folder" without re-saving completed items;
- detect files moved/renamed since the previous run.

Do not hash every large video in the critical path unless the user enables duplicate checking; filesystem hashing can itself become the bottleneck.

---

## Phase 7 — Optional export-speed investigation

Only after the bulk saver is complete, treat faster **Export Chat History** as a separate project.

Possible research:

- make export file-part concurrency real rather than nominal;
- determine whether takeout sessions impose server-side throughput limits;
- investigate CDN redirect support in the exporter;
- compare Premium and non-Premium behavior;
- determine whether exporter concurrency changes are safe for Telegram rate limits and takeout sessions.

Do not make the bulk-save feature depend on this work. The bulk saver solves the user's media-download use case even if Telegram's exporter remains slow forever.

---

# Likely files touched by MVP

Exact placement can change during implementation, but keep the initial diff concentrated around these areas:

```text
Telegram/SourceFiles/menu/menu_item_download_files.cpp
Telegram/SourceFiles/menu/menu_item_download_files.h
Telegram/SourceFiles/info/profile/tabs/adapters/info_profile_tab_media.cpp
Telegram/SourceFiles/data/data_shared_media.*          # preferably consume, avoid invasive changes
Telegram/SourceFiles/storage/storage_shared_media.*    # preferably consume, avoid invasive changes
Telegram/SourceFiles/info/media/info_media_bulk_save.* # proposed new job/controller
Telegram/SourceFiles/lang/...                          # translation keys, exact source per project convention
Telegram/CMakeLists.txt or source manifests            # only if required for new files
```

Prefer consuming Shared Media APIs over modifying them. A new generic API is justified only if the existing viewer interfaces cannot support finite forward/backward enumeration cleanly.

---

# Design constraints for AI coding agents

When handing this plan to Codex/another agent, keep these constraints explicit:

1. **Inspect existing code before inventing APIs.** Names in this document marked as suggested are architectural labels, not guaranteed existing symbols.
2. **Do not optimize Export Chat History.** The feature must use normal media download/save behavior.
3. **Do not issue raw MTProto file downloads.** Reuse Telegram's media/data layers.
4. **Do not fake Select All by scrolling the UI.** Enumerate through Shared Media data.
5. **Do not enqueue the entire channel at once.** Bound active work.
6. **Do not bypass copy/TTL restrictions.** Match existing UI behavior.
7. **Do not make the job depend on a widget lifetime.** Capture a value-like scope.
8. **Do not silently overwrite destination files.** Use existing safe naming helpers.
9. **The MVP progress UI must show active downloads.** Aggregate counts alone are insufficient; expose per-active-item identity and existing downloader progress where available.
10. **Do not create a second downloader merely for progress reporting.** Use existing Telegram media/download state; indeterminate active rows are acceptable where precise byte progress is not exposed.
11. **Keep upstream/AyuGram rebases in mind.** Prefer additive files and small touch points in heavily modified upstream files.
12. **Stop at the MVP boundary before adding filters or more media types.**

---

# Definition of success

The core success test is simple:

> From a large chat/channel/topic, a user can save all photos and/or videos in one operation, without scrolling/selecting them manually, see which media are actively downloading and their progress where available, and downloads run through the same normal Telegram media pipeline that makes manual Save As substantially faster than Export Chat History.

Everything after that is enhancement work.
