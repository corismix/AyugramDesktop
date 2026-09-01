# Current State

The pasted macOS compiler failure was fixed in the bulk-save code: enum-mask
membership now uses `test()`, checkpoint persistence uses the typed account
preference API, the Save callback owns a mutable request copy, and the bulk-save
header includes the concrete `PeerData` definition. Source checks and `git
diff --check` pass, and the user-verified Debug build now completes. Runtime
behavior remains unverified.

Phase 4 implementation is present and build-verified but not runtime-verified.
The
bulk-save job now has explicit running/pausing/paused/cancelling/finished
states, retryable failure IDs, manager ownership from `Main::Session`,
account-local versioned checkpoints, restored-paused jobs, and a main-menu jobs
surface. Existing phase-3 filters/layouts and normal Telegram media save paths
remain in the same job. Source checks passed; compilation, relaunch restore,
pause/resume, retry, and manual UI behavior remain pending because this
checkout must not be built without explicit authorization.

Bulk Media Save MVP and Phase 2 are complete. A macOS crash report showed
stack exhaustion while paginating cached media: synchronous RPL emissions
re-entered `loadPage()` through `handlePage()` and `fillSlots()`. The page
handler now runs on the queued main event loop, breaking that recursion.

Bulk-save filenames append `_m<message-id>` after the generated timestamp and
before the extension, with final-name collision checking. The final resolver
now receives a full destination path; passing `skipExistance` to
`filedialogDefaultName` had reduced that path to a bare filename and redirected
saves to the process working directory. Discovered messages expand through
`Session::itemOrItsGroup()` so loaded album members are queued together and
deduplicated.

Source validation and the user-run Debug build passed; runtime reproduction
remains pending.
