#pragma once

#include <DevicePolicy.h>

// Plugin-system contract advertised to catalogs ({firmware_family},
// {plugin_api}, {plugin_capabilities}) and the web UI (/api/status). The values
// follow upstream CrossPoint 2.0's src/util/FirmwareIdentity.h, so a catalog
// written for CrossPoint filters entries the same way here.
//
// List a capability ONLY once the behaviour behind it is implemented in this
// tree: catalogs hide entries whose `requires` names a capability we do not
// advertise, so an unimplemented one here would offer a plugin that cannot
// work. Never list crypto / book.key / book.protected (content protection is
// not part of this firmware).
namespace firmwareidentity {
inline constexpr const char* FAMILY = "crosspoint";

#if CROSSPOINT_SD_PLUGINS
inline constexpr int PLUGIN_API_VERSION = 1;
// clang-format off
inline constexpr const char* PLUGIN_CAPABILITIES =
    "catalog.lists,"
    "catalog.notices,"
    "catalog.bundle_install,"
    "catalog.version_tracking,"
    "catalog.compatibility,"
    "catalog.item_details,"
    "catalog.encoded_item_fields,"
    "catalog.list_index_mapping,"
    "catalog.list_requirements,"
    "catalog.server_paths,"
    "events.reader.open,"
    "events.reader.exit,"
    "events.reader.session,"
    "events.book.downloaded,"
    "events.sleep.enter";
// clang-format on
#else
// Boards without plugin support (lib/DevicePolicy) run no plugin contract.
inline constexpr int PLUGIN_API_VERSION = 0;
inline constexpr const char* PLUGIN_CAPABILITIES = "";
#endif
}  // namespace firmwareidentity
