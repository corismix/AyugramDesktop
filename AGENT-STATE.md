# Current State

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

Source validation passed; build and runtime reproduction remain pending because
the repository guide says not to build this checkout unless explicitly asked.
