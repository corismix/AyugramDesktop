# Phase 6 — Export Chat History investigation

Status: source audit and benchmark protocol complete. No exporter code was
changed, and no live performance measurements were run.

## Scope and conclusion

This investigation covers the existing Export Chat History implementation only.
It does not change the Bulk Media Save job, its normal media-save pipeline, or
any takeout behavior. Bulk Save should continue to use the ordinary
`PhotoMedia` / `DocumentData` save paths regardless of the findings here.

The source shows that Export Chat History is a separate takeout-based
pipeline. There is not enough runtime evidence in this checkout to claim that
it can be made faster safely. Any optimization should therefore be a separate
project with controlled measurements and explicit server-behavior safeguards.

## Source-verified findings

### Takeout request sequence

1. Export first retrieves the current user, then calls
   `account.initTakeoutSession` with flags derived from the selected export
   types and media size limit.
2. The returned takeout ID is stored in `Export::ApiWrap`.
3. Subsequent export requests are wrapped in `invokeWithTakeout`; message-range
   requests add `invokeWithMessagesRange` where required.
4. Export finishes with `account.finishTakeoutSession(success)`. Fast cancel
   sends `account.finishTakeoutSession` without the success flag and detaches
   the request.

Relevant implementation: `Telegram/SourceFiles/export/export_api_wrap.cpp`,
`startMainSession`, `mainRequest`, `finishExport`, and `cancelExportFast`.

### File concurrency and transfer behavior

- The exporter uses `MTP::ConcurrentSender`, but its `ApiWrap` owns one
  `FileProcess` at a time.
- Each file is split into 8 × 128 KiB chunks for requests to
  `upload.getFile`.
- `loadFilePart()` refuses to issue a request while the current file has a
  request ID, so only one file-part request is in flight in the current code.
- `kFileRequestsCount` is set to 3, but the adjacent pipelining code is
  disabled and documents that only one request at a time is supported.
- Message slices and media lists are requested in batches of 100, while file
  loading proceeds serially through the slice’s file work.

This proves the current scheduling shape, not its throughput, server-side
limits, or the optimal safe concurrency.

### CDN and data-center handling

- Control-plane takeout requests are routed through the export DC shift.
- File requests use the media DC shift derived from each file location.
- `upload.getFile` results of type `upload.fileCdnRedirect` are rejected with
  `Cdn redirect is not supported.`; there is no CDN hash/decryption follow-up
  in this implementation.
- Invalid or unavailable locations are treated as unavailable. File-reference
  failures trigger source-message/reference refresh paths where the origin can
  be resolved.

The source therefore identifies CDN redirects as an explicit unsupported path;
it does not establish how often Telegram returns that path for export media.

### Premium and non-Premium behavior

No Premium-specific branch was found in `Export::ApiWrap`, export settings, or
the export manager. The implementation passes the configured media size limit
to `account.initTakeoutSession` and relies on the server response. Account
tier, entitlement, and any server-side export policy are consequently runtime
variables, not behavior that can be inferred from this client source.

### Rate limits and takeout-session errors

- Export errors are emitted through `ApiWrap::errors()` and displayed by the
  export panel.
- `TAKEOUT_INVALID` gets a dedicated invalid-session message.
- `TAKEOUT_INIT_DELAY_<seconds>` is converted into a retry-available time and
  persisted as an export suggestion.
- Other API errors are shown as critical errors with code, type, and
  description.
- The inspected export code does not provide a dedicated concurrency tuner or
  an experiment-specific rate-limit budget.

These are client handling paths. They are not measurements of server quotas
or evidence that a particular parallelism level is safe.

## Reproducible benchmark protocol

Run this only in a dedicated test checkout with disposable export output and
accounts approved for the test. Do not use production chat data as the sole
fixture. Keep each trial on a fresh export session and record the client build,
account tier, data-center location, network condition, and server errors.

### Matrix

| Variable | Required cases |
| --- | --- |
| Account | Premium and non-Premium, otherwise matched |
| Chat | private chat, group, channel, and topic where available |
| Media | photos, small videos, large videos, mixed media |
| Size | below the configured limit, near the limit, and rejected/oversized |
| Connection/DC | stable low-latency, constrained bandwidth, and each observed media DC |
| Concurrency | current serial baseline, then one controlled candidate level at a time |
| Repetition | at least three runs per comparable cell, with a fresh session |

### Measurements

Capture:

- time to initialize and finish the takeout session;
- total wall-clock time and payload size;
- per-file and aggregate throughput;
- number of simultaneous `upload.getFile` requests;
- CDN redirects, file-reference refreshes, retries, and unavailable files;
- API error type/code, including takeout delays, flood waits, and session
  invalidation;
- UI responsiveness and disk-write failures;
- whether Premium status changes accepted size, throughput, or failure rates.

Compare any candidate concurrency only against the unchanged serial baseline.
Stop a trial if errors, takeout delays, or rate-limit responses increase, and
do not interpret a faster single run as evidence of a safe production policy.

### Evidence format

Store one machine-readable row per trial with:

```text
build, account_tier, chat_kind, media_fixture, size_limit,
network_profile, control_dc, media_dc, concurrency,
duration_seconds, bytes, throughput, max_in_flight,
cdn_redirects, reference_refreshes, retries, failures, error_types
```

Attach client logs and the exact export settings to each run. Report medians
and ranges rather than a single best run, and distinguish client-observed
values from server-side limits that are only inferred.

## Current validation boundary

Live measurements were not performed. This checkout must not be built without
explicit authorization, and no approved test-account fixtures were provided
for takeout experiments. The findings above are source-verified; concurrency
optimality, CDN incidence, Premium differences, throughput, and rate-limit
consequences remain unverified.

## Decision gate for a separate exporter project

Do not modify `export_api_wrap.cpp` as part of Bulk Media Save. Open a separate
exporter project only if the benchmark demonstrates a repeatable bottleneck,
identifies a server-safe change, and shows no unacceptable increase in
takeout/session errors, CDN failures, or rate-limit responses. Any resulting
implementation must retain explicit cancellation, reference refresh, and
takeout-session cleanup behavior.
