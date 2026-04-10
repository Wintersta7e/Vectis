#pragma once

#include <filesystem>
#include <string>

#include "core/result.h"

namespace vectis::services { class StorageEngine; }

namespace vectis::modes::code {

class CodeIndex;

/// Metadata written alongside the index cache in the kv_store.
struct CacheMetadata {
    std::filesystem::path project_root;
    std::string           scan_timestamp;
};

/// Persist the entire CodeIndex to the database.
///
/// Runs inside a single transaction: deletes all existing rows in
/// files/symbols/dependencies then re-inserts from the index snapshot.
/// Writes project root and timestamp to kv_store for cache validation.
[[nodiscard]] vectis::core::Result<void>
save_index(vectis::services::StorageEngine& storage,
           const CodeIndex&                 index,
           const CacheMetadata&             metadata);

/// Load a previously saved index from the database.
///
/// Returns the metadata that was written by `save_index`, or an error
/// if no cache exists (kv_store key missing or files table empty).
/// The caller must provide a cleared CodeIndex.
[[nodiscard]] vectis::core::Result<CacheMetadata>
load_index(vectis::services::StorageEngine& storage,
           CodeIndex&                       index);

/// Check whether a cache exists for the given project root.
[[nodiscard]] bool
has_cache_for(vectis::services::StorageEngine& storage,
              const std::filesystem::path&     project_root);

/// Delete all index data from the database (files, symbols, deps,
/// fts_content, and cache.* kv_store entries).
[[nodiscard]] vectis::core::Result<void>
clear_cache(vectis::services::StorageEngine& storage);

} // namespace vectis::modes::code
