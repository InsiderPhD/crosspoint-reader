#include "SessionJournal.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

#include "StatsPayload.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "util/TimeUtils.h"

namespace SessionJournal {
namespace {
constexpr char OUTBOX[] = "/.crosspoint/kosync_sessions.jsonl";
constexpr char OUTBOX_TMP[] = "/.crosspoint/kosync_sessions.tmp";
constexpr uint32_t MAX_SPAN_SECONDS = 7 * 24 * 3600;  // server limit
// accepted_session_ids for 20 ids of ~22 chars plus the envelope.
constexpr size_t REPLY_CAPACITY = 1024;
constexpr size_t MAX_LINE = 600;  // a path is at most ~255 bytes

// Local offset from UTC at `t`, in minutes (no tm_gmtoff on newlib).
int32_t utcOffsetMinutes(const time_t t) {
  tm local = {};
  tm utc = {};
  if (!localtime_r(&t, &local) || !gmtime_r(&t, &utc)) return 0;
  int32_t minutes = (local.tm_hour - utc.tm_hour) * 60 + (local.tm_min - utc.tm_min);
  const int dayDelta = local.tm_yday - utc.tm_yday;
  if (dayDelta == 1 || dayDelta < -1) minutes += 24 * 60;  // local is a day ahead (or year wrapped)
  if (dayDelta == -1 || dayDelta > 1) minutes -= 24 * 60;
  return minutes;
}

std::string documentIdFor(const std::string& path) {
  return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(path)
             : KOReaderDocumentId::calculate(path);
}

// Reads one '\n'-terminated line into `out`. False at end of file or for a
// line too long to be ours (skipped).
bool readLine(FsFile& f, std::string& out) {
  out.clear();
  char c = 0;
  bool any = false;
  while (f.read(&c, 1) == 1) {
    any = true;
    if (c == '\n') return true;
    if (out.size() < MAX_LINE) out += c;
  }
  return any && !out.empty();
}
}  // namespace

void record(const std::string& path, const uint32_t startedAt, const uint32_t activeMs) {
  if (!KOREADER_STORE.getSyncStats() || !KOREADER_STORE.supportsSyncExtensions()) return;
  const uint32_t endedAt = TimeUtils::getCurrentValidTimestamp();
  const uint32_t seconds = activeMs / 1000;
  // A session the server would refuse is not worth the card write.
  if (startedAt == 0 || !TimeUtils::isClockValid(endedAt) || endedAt < startedAt || seconds < 10 ||
      endedAt - startedAt > MAX_SPAN_SECONDS || path.empty()) {
    return;
  }
  FsFile f = Storage.open(OUTBOX, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    LOG_ERR("SESSJ", "Cannot open the session outbox");
    return;
  }
  if (f.size() > OUTBOX_LIMIT) {
    // Bounded: a reader that never syncs keeps its oldest sessions, not an
    // ever-growing file.
    f.close();
    LOG_INF("SESSJ", "Session outbox full; not recording");
    return;
  }
  // Session id: start time + a hash of the path, unique per reader.
  MD5Builder md5;
  md5.begin();
  md5.add(path.c_str());
  md5.calculate();
  JsonDocument doc;
  char id[40];
  snprintf(id, sizeof(id), "s%lu-%.8s", static_cast<unsigned long>(startedAt), md5.toString().c_str());
  doc["id"] = id;
  doc["path"] = path;
  doc["s"] = startedAt;
  doc["e"] = endedAt;
  // Active time never exceeds the wall span (a clock step mid-session).
  doc["sec"] = std::min(seconds, endedAt - startedAt);
  doc["tz"] = utcOffsetMinutes(static_cast<time_t>(endedAt));
  std::string line;
  serializeJson(doc, line);
  line += '\n';
  f.write(line.data(), line.size());
  f.close();
}

Result upload(GfxRenderer& renderer, char* buffer, const size_t capacity, const char* deviceId, unsigned& sent) {
  sent = 0;
  if (!Storage.exists(OUTBOX)) return Result::Ok;
  std::unique_ptr<char, decltype(&free)> reply(static_cast<char*>(malloc(REPLY_CAPACITY)), &free);
  if (!reply) {
    LOG_ERR("SESSJ", "malloc failed: %u bytes", static_cast<unsigned>(REPLY_CAPACITY));
    return Result::Failed;
  }

  FsFile in;
  if (!Storage.openFileForRead("SESSJ", OUTBOX, in)) return Result::Failed;
  std::string line;
  line.reserve(MAX_LINE);
  size_t consumedBytes = 0;  // outbox bytes the server has confirmed (or that were invalid)
  Result result = Result::Ok;

  StatsPayload::SessionBatch batch(buffer, capacity);
  // Ids and documents of the batch in flight, to match the reply against.
  struct Pending {
    char id[40];
    char document[33];
  };
  Pending pending[StatsPayload::MAX_SESSION_BATCH];
  size_t pendingBytes = 0;

  const auto send = [&]() -> bool {
    if (batch.count() == 0) return true;
    if (!batch.finish()) return false;
    KOReaderSyncClient::Error err;
    {
      TlsFramebufferBorrow borrow(renderer);
      err =
          KOReaderSyncClient::extensionRequest("/api/v1/stats/sessions", batch.payload(), reply.get(), REPLY_CAPACITY);
    }
    if (err == KOReaderSyncClient::EXTENSION_UNSUPPORTED) {
      // An older server: keep the outbox for when it learns sessions.
      result = Result::Unsupported;
      return false;
    }
    if (err != KOReaderSyncClient::OK) {
      LOG_ERR("SESSJ", "Session upload failed: %s", KOReaderSyncClient::errorString(err));
      result = Result::Failed;
      return false;
    }
    // Only ids the server names are confirmed; a generic 200 is not enough.
    JsonDocument doc;
    if (deserializeJson(doc, reply.get()) != DeserializationError::Ok) {
      result = Result::Failed;
      return false;
    }
    const JsonArrayConst accepted = doc["accepted_session_ids"].as<JsonArrayConst>();
    for (size_t i = 0; i < batch.count(); i++) {
      bool found = false;
      for (JsonVariantConst value : accepted) {
        const char* acceptedId = value.as<const char*>();
        if (acceptedId && strcmp(acceptedId, pending[i].id) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        result = Result::Failed;
        return false;
      }
    }
    sent += static_cast<unsigned>(batch.count());
    consumedBytes += pendingBytes;
    pendingBytes = 0;
    return batch.begin(deviceId);
  };

  if (!batch.begin(deviceId)) return Result::Failed;
  bool ok = true;
  while (ok && readLine(in, line)) {
    const size_t lineBytes = line.size() + 1;
    JsonDocument rec;
    if (deserializeJson(rec, line) != DeserializationError::Ok) {
      // Drop a torn line: gone now if nothing is in flight, else with the
      // batch it sits inside once that is confirmed.
      if (batch.count() == 0) {
        consumedBytes += lineBytes;
      } else {
        pendingBytes += lineBytes;
      }
      continue;
    }
    const std::string document = documentIdFor(rec["path"] | "");
    const size_t slot = batch.count();
    if (slot >= StatsPayload::MAX_SESSION_BATCH) {
      ok = send();
      if (!ok) break;
    }
    Pending& p = pending[batch.count()];
    snprintf(p.id, sizeof(p.id), "%s", rec["id"] | "");
    snprintf(p.document, sizeof(p.document), "%s", document.c_str());
    StatsPayload::Session s;
    s.id = p.id;
    s.document = p.document;
    s.startedAt = rec["s"] | 0u;
    s.endedAt = rec["e"] | 0u;
    s.seconds = rec["sec"] | 0u;
    s.utcOffsetMinutes = rec["tz"] | 0;
    if (!batch.add(s)) {
      if (batch.count() == 0 || document.size() != 32) {
        // Invalid on its own (book gone under Binary matching, bad record): drop it.
        if (batch.count() == 0)
          consumedBytes += lineBytes;
        else
          pendingBytes += lineBytes;
        continue;
      }
      ok = send();  // buffer full: send, then this record starts the next batch
      if (!ok) break;
      Pending& q = pending[0];
      snprintf(q.id, sizeof(q.id), "%s", p.id);
      snprintf(q.document, sizeof(q.document), "%s", p.document);
      s.id = q.id;
      s.document = q.document;
      if (!batch.add(s)) {
        consumedBytes += lineBytes;
        continue;
      }
    }
    pendingBytes += lineBytes;
  }
  if (ok) send();
  const size_t total = in.size();
  in.close();

  if (consumedBytes >= total) {
    Storage.remove(OUTBOX);
  } else if (consumedBytes > 0) {
    // Keep only what the server has not confirmed.
    FsFile src;
    FsFile dst = Storage.open(OUTBOX_TMP, O_WRITE | O_CREAT | O_TRUNC);
    if (dst && Storage.openFileForRead("SESSJ", OUTBOX, src) && src.seekSet(consumedBytes)) {
      char chunk[128];
      int n;
      while ((n = src.read(chunk, sizeof(chunk))) > 0) dst.write(chunk, static_cast<size_t>(n));
      src.close();
      dst.close();
      Storage.remove(OUTBOX);
      Storage.rename(OUTBOX_TMP, OUTBOX);
    } else {
      LOG_ERR("SESSJ", "Could not trim the session outbox; sent sessions will be resent (the server dedupes by id)");
    }
  }
  LOG_INF("SESSJ", "Sent %u sessions (result %d)", sent, static_cast<int>(result));
  return result;
}

}  // namespace SessionJournal
