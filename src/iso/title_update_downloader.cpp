// title_update_downloader.cpp — Title update downloader implementation
//
// Downloads Xbox 360 title update packages from xboxunity.net and extracts
// .xexp payload files from STFS containers. Uses WinHTTP on Windows.
//
// URL format: https://xboxunity.net/Resources/Lib/TitleUpdate.php?tuid=<TITLE_ID_DECIMAL>
// The response is a CON/LIVE/PIRS STFS package containing .xexp files.
//
// Inspired by Skate3Recomp's DownloadToFile + StagePayloads system.

#include "iso/title_update_downloader.h"
#include "iso/stfs_reader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace recomp::tu {

namespace {

#ifdef _WIN32

std::wstring Widen(const std::string& value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      wide.data(), length);
  return wide;
}

#ifndef WINHTTP_OPTION_IPV6_FAST_FALLBACK
#define WINHTTP_OPTION_IPV6_FAST_FALLBACK 140
#endif

#endif // _WIN32

// SHA-256 using a minimal implementation (inline, no external dependency)
// For verification of downloaded payloads
#include <cstdint>

// Minimal SHA-256
struct Sha256Context {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t data[64];
  uint32_t datalen;
};

void Sha256Init(Sha256Context& ctx) {
  ctx.datalen = 0;
  ctx.bitlen = 0;
  ctx.state[0] = 0x6a09e667; ctx.state[1] = 0xbb67ae85;
  ctx.state[2] = 0x3c6ef372; ctx.state[3] = 0xa54ff53a;
  ctx.state[4] = 0x510e527f; ctx.state[5] = 0x9b05688c;
  ctx.state[6] = 0x1f83d9ab; ctx.state[7] = 0x5be0cd19;
}

static const uint32_t k[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32-(n))))
#define EP0(x) (ROTR(x,2) ^ ROTR(x,13) ^ ROTR(x,22))
#define EP1(x) (ROTR(x,6) ^ ROTR(x,11) ^ ROTR(x,25))
#define SIG0(x) (ROTR(x,7) ^ ROTR(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))

void Sha256Transform(Sha256Context& ctx, const uint8_t data[]) {
  uint32_t a,b,c,d,e,f,g,h,t1,t2,m[64];
  for (int i = 0, j = 0; i < 16; ++i, j += 4)
    m[i] = ((uint32_t)data[j]<<24)|((uint32_t)data[j+1]<<16)|((uint32_t)data[j+2]<<8)|(uint32_t)data[j+3];
  for (int i = 16; i < 64; ++i)
    m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];
  a=ctx.state[0]; b=ctx.state[1]; c=ctx.state[2]; d=ctx.state[3];
  e=ctx.state[4]; f=ctx.state[5]; g=ctx.state[6]; h=ctx.state[7];
  for (int i = 0; i < 64; ++i) {
    t1 = h + EP1(e) + ((e&f)^((~e)&g)) + k[i] + m[i];
    t2 = EP0(a) + ((a&b)^(a&c)^(b&c));
    h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
  }
  ctx.state[0]+=a; ctx.state[1]+=b; ctx.state[2]+=c; ctx.state[3]+=d;
  ctx.state[4]+=e; ctx.state[5]+=f; ctx.state[6]+=g; ctx.state[7]+=h;
}

void Sha256Update(Sha256Context& ctx, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    ctx.data[ctx.datalen++] = data[i];
    if (ctx.datalen == 64) {
      Sha256Transform(ctx, ctx.data);
      ctx.bitlen += 512;
      ctx.datalen = 0;
    }
  }
}

void Sha256Final(Sha256Context& ctx, uint8_t hash[32]) {
  uint32_t i = ctx.datalen;
  if (ctx.datalen < 56) {
    ctx.data[i++] = 0x80;
    while (i < 56) ctx.data[i++] = 0x00;
  } else {
    ctx.data[i++] = 0x80;
    while (i < 64) ctx.data[i++] = 0x00;
    Sha256Transform(ctx, ctx.data);
    memset(ctx.data, 0, 56);
  }
  ctx.bitlen += (uint64_t)ctx.datalen * 8;
  ctx.data[63] = (uint8_t)(ctx.bitlen);
  ctx.data[62] = (uint8_t)(ctx.bitlen >> 8);
  ctx.data[61] = (uint8_t)(ctx.bitlen >> 16);
  ctx.data[60] = (uint8_t)(ctx.bitlen >> 24);
  ctx.data[59] = (uint8_t)(ctx.bitlen >> 32);
  ctx.data[58] = (uint8_t)(ctx.bitlen >> 40);
  ctx.data[57] = (uint8_t)(ctx.bitlen >> 48);
  ctx.data[56] = (uint8_t)(ctx.bitlen >> 56);
  Sha256Transform(ctx, ctx.data);
  for (i = 0; i < 4; ++i) {
    for (int j = 0; j < 8; ++j) {
      hash[i*8+j] = (uint8_t)((ctx.state[j] >> (24 - i * 8)) & 0xff);
    }
  }
}

std::string Sha256Hex(const uint8_t* data, size_t size) {
  Sha256Context ctx;
  Sha256Init(ctx);
  Sha256Update(ctx, data, size);
  uint8_t hash[32];
  Sha256Final(ctx, hash);
  static const char hex[] = "0123456789abcdef";
  std::string result(64, '\0');
  for (size_t i = 0; i < 32; ++i) {
    result[i*2] = hex[hash[i] >> 4];
    result[i*2+1] = hex[hash[i] & 0xf];
  }
  return result;
}

}  // namespace

bool DownloadPackage(const std::string& url,
                     const std::filesystem::path& destination,
                     ProgressCallback progress,
                     std::string& error) {
#ifdef _WIN32
  std::wstring wide_url = Widen(url);
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
    error = "Invalid title update URL.";
    return false;
  }
  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
  if (components.lpszExtraInfo && components.dwExtraInfoLength) {
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;

  HINTERNET session = WinHttpOpen(L"glue360-recompiler/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) { error = "WinHttpOpen failed."; return false; }

  // IPv6 fast fallback (Happy Eyeballs) to avoid stalling on dead IPv6
  DWORD fast_fallback = 1;
  WinHttpSetOption(session, WINHTTP_OPTION_IPV6_FAST_FALLBACK, &fast_fallback, sizeof(fast_fallback));

  HINTERNET connect = WinHttpConnect(session, host.c_str(),
                                     secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
  if (!connect) { error = "WinHttpConnect failed."; WinHttpCloseHandle(session); return false; }

  HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0);
  if (!request) { error = "WinHttpOpenRequest failed."; WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }

  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    error = "WinHttpSendRequest failed.";
    WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
    return false;
  }

  if (!WinHttpReceiveResponse(request, nullptr)) {
    error = "WinHttpReceiveResponse failed.";
    WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
    return false;
  }

  // Get content length for progress
  uint64_t total_bytes = 0;
  DWORD size = sizeof(total_bytes);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                      WINHTTP_HEADER_NAME_BY_INDEX, &total_bytes, &size, WINHTTP_NO_HEADER_INDEX);

  // Download to file
  std::ofstream out(destination, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "Unable to create download file: " + destination.string();
    WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
    return false;
  }

  uint64_t downloaded = 0;
  std::vector<uint8_t> buffer(64 * 1024);
  DWORD bytes_read = 0;
  while (WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read) && bytes_read > 0) {
    out.write(reinterpret_cast<const char*>(buffer.data()), bytes_read);
    if (!out) {
      error = "Failed to write download data.";
      WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
      return false;
    }
    downloaded += bytes_read;
    if (progress) progress(downloaded, total_bytes);
  }

  out.close();
  WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);

  if (downloaded == 0) {
    error = "Downloaded 0 bytes — the server may be unavailable.";
    return false;
  }

  return true;
#else
  error = "Title update download is only supported on Windows (WinHTTP).";
  return false;
#endif
}

bool StagePayloads(const std::filesystem::path& package_path,
                   const std::filesystem::path& game_root,
                   const std::vector<TitleUpdatePayload>& payloads,
                   std::string& error) {
  // Read the package
  std::ifstream file(package_path, std::ios::binary);
  if (!file) {
    error = "Unable to open package: " + package_path.string();
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

  // Check if it's an STFS package
  if (!stfs::StfsReader::LooksLikeStfs(data)) {
    // Maybe it's a raw .xexp file — check if it matches any payload
    for (const auto& payload : payloads) {
      if (!payload.sha256.empty() && data.size() == payload.size) {
        std::string hash = Sha256Hex(data.data(), data.size());
        if (hash == payload.sha256) {
          // Stage it directly
          auto target = game_root / std::filesystem::path(payload.staged_path);
          std::error_code ec;
          std::filesystem::create_directories(target.parent_path(), ec);
          std::ofstream out(target, std::ios::binary | std::ios::trunc);
          if (!out) { error = "Unable to write: " + target.string(); return false; }
          out.write(reinterpret_cast<const char*>(data.data()), data.size());
          return true;
        }
      }
    }
    error = "The file is not an STFS package and doesn't match any expected payload.";
    return false;
  }

  // Parse STFS package
  stfs::StfsReader reader;
  if (!reader.Open(std::move(data), error)) return false;

  std::vector<stfs::StfsEntry> entries;
  if (!reader.ListEntries(entries, error)) return false;

  // Extract and stage each payload
  for (const auto& payload : payloads) {
    bool found = false;
    for (const auto& entry : entries) {
      if (entry.is_dir) continue;
      // Match by container path or by filename (last path component)
      bool match = (entry.path == payload.container_path);
      if (!match) {
        auto pos = entry.path.find_last_of('/');
        if (pos != std::string::npos) {
          match = (entry.path.substr(pos + 1) == payload.container_path);
        }
      }
      if (match) {
        std::vector<uint8_t> file_data;
        if (!reader.ReadFile(entry, file_data, error)) return false;

        // Verify SHA-256 if specified
        if (!payload.sha256.empty()) {
          std::string hash = Sha256Hex(file_data.data(), file_data.size());
          if (hash != payload.sha256) {
            error = "SHA-256 mismatch for " + payload.container_path +
                    ": expected " + payload.sha256 + ", got " + hash;
            return false;
          }
        }

        // Stage the file
        auto target = game_root / std::filesystem::path(payload.staged_path);
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) { error = "Unable to write: " + target.string(); return false; }
        out.write(reinterpret_cast<const char*>(file_data.data()),
                  static_cast<std::streamsize>(file_data.size()));
        if (!out) { error = "Failed to write: " + target.string(); return false; }
        found = true;
        break;
      }
    }
    if (!found) {
      error = "Payload not found in package: " + payload.container_path;
      return false;
    }
  }

  return true;
}

bool IsTitleUpdateInstalled(const std::filesystem::path& game_root,
                            const std::vector<TitleUpdatePayload>& payloads) {
  for (const auto& payload : payloads) {
    if (!std::filesystem::exists(game_root / std::filesystem::path(payload.staged_path))) {
      return false;
    }
  }
  return true;
}

bool InstallTitleUpdate(const std::string& url,
                        const std::filesystem::path& game_root,
                        const std::vector<TitleUpdatePayload>& payloads,
                        ProgressCallback download_progress,
                        std::string& error) {
  // Check if already installed
  if (IsTitleUpdateInstalled(game_root, payloads)) {
    return true;
  }

  // Download to temp file
  auto temp_path = game_root / ".tu_download.tmp";
  if (!DownloadPackage(url, temp_path, download_progress, error)) {
    std::filesystem::remove(temp_path);
    return false;
  }

  // Stage payloads
  if (!StagePayloads(temp_path, game_root, payloads, error)) {
    std::filesystem::remove(temp_path);
    return false;
  }

  // Clean up
  std::filesystem::remove(temp_path);
  return true;
}

}  // namespace recomp::tu
