#pragma once

#include <string>

// Protect stream keys at rest. On Windows uses DPAPI; on macOS Keychain-backed AES-256-CBC;
// on Linux OpenSSL when available. Falls back to storing plaintext if protection fails.
bool stream_key_protect_for_save(const std::string &plain, std::string *out_blob, std::string *out_encoding);

// Reverse stream_key_protect_for_save. encoding empty means legacy plaintext in blob.
bool stream_key_unprotect_load(const std::string &blob, const std::string &encoding, std::string *out_plain);

// Linux/OpenSSL build only: must point at a persistent path before load/save (module config dir).
void stream_key_set_master_key_file_path(const char *path);
