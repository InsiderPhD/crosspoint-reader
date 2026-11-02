#include "SyncBookLauncher.h"

#include <FsHelpers.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include "BookFusionBookIdStore.h"
#include "BookFusionTokenStore.h"
#include "CrossPointState.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

namespace SyncBookLauncher {

bool canSync(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path)) return false;
  if (KOREADER_STORE.hasCredentials()) return true;
  return BF_TOKEN_STORE.hasToken() && BookFusionBookIdStore::loadBookId(path.c_str()) != 0;
}

void start(const std::string& path) {
  LOG_INF("SYNCBOOK", "Opening %s to sync it", path.c_str());
  // Memory only: a reboot before the reader consumes it must not sync later.
  APP_STATE.pendingSyncPath = path;
  activityManager.goToReader(path);
}

}  // namespace SyncBookLauncher
