// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string_view>

#include <curl/curl.h>

#include "Common/FileUtil.h"

namespace Common
{
// The bundled libcurl uses mbedTLS, which can't read the system certificate store, so point it at
// the system CA bundle. Orca bundles libcurl because macOS's system copy lacks WebSocket support.
// Schannel on Windows and other TLS backends find the system store themselves.
inline void ConfigureCurlTLS(CURL* curl)
{
  const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
  if (!info || !info->ssl_version)
    return;
  const std::string_view backend(info->ssl_version);
  // By default Schannel fails when the revocation server can't be reached, which happens behind
  // captive portals and some proxies. Best effort still refuses certificates known to be revoked.
  if (backend.find("Schannel") != std::string_view::npos)
  {
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    return;
  }
  if (backend.find("mbedTLS") == std::string_view::npos)
    return;
  static constexpr std::array<const char*, 4> bundles{
      "/etc/ssl/cert.pem",                   // macOS, some BSDs
      "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Arch
      "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL
      "/etc/ssl/ca-bundle.pem",              // openSUSE
  };
  for (const char* bundle : bundles)
  {
    if (File::Exists(bundle))
    {
      curl_easy_setopt(curl, CURLOPT_CAINFO, bundle);
      return;
    }
  }
}
}  // namespace Common
