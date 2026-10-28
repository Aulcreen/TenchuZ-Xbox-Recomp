// tenchuz setup - the installer and uninstaller.
//
// TenchuZ-Setup.exe is this program with the game's files appended
// (tools/package_release.ps1 builds it with --pack). Installing needs the
// player's own Tenchu Z disc image: Setup checks that the image's default.xex
// is the version this build was recompiled from before it installs anything,
// then writes the game files and the disc's file system (game\) into the
// install folder, makes shortcuts and registers an uninstaller (Windows'
// installed apps list, per user). The installed game runs from that folder
// alone; the disc image isn't used again. Nothing from the disc ships with
// the installer, and the window's art is drawn here, not taken from the game.
//
//   TenchuZ-Setup.exe [--iso <image.iso>] [--dir <folder>] [--yes]
//                     [--no-shortcut] [--no-launch]
//   Setup.exe --uninstall [--dir <folder>] [--yes] [--remove-saves]
//   Setup.exe --pack <folder> <out.exe>   (packaging)
//   Setup.exe --render-pages <folder>     (every page as a PNG, for design reviews)
//
// --yes runs without the window and answers every question with its default
// (unattended installs, tests). Without a payload (the plain Setup.exe left
// in an install folder), Setup repairs: it (re)extracts the disc image into
// game\ next to it. Setup.exe in the install folder is also the uninstaller;
// it runs from a copy in %TEMP% so it can remove itself.
//
// The disc reader follows tools/xiso.py: Redump images (XGD1/2/3 video
// partition offsets) and trimmed game-partition images.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace fs = std::filesystem;
namespace gp = Gdiplus;

namespace {

constexpr uint64_t kSector = 0x800;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr uint64_t kPartitionOffsets[] = {0x0, 0xFD90000, 0x2080000, 0x18300000};
// default.xex of the release this build was recompiled from (Tenchu Z,
// Title ID 465307D7, 11,649,024 bytes).
constexpr char kXexSha256[] = "C727DEBEAAD474DFCF8517202EEBC388F9E51705CE3B01499486FA5FF47F9F06";
constexpr wchar_t kTitle[] = L"Tenchu Z setup";
constexpr char kPayloadMagic[8] = {'T', 'Z', 'S', 'E', 'T', 'U', 'P', '1'};
// Windows' installed apps list, per user (no administrator rights needed).
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TenchuZ-Recomp";
// The installed game files, one per line, for the uninstaller.
constexpr wchar_t kManifest[] = L"setup_files.txt";

bool g_yes = false;  // --yes: no window, defaults for every question

// --- Messages (unattended runs) ---------------------------------------------

int Fail(const std::wstring& message) {
  std::fwprintf(stderr, L"\n%ls\n", message.c_str());
  return 1;
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
  return out;
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring Gigabytes(uint64_t bytes) {
  wchar_t text[32];
  std::swprintf(text, 32, L"%.1f GB", double(bytes) / double(1ull << 30));
  return text;
}

// --- Files ------------------------------------------------------------------

class File {
 public:
  File(const fs::path& path, bool write) {
    handle_ = write ? CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr)
                    : CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  }
  ~File() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }
  bool ok() const { return handle_ != INVALID_HANDLE_VALUE; }
  uint64_t size() const {
    LARGE_INTEGER s = {};
    GetFileSizeEx(handle_, &s);
    return uint64_t(s.QuadPart);
  }
  bool ReadAt(uint64_t offset, void* out, size_t size) {
    LARGE_INTEGER at;
    at.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(handle_, at, nullptr, FILE_BEGIN)) return false;
    return Read(out, size);
  }
  bool Read(void* out, size_t size) {
    auto* p = static_cast<uint8_t*>(out);
    while (size) {
      DWORD got = 0;
      const DWORD chunk = DWORD(std::min<size_t>(size, 1u << 30));
      if (!ReadFile(handle_, p, chunk, &got, nullptr) || got != chunk) return false;
      p += got;
      size -= got;
    }
    return true;
  }
  bool Write(const void* data, size_t size) {
    auto* p = static_cast<const uint8_t*>(data);
    while (size) {
      DWORD written = 0;
      const DWORD chunk = DWORD(std::min<size_t>(size, 1u << 30));
      if (!WriteFile(handle_, p, chunk, &written, nullptr) || written != chunk) return false;
      p += written;
      size -= written;
    }
    return true;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::string Sha256(const void* data, size_t size) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  uint8_t digest[32];
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
      BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
      BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), 0) == 0 &&
      BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
    char text[3];
    for (uint8_t b : digest) {
      std::snprintf(text, sizeof(text), "%02X", b);
      hex += text;
    }
  }
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  return hex;
}

std::string Sha256File(const fs::path& path) {
  File file(path, false);
  if (!file.ok()) return {};
  std::vector<uint8_t> data(size_t(file.size()));
  if (!data.empty() && !file.Read(data.data(), data.size())) return {};
  return Sha256(data.data(), data.size());
}

// --- Disc image ---------------------------------------------------------------

struct Entry {
  std::string path;  // with '/' separators
  uint32_t sector;
  uint32_t size;
};

class Iso {
 public:
  explicit Iso(const fs::path& path) : file_(path, false) {}
  bool ok() const { return file_.ok(); }

  bool FindPartition() {
    char magic[sizeof(kMagic) - 1];
    for (uint64_t offset : kPartitionOffsets) {
      if (file_.ReadAt(offset + 0x10000, magic, sizeof(magic)) && std::memcmp(magic, kMagic, sizeof(magic)) == 0) {
        base_ = offset;
        return true;
      }
    }
    return false;
  }

  bool List(std::vector<Entry>& out) {
    uint32_t root[2];
    if (!file_.ReadAt(base_ + 0x10000 + 20, root, sizeof(root))) return false;
    return Walk(root[0], root[1], "", out, 0);
  }

  bool ReadEntry(const Entry& e, uint64_t offset, void* out, size_t size) {
    return file_.ReadAt(base_ + uint64_t(e.sector) * kSector + offset, out, size);
  }

 private:
  bool Walk(uint32_t sector, uint32_t size, const std::string& prefix, std::vector<Entry>& out, int depth) {
    if (depth > 32 || size == 0 || size > (64u << 20)) return depth <= 32;
    std::vector<uint8_t> data(size);
    if (!file_.ReadAt(base_ + uint64_t(sector) * kSector, data.data(), size)) return false;
    std::set<uint32_t> seen;
    std::vector<uint32_t> stack{0};
    while (!stack.empty()) {
      const uint32_t off = stack.back();
      stack.pop_back();
      if (!seen.insert(off).second || off + 14 > size) continue;
      uint16_t left, right;
      uint32_t start, length;
      std::memcpy(&left, &data[off], 2);
      std::memcpy(&right, &data[off + 2], 2);
      std::memcpy(&start, &data[off + 4], 4);
      std::memcpy(&length, &data[off + 8], 4);
      const uint8_t attrs = data[off + 12], name_length = data[off + 13];
      if (left == 0xFFFF) continue;  // padding
      if (off + 14 + name_length > size) continue;
      const std::string name(reinterpret_cast<const char*>(&data[off + 14]), name_length);
      if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) continue;
      if (attrs & 0x10) {
        if (length && !Walk(start, length, prefix + name + "/", out, depth + 1)) return false;
      } else {
        out.push_back({prefix + name, start, length});
      }
      if (left) stack.push_back(uint32_t(left) * 4);
      if (right) stack.push_back(uint32_t(right) * 4);
    }
    return true;
  }

  File file_;
  uint64_t base_ = 0;
};

fs::path JoinPath(const fs::path& root, const std::string& slashed) {
  fs::path out = root;
  size_t from = 0;
  for (size_t slash; (slash = slashed.find('/', from)) != std::string::npos; from = slash + 1) {
    out /= Widen(slashed.substr(from, slash - from));
  }
  return out / Widen(slashed.substr(from));
}

// Opens a disc image and checks it's the Tenchu Z release this build is for.
// On failure `error` says why, for the player.
struct Disc {
  fs::path path;
  std::vector<Entry> files;
  uint64_t bytes = 0;
};

bool CheckDisc(const fs::path& path, Disc& disc, std::wstring& error) {
  disc = {};
  disc.path = path;
  Iso iso(path);
  if (!iso.ok()) {
    error = L"Can't open this file.";
    return false;
  }
  if (!iso.FindPartition() || !iso.List(disc.files) || disc.files.empty()) {
    error = L"Not an Xbox 360 disc image.";
    return false;
  }
  auto xex = std::find_if(disc.files.begin(), disc.files.end(),
                          [](const Entry& e) { return _stricmp(e.path.c_str(), "default.xex") == 0; });
  if (xex == disc.files.end()) {
    error = L"Not a game disc (no default.xex).";
    return false;
  }
  std::vector<uint8_t> xex_data(xex->size);
  if (!iso.ReadEntry(*xex, 0, xex_data.data(), xex_data.size()) ||
      Sha256(xex_data.data(), xex_data.size()) != kXexSha256) {
    error = L"Not the Tenchu Z (Xbox 360) release this build is for.";
    return false;
  }
  for (const Entry& e : disc.files) disc.bytes += e.size;
  return true;
}

// --- Payload (the game files appended to TenchuZ-Setup.exe) --------------------
//
// [Setup.exe][file data (LZMS)...][index][footer]
// index: per file {u32 name bytes, name (UTF-8, '/'), u64 offset, u64 packed, u64 size}
// footer: {magic "TZSETUP1", u64 index offset, u64 index size}

struct PayloadFile {
  std::string name;
  uint64_t offset = 0, packed = 0, size = 0;
};

#pragma pack(push, 1)
struct Footer {
  char magic[8];
  uint64_t index_offset;
  uint64_t index_size;
};
#pragma pack(pop)

bool ReadPayload(const fs::path& exe, std::vector<PayloadFile>& files) {
  File file(exe, false);
  if (!file.ok() || file.size() < sizeof(Footer)) return false;
  Footer footer;
  if (!file.ReadAt(file.size() - sizeof(Footer), &footer, sizeof(footer)) ||
      std::memcmp(footer.magic, kPayloadMagic, sizeof(kPayloadMagic)) != 0) {
    return false;
  }
  std::vector<uint8_t> index(size_t(footer.index_size));
  if (!file.ReadAt(footer.index_offset, index.data(), index.size())) return false;
  size_t at = 0;
  auto take = [&](void* out, size_t n) {
    if (at + n > index.size()) return false;
    std::memcpy(out, index.data() + at, n);
    at += n;
    return true;
  };
  while (at < index.size()) {
    uint32_t name_size = 0;
    PayloadFile f;
    if (!take(&name_size, 4) || name_size > 1024) return false;
    f.name.resize(name_size);
    if (!take(f.name.data(), name_size) || !take(&f.offset, 8) || !take(&f.packed, 8) || !take(&f.size, 8)) {
      return false;
    }
    files.push_back(std::move(f));
  }
  return true;
}

int Pack(const fs::path& folder, const fs::path& out_path) {
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::vector<PayloadFile> existing;
  if (ReadPayload(self, existing)) return Fail(L"--pack needs the plain Setup.exe, not an installer.");
  File stub(self, false);
  std::vector<uint8_t> stub_bytes(size_t(stub.size()));
  if (!stub.ok() || !stub.Read(stub_bytes.data(), stub_bytes.size())) return Fail(L"Can't read Setup.exe.");
  File out(out_path, true);
  if (!out.ok() || !out.Write(stub_bytes.data(), stub_bytes.size())) return Fail(L"Can't write " + out_path.wstring());
  uint64_t offset = stub_bytes.size();

  COMPRESSOR_HANDLE compressor = nullptr;
  if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) return Fail(L"No LZMS compressor.");
  std::vector<fs::path> paths;
  for (const auto& entry : fs::recursive_directory_iterator(folder)) {
    if (entry.is_regular_file()) paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());
  std::vector<PayloadFile> files;
  for (const fs::path& path : paths) {
    File in(path, false);
    std::vector<uint8_t> data(size_t(in.size()));
    if (!in.ok() || (!data.empty() && !in.Read(data.data(), data.size()))) return Fail(L"Can't read " + path.wstring());
    PayloadFile f;
    f.name = Narrow(fs::relative(path, folder).generic_wstring());
    f.size = data.size();
    f.offset = offset;
    if (!data.empty()) {
      SIZE_T needed = 0;
      Compress(compressor, data.data(), data.size(), nullptr, 0, &needed);
      std::vector<uint8_t> packed(needed);
      SIZE_T packed_size = 0;
      if (!Compress(compressor, data.data(), data.size(), packed.data(), packed.size(), &packed_size)) {
        CloseCompressor(compressor);
        return Fail(L"Compressing " + path.wstring() + L" failed.");
      }
      if (!out.Write(packed.data(), packed_size)) return Fail(L"Can't write " + out_path.wstring());
      f.packed = packed_size;
      offset += packed_size;
    }
    std::wprintf(L"  %ls: %llu -> %llu bytes\n", Widen(f.name).c_str(), f.size, f.packed);
    files.push_back(std::move(f));
  }
  CloseCompressor(compressor);

  std::vector<uint8_t> index;
  auto put = [&](const void* p, size_t n) {
    index.insert(index.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
  };
  for (const PayloadFile& f : files) {
    const uint32_t name_size = uint32_t(f.name.size());
    put(&name_size, 4);
    put(f.name.data(), name_size);
    put(&f.offset, 8);
    put(&f.packed, 8);
    put(&f.size, 8);
  }
  Footer footer;
  std::memcpy(footer.magic, kPayloadMagic, sizeof(kPayloadMagic));
  footer.index_offset = offset;
  footer.index_size = index.size();
  if (!out.Write(index.data(), index.size()) || !out.Write(&footer, sizeof(footer))) {
    return Fail(L"Can't write " + out_path.wstring());
  }
  std::wprintf(L"Packed %zu files into %ls\n", files.size(), out_path.c_str());
  return 0;
}

// --- Shell ----------------------------------------------------------------------

fs::path PickIso(HWND owner) {
  fs::path result;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    return result;
  }
  const COMDLG_FILTERSPEC filters[] = {{L"Disc images (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
  dialog->SetFileTypes(2, filters);
  dialog->SetTitle(L"Your Tenchu Z (Xbox 360) disc image");
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return result;
}

fs::path PickFolder(HWND owner) {
  fs::path result;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    return result;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(L"Install Tenchu Z in (a \"Tenchu Z\" folder is made inside)");
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = fs::path(path) / L"Tenchu Z";
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return result;
}

uint64_t FreeBytes(const fs::path& path) {
  ULARGE_INTEGER free_bytes = {};
  fs::path probe = path;
  while (!probe.empty() && !fs::exists(probe)) {
    if (probe == probe.parent_path()) break;
    probe = probe.parent_path();
  }
  GetDiskFreeSpaceExW(probe.empty() ? nullptr : probe.c_str(), &free_bytes, nullptr, nullptr);
  return free_bytes.QuadPart;
}

// "<fixed drive with the most free space>\Games\Tenchu Z".
fs::path SuggestedFolder() {
  wchar_t best_root[4] = L"C:\\";
  uint64_t best_free = 0;
  const DWORD drives = GetLogicalDrives();
  for (int i = 2; i < 26; ++i) {
    if (!(drives & (1u << i))) continue;
    wchar_t root[4] = {wchar_t(L'A' + i), L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
    const uint64_t free_bytes = FreeBytes(root);
    if (free_bytes > best_free) {
      best_free = free_bytes;
      wcscpy_s(best_root, root);
    }
  }
  return fs::path(best_root) / L"Games" / L"Tenchu Z";
}

bool Writable(const fs::path& dir) {
  std::error_code error;
  fs::create_directories(dir, error);
  const fs::path probe = dir / L".setup_write_test";
  {
    File test(probe, true);
    if (!test.ok() || !test.Write("ok", 2)) return false;
  }
  fs::remove(probe, error);
  return true;
}

bool CreateShortcut(const fs::path& target, const fs::path& link, const wchar_t* description,
                    const wchar_t* arguments = nullptr) {
  IShellLinkW* shell_link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link)))) {
    return false;
  }
  shell_link->SetPath(target.c_str());
  if (arguments) shell_link->SetArguments(arguments);
  shell_link->SetWorkingDirectory(target.parent_path().c_str());
  shell_link->SetDescription(description);
  IPersistFile* file = nullptr;
  bool ok = SUCCEEDED(shell_link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(link.c_str(), TRUE));
  if (file) file->Release();
  shell_link->Release();
  return ok;
}

// The program a shortcut starts (empty if it can't be read).
fs::path ShortcutTarget(const fs::path& link) {
  fs::path result;
  IShellLinkW* shell_link = nullptr;
  IPersistFile* file = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link))) &&
      SUCCEEDED(shell_link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Load(link.c_str(), STGM_READ))) {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(shell_link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH))) result = path;
  }
  if (file) file->Release();
  if (shell_link) shell_link->Release();
  return result;
}

fs::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  fs::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) result = path;
  CoTaskMemFree(path);
  return result;
}

bool SamePath(const fs::path& a, const fs::path& b) {
  std::error_code error;
  if (fs::equivalent(a, b, error)) return true;
  return _wcsicmp(fs::absolute(a, error).lexically_normal().c_str(), fs::absolute(b, error).lexically_normal().c_str()) == 0;
}

// Whether tenchuz.exe from `dir` is running (its files can't be replaced or removed then).
bool GameRunning(const fs::path& dir) {
  bool running = false;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W entry = {sizeof(entry)};
  for (BOOL more = Process32FirstW(snapshot, &entry); more && !running; more = Process32NextW(snapshot, &entry)) {
    if (_wcsicmp(entry.szExeFile, L"tenchuz.exe") != 0) continue;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)) {
      wchar_t path[MAX_PATH];
      DWORD size = MAX_PATH;
      if (QueryFullProcessImageNameW(process, 0, path, &size)) running = SamePath(fs::path(path).parent_path(), dir);
      CloseHandle(process);
    }
  }
  CloseHandle(snapshot);
  return running;
}

std::wstring ReadText(const fs::path& path) {
  File file(path, false);
  if (!file.ok()) return {};
  std::string data(size_t(file.size()), '\0');
  if (!data.empty() && !file.Read(data.data(), data.size())) return {};
  while (!data.empty() && (data.back() == '\n' || data.back() == '\r' || data.back() == ' ')) data.pop_back();
  return Widen(data);
}

// --- Install and uninstall ----------------------------------------------------

// Progress shared with the window: the worker thread writes, the window reads.
struct Progress {
  std::atomic<uint64_t> done{0}, total{1};
  std::atomic<bool> cancel{false};
  std::mutex mutex;
  std::wstring step;  // what's happening now
  void SetStep(const std::wstring& text) {
    std::lock_guard lock(mutex);
    step = text;
    if (g_yes) std::wprintf(L"%ls\n", text.c_str());
  }
  std::wstring Step() {
    std::lock_guard lock(mutex);
    return step;
  }
};

struct InstallOptions {
  fs::path self;
  std::vector<PayloadFile> payload;  // empty: repair (disc data only)
  Disc disc;
  fs::path dir;
  bool desktop_shortcut = true;
  bool start_menu_shortcut = true;
};

void Register(const fs::path& dir, uint64_t bytes) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return;
  }
  auto text = [&](const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   DWORD((value.size() + 1) * sizeof(wchar_t)));
  };
  auto number = [&](const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
  };
  const std::wstring setup = L"\"" + (dir / L"Setup.exe").wstring() + L"\"";
  std::wstring version = ReadText(dir / L"version.txt");
  SYSTEMTIME now;
  GetLocalTime(&now);
  wchar_t date[16];
  std::swprintf(date, 16, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
  text(L"DisplayName", L"Tenchu Z (PC recompilation)");
  text(L"DisplayIcon", (dir / L"tenchuz.exe").wstring());
  if (!version.empty()) text(L"DisplayVersion", version);
  text(L"Publisher", L"Tenchu Z recompilation project");
  text(L"InstallLocation", dir.wstring());
  text(L"InstallDate", date);
  text(L"UninstallString", setup + L" --uninstall");
  text(L"QuietUninstallString", setup + L" --uninstall --yes");
  number(L"EstimatedSize", DWORD(std::min<uint64_t>(bytes >> 10, 0xFFFFFFFFu)));
  number(L"NoModify", 1);
  number(L"NoRepair", 1);
  RegCloseKey(key);
}

// Returns an empty string on success, else what went wrong.
std::wstring Install(const InstallOptions& options, Progress& progress) {
  const fs::path& dir = options.dir;
  if (GameRunning(dir)) return L"Tenchu Z is running from this folder. Close it, then try again.";
  if (!Writable(dir)) {
    return L"Can't write to " + dir.wstring() + L". Choose a folder you can write to (not inside Program Files).";
  }
  uint64_t payload_bytes = 0;
  for (const PayloadFile& f : options.payload) payload_bytes += f.size;
  const uint64_t needed = options.disc.bytes + payload_bytes + (100ull << 20);
  if (FreeBytes(dir) < needed) return L"Not enough free space: " + Gigabytes(needed) + L" needed.";
  progress.total = std::max<uint64_t>(1, payload_bytes + options.disc.bytes);
  progress.done = 0;

  // 1. The game files.
  if (!options.payload.empty()) {
    progress.SetStep(L"Installing the game");
    File self(options.self, false);
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!self.ok() || !CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor)) {
      return L"Can't read the installer.";
    }
    std::string manifest;
    for (const PayloadFile& f : options.payload) {
      if (progress.cancel) {
        CloseDecompressor(decompressor);
        return L"Stopped.";
      }
      const fs::path path = JoinPath(dir, f.name);
      std::error_code error;
      fs::create_directories(path.parent_path(), error);
      std::vector<uint8_t> packed(size_t(f.packed)), data(size_t(f.size));
      SIZE_T got = 0;
      if (f.packed && (!self.ReadAt(f.offset, packed.data(), packed.size()) ||
                       !Decompress(decompressor, packed.data(), packed.size(), data.data(), data.size(), &got) ||
                       got != f.size)) {
        CloseDecompressor(decompressor);
        return L"The installer is damaged (" + Widen(f.name) + L"). Download it again.";
      }
      File out(path, true);
      if (!out.ok() || (!data.empty() && !out.Write(data.data(), data.size()))) {
        CloseDecompressor(decompressor);
        return L"Can't write " + path.wstring() + L".";
      }
      manifest += f.name + "\n";
      progress.done += f.size;
    }
    CloseDecompressor(decompressor);
    File list(dir / kManifest, true);
    list.Write(manifest.data(), manifest.size());
  }

  // 2. The disc's file system into game\ (resumable: right-sized files are kept).
  progress.SetStep(L"Copying the game data from your disc image");
  Iso iso(options.disc.path);
  if (!iso.ok() || !iso.FindPartition()) return L"Can't read the disc image any more.";
  const fs::path game_dir = dir / L"game";
  std::vector<uint8_t> buffer(16u << 20);
  for (const Entry& e : options.disc.files) {
    const fs::path out = JoinPath(game_dir, e.path);
    std::error_code error;
    if (fs::is_regular_file(out, error) && fs::file_size(out, error) == e.size) {
      progress.done += e.size;
      continue;
    }
    fs::create_directories(out.parent_path(), error);
    File file(out, true);
    if (!file.ok()) return L"Can't write " + out.wstring() + L".";
    for (uint64_t offset = 0; offset < e.size;) {
      if (progress.cancel) return L"Stopped.";  // the partial file is replaced next time
      const uint32_t chunk = uint32_t(std::min<uint64_t>(e.size - offset, buffer.size()));
      if (!iso.ReadEntry(e, offset, buffer.data(), chunk) || !file.Write(buffer.data(), chunk)) {
        return L"Copying " + out.wstring() + L" failed: disk full, or a damaged disc image.";
      }
      offset += chunk;
      progress.done += chunk;
    }
  }
  progress.SetStep(L"Checking");
  if (Sha256File(game_dir / L"default.xex") != kXexSha256) {
    return L"The copied game data doesn't match the disc image. Run setup again.";
  }

  // 3. Shortcuts and the uninstaller.
  if (!options.payload.empty()) {
    const fs::path exe = dir / L"tenchuz.exe";
    if (options.start_menu_shortcut) {
      const fs::path programs = KnownFolder(FOLDERID_Programs);
      if (!programs.empty()) CreateShortcut(exe, programs / L"Tenchu Z.lnk", L"Tenchu Z");
    }
    if (options.desktop_shortcut) {
      const fs::path desktop = KnownFolder(FOLDERID_Desktop);
      if (!desktop.empty()) CreateShortcut(exe, desktop / L"Tenchu Z.lnk", L"Tenchu Z");
    }
    Register(dir, payload_bytes + options.disc.bytes);
  }
  progress.done = progress.total.load();
  progress.SetStep(L"Done");
  return {};
}

// The files an install without a manifest (before the uninstaller) put next to tenchuz.exe.
constexpr const wchar_t* kKnownFiles[] = {
    L"tenchuz.exe",         L"tenchuz.pdb",      L"Setup.exe",       L"README.txt",           L"version.txt",
    L"rexruntimerd.dll",    L"rexgpu-xenosrd.dll", L"TracyClientrd.dll", L"steam_api64.dll",    L"msvcp140.dll",
    L"msvcp140_atomic_wait.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"};

struct UninstallResult {
  std::wstring error;  // empty: done
  bool left_files = false;  // the folder stays: saves kept, or files Setup didn't make
};

UninstallResult Uninstall(const fs::path& dir, bool keep_saves, Progress& progress) {
  UninstallResult result;
  if (GameRunning(dir)) {
    result.error = L"Tenchu Z is running. Close it, then try again.";
    return result;
  }
  progress.SetStep(L"Removing Tenchu Z");
  std::vector<fs::path> files, folders = {dir / L"game", dir / L"logs"};
  if (!keep_saves) folders.push_back(dir / L"user");
  const std::string manifest = Narrow(ReadText(dir / kManifest));
  if (!manifest.empty()) {
    size_t from = 0;
    while (from < manifest.size()) {
      size_t end = manifest.find('\n', from);
      if (end == std::string::npos) end = manifest.size();
      std::string name = manifest.substr(from, end - from);
      if (!name.empty() && name.back() == '\r') name.pop_back();
      // Only paths inside the install folder.
      if (!name.empty() && name.find("..") == std::string::npos && name.find(':') == std::string::npos) {
        files.push_back(JoinPath(dir, name));
      }
      from = end + 1;
    }
  } else {
    for (const wchar_t* name : kKnownFiles) files.push_back(dir / name);
    folders.push_back(dir / L"steam_input");
  }
  files.push_back(dir / kManifest);
  if (!keep_saves) files.push_back(dir / L"tenchuz.toml");
  progress.total = files.size() + folders.size();
  progress.done = 0;

  std::error_code error;
  for (const fs::path& folder : folders) {
    if (fs::exists(folder, error) && !fs::remove_all(folder, error) && error) {
      result.error = L"Can't remove " + folder.wstring() + L". Close anything using it, then try again.";
      return result;
    }
    ++progress.done;
  }
  for (const fs::path& file : files) {
    fs::remove(file, error);  // the running uninstaller's own Setup.exe is a copy in %TEMP%
    ++progress.done;
    // Folders the manifest's files were in, once empty (steam_input\...).
    for (fs::path parent = file.parent_path(); parent != dir && parent.has_parent_path() &&
                                               fs::is_empty(parent, error) && !error;
         parent = parent.parent_path()) {
      fs::remove(parent, error);
    }
  }

  // Shortcuts that start this tenchuz.exe, and the installed apps entry.
  const fs::path exe = dir / L"tenchuz.exe";
  for (REFKNOWNFOLDERID id : {FOLDERID_Programs, FOLDERID_Desktop}) {
    const fs::path link = KnownFolder(id) / L"Tenchu Z.lnk";
    if (fs::exists(link, error) && SamePath(ShortcutTarget(link), exe)) fs::remove(link, error);
  }
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
    wchar_t location[MAX_PATH] = {};
    DWORD size = sizeof(location);
    const bool ours = RegQueryValueExW(key, L"InstallLocation", nullptr, nullptr, reinterpret_cast<BYTE*>(location),
                                       &size) == ERROR_SUCCESS &&
                      SamePath(location, dir);
    RegCloseKey(key);
    if (ours) RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
  }
  if (fs::is_empty(dir, error) && !error) {
    fs::remove(dir, error);
  } else {
    result.left_files = true;
  }
  progress.SetStep(L"Done");
  return result;
}

// --- The window -----------------------------------------------------------------
//
// Drawn with GDI+ after the game's own menus (no art taken from them; see
// Drawing below). Enter is A, Esc is B, Backspace is X. Everything is laid
// out in 96-DPI units and scaled to the monitor.

constexpr float kWidth = 880, kHeight = 540;
constexpr UINT kWorkerDone = WM_APP + 1;
constexpr UINT kDiscChecked = WM_APP + 2;
constexpr UINT_PTR kTimer = 1;

enum class Page { kWelcome, kDisc, kFolder, kWorking, kDone, kFailed, kUninstall, kUninstalled };
enum class Mode { kInstall, kRepair, kUninstall };

struct Button {
  wchar_t glyph = 0;  // 'A', 'B', 'X' or 0 (none shown)
  std::wstring label;
  gp::RectF rect;
  bool enabled = true;
};

struct Toggle {
  std::wstring label;
  bool* value = nullptr;
  gp::RectF rect;
};

class Window {
 public:
  Mode mode = Mode::kInstall;
  fs::path self;
  std::vector<PayloadFile> payload;
  fs::path dir;
  fs::path iso_arg;
  bool from_temp = false;

  int Run() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = &Window::Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"TenchuZSetup";
    RegisterClassExW(&wc);
    const UINT dpi = SystemDpi();
    scale_ = float(dpi) / 96.0f;
    const int w = int(kWidth * scale_), h = int(kHeight * scale_);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_ACCEPTFILES, wc.lpszClassName,
                            mode == Mode::kUninstall ? L"Uninstall Tenchu Z" : L"Tenchu Z setup",
                            WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU, work.left + (work.right - work.left - w) / 2,
                            work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, instance, this);
    if (!hwnd_) return 1;
    Start();
    ShowWindow(hwnd_, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (worker_.joinable()) {
      progress_.cancel = true;
      worker_.join();
    }
    if (checker_.joinable()) checker_.join();
    return result_;
  }

 private:
  static UINT SystemDpi() {
    HDC screen = GetDC(nullptr);
    const UINT dpi = UINT(GetDeviceCaps(screen, LOGPIXELSX));
    ReleaseDC(nullptr, screen);
    return dpi ? dpi : 96;
  }

  void Start() {
    if (mode == Mode::kUninstall) {
      page_ = Page::kUninstall;
    } else if (mode == Mode::kRepair) {
      page_ = Page::kDisc;
    } else {
      if (dir.empty()) dir = SuggestedFolder();
      page_ = Page::kWelcome;
    }
    if (!iso_arg.empty()) CheckDiscAsync(iso_arg);
  }

  // --- Actions ---

  void CheckDiscAsync(const fs::path& path) {
    if (checking_) return;
    if (checker_.joinable()) checker_.join();
    checking_ = true;
    disc_ok_ = false;
    disc_error_.clear();
    disc_.path = path;
    Invalidate();
    checker_ = std::thread([this, path] {
      Disc disc;
      std::wstring error;
      const bool ok = CheckDisc(path, disc, error);
      {
        std::lock_guard lock(progress_.mutex);
        checked_disc_ = std::move(disc);
        checked_error_ = error;
        checked_ok_ = ok;
      }
      PostMessageW(hwnd_, kDiscChecked, 0, 0);
    });
  }

  void StartInstall() {
    InstallOptions options;
    options.self = self;
    options.payload = mode == Mode::kRepair ? std::vector<PayloadFile>{} : payload;
    options.disc = disc_;
    options.dir = dir;
    options.desktop_shortcut = desktop_shortcut_;
    options.start_menu_shortcut = start_menu_shortcut_;
    progress_.cancel = false;
    progress_.done = 0;
    progress_.total = 1;
    page_ = Page::kWorking;
    SetTimer(hwnd_, kTimer, 100, nullptr);
    worker_ = std::thread([this, options] {
      error_ = Install(options, progress_);
      PostMessageW(hwnd_, kWorkerDone, 0, 0);
    });
    Invalidate();
  }

  void StartUninstall() {
    progress_.cancel = false;
    page_ = Page::kWorking;
    SetTimer(hwnd_, kTimer, 100, nullptr);
    worker_ = std::thread([this] {
      const UninstallResult result = Uninstall(dir, keep_saves_, progress_);
      error_ = result.error;
      left_files_ = result.left_files;
      PostMessageW(hwnd_, kWorkerDone, 0, 0);
    });
    Invalidate();
  }

  void Finish() {
    if (page_ == Page::kDone && launch_ && mode != Mode::kUninstall) {
      const fs::path exe = dir / L"tenchuz.exe";
      STARTUPINFOW startup = {sizeof(startup)};
      PROCESS_INFORMATION process = {};
      std::wstring command = L"\"" + exe.wstring() + L"\"";
      if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup,
                         &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
      }
    }
    DestroyWindow(hwnd_);
  }

  void Press(wchar_t glyph) {
    for (const Button& b : buttons_) {
      if (b.glyph == glyph && b.enabled) {
        Activate(b);
        return;
      }
    }
  }

  void Activate(const Button& b) {
    switch (page_) {
      case Page::kWelcome:
        if (b.glyph == 'A') page_ = Page::kDisc;
        if (b.glyph == 'B') Close();
        break;
      case Page::kDisc:
        if (b.glyph == 'X') {
          const fs::path path = PickIso(hwnd_);
          if (!path.empty()) CheckDiscAsync(path);
        }
        if (b.glyph == 'A') {
          if (mode == Mode::kRepair) {
            StartInstall();
          } else {
            page_ = Page::kFolder;
          }
        }
        if (b.glyph == 'B') {
          if (mode == Mode::kRepair) {
            Close();
          } else {
            page_ = Page::kWelcome;
          }
        }
        break;
      case Page::kFolder:
        if (b.glyph == 'X') {
          const fs::path path = PickFolder(hwnd_);
          if (!path.empty()) dir = path;
        }
        if (b.glyph == 'A') StartInstall();
        if (b.glyph == 'B') page_ = Page::kDisc;
        break;
      case Page::kWorking:
        if (b.glyph == 'B') progress_.cancel = true;
        break;
      case Page::kDone:
      case Page::kUninstalled:
        Finish();
        return;
      case Page::kFailed:
        if (b.glyph == 'A') {
          page_ = mode == Mode::kUninstall ? Page::kUninstall : (mode == Mode::kRepair ? Page::kDisc : Page::kFolder);
        }
        if (b.glyph == 'B') Close();
        break;
      case Page::kUninstall:
        if (b.glyph == 'A') StartUninstall();
        if (b.glyph == 'B') Close();
        break;
    }
    Invalidate();
  }

  void Close() {
    if (page_ == Page::kWorking) {
      progress_.cancel = true;  // the window closes once the worker stops
      closing_ = true;
      return;
    }
    DestroyWindow(hwnd_);
  }

  void Invalidate() { InvalidateRect(hwnd_, nullptr, FALSE); }

  // --- Messages ---

  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      auto* window = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(window));
      window->hwnd_ = hwnd;
    }
    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return window ? window->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
  }

  LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        Paint(hdc);
        EndPaint(hwnd_, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;
      case WM_NCHITTEST: {
        POINT p = {short(LOWORD(lparam)), short(HIWORD(lparam))};
        ScreenToClient(hwnd_, &p);
        const gp::PointF at(float(p.x) / scale_, float(p.y) / scale_);
        if (Hit(at)) return HTCLIENT;
        return HTCAPTION;  // drag the window by anything else
      }
      case WM_MOUSEMOVE: {
        const gp::PointF at(float(short(LOWORD(lparam))) / scale_, float(short(HIWORD(lparam))) / scale_);
        const int hover = HitIndex(at);
        if (hover != hover_) {
          hover_ = hover;
          Invalidate();
        }
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&track);
        return 0;
      }
      case WM_MOUSELEAVE:
        hover_ = -1;
        Invalidate();
        return 0;
      case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && hover_ >= 0) {
          SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));  // IDC_HAND
          return TRUE;
        }
        break;
      case WM_LBUTTONUP: {
        const gp::PointF at(float(short(LOWORD(lparam))) / scale_, float(short(HIWORD(lparam))) / scale_);
        if (close_rect_.Contains(at)) {
          Close();
          return 0;
        }
        for (const Toggle& t : toggles_) {
          if (t.rect.Contains(at)) {
            *t.value = !*t.value;
            Invalidate();
            return 0;
          }
        }
        for (const Button& b : buttons_) {
          if (b.enabled && b.rect.Contains(at)) {
            const Button pressed = b;
            Activate(pressed);
            return 0;
          }
        }
        return 0;
      }
      case WM_KEYDOWN:
        if (wparam == VK_RETURN) Press('A');
        if (wparam == VK_ESCAPE) Press('B');
        if (wparam == VK_BACK) Press('X');
        return 0;
      case WM_DROPFILES: {
        auto drop = HDROP(wparam);
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH) && (page_ == Page::kDisc || page_ == Page::kWelcome)) {
          page_ = Page::kDisc;
          CheckDiscAsync(path);
        }
        DragFinish(drop);
        return 0;
      }
      case WM_TIMER:
        Invalidate();
        return 0;
      case kDiscChecked: {
        if (checker_.joinable()) checker_.join();
        std::lock_guard lock(progress_.mutex);
        checking_ = false;
        disc_ok_ = checked_ok_;
        disc_error_ = checked_error_;
        disc_ = checked_disc_;
        Invalidate();
        return 0;
      }
      case kWorkerDone:
        if (worker_.joinable()) worker_.join();
        KillTimer(hwnd_, kTimer);
        if (closing_) {
          DestroyWindow(hwnd_);
          return 0;
        }
        if (error_.empty()) {
          page_ = mode == Mode::kUninstall ? Page::kUninstalled : Page::kDone;
          result_ = 0;
        } else {
          page_ = Page::kFailed;
          result_ = 1;
        }
        Invalidate();
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
  }

  bool Hit(const gp::PointF& at) { return close_rect_.Contains(at) || HitIndex(at) >= 0; }

  // Buttons 0.., toggles 100.., close 200; -1 for none.
  int HitIndex(const gp::PointF& at) {
    for (size_t i = 0; i < buttons_.size(); ++i) {
      if (buttons_[i].enabled && buttons_[i].rect.Contains(at)) return int(i);
    }
    for (size_t i = 0; i < toggles_.size(); ++i) {
      if (toggles_[i].rect.Contains(at)) return 100 + int(i);
    }
    if (close_rect_.Contains(at)) return 200;
    return -1;
  }

  // --- Drawing ---
  //
  // After the game's own screens (K2's): black with ink-wash clouds along the
  // top and bottom (the "Load game?" screen), headings on a red brush stroke
  // with plain white text under them (the loading tips), paper strips for
  // what you choose, and the A / B / X prompts on a torn paper strip at the
  // bottom. Flat colours and hand lettering, like the game.

  static const wchar_t* Family(std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* name : names) {
      gp::FontFamily family(name);
      if (family.IsAvailable()) return name;
    }
    return L"Segoe UI";
  }

  // Deterministic noise for the brush and paper edges (the same every frame).
  static float Noise(int i) {
    uint32_t x = uint32_t(i) * 2654435761u ^ 0x9E3779B9u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return float(x & 0xFFFF) / 65535.0f;
  }

  static gp::Color Rgb(BYTE r, BYTE g, BYTE b, BYTE a = 255) { return gp::Color(a, r, g, b); }

  void Paint(HDC hdc) {
    RECT client;
    GetClientRect(hwnd_, &client);
    gp::Bitmap frame(client.right, client.bottom, PixelFormat32bppPARGB);
    Render(frame);
    gp::Graphics screen(hdc);
    screen.DrawImage(&frame, 0, 0);
  }

  void Render(gp::Bitmap& frame) {
    gp::Graphics g(&frame);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHighQuality);
    g.ScaleTransform(scale_, scale_);
    g.Clear(Rgb(12, 12, 12));

    DrawClouds(g, false);
    DrawClouds(g, true);
    DrawMark(g);
    buttons_.clear();
    toggles_.clear();
    DrawPage(g);
    DrawPrompts(g);
    DrawClose(g);
  }

 public:
  // --render-pages <folder>: every page as a PNG, with made-up progress (design reviews).
  void RenderPages(const fs::path& folder) {
    CLSID png = {};
    UINT count = 0, size = 0;
    gp::GetImageEncodersSize(&count, &size);
    std::vector<uint8_t> buffer(size);
    auto* encoders = reinterpret_cast<gp::ImageCodecInfo*>(buffer.data());
    gp::GetImageEncoders(count, size, encoders);
    for (UINT i = 0; i < count; ++i) {
      if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0) png = encoders[i].Clsid;
    }
    scale_ = 1.0f;
    dir = L"D:\\Games\\Tenchu Z";
    disc_.path = L"E:\\Images\\Tenchu Z (USA).iso";
    disc_.bytes = 4645ull << 20;
    progress_.total = 4700ull << 20;
    progress_.done = 1810ull << 20;
    progress_.SetStep(L"Copying the game data from your disc image");
    struct Shot {
      const wchar_t* name;
      Page page;
      Mode mode;
      bool ok;
    };
    const Shot shots[] = {{L"1_welcome", Page::kWelcome, Mode::kInstall, false},
                          {L"2_disc", Page::kDisc, Mode::kInstall, true},
                          {L"3_folder", Page::kFolder, Mode::kInstall, true},
                          {L"4_installing", Page::kWorking, Mode::kInstall, true},
                          {L"5_done", Page::kDone, Mode::kInstall, true},
                          {L"6_failed", Page::kFailed, Mode::kInstall, true},
                          {L"7_uninstall", Page::kUninstall, Mode::kUninstall, true},
                          {L"8_uninstalled", Page::kUninstalled, Mode::kUninstall, true}};
    error_ = L"Not enough free space: 4.6 GB needed.";
    left_files_ = true;
    for (const Shot& shot : shots) {
      page_ = shot.page;
      mode = shot.mode;
      disc_ok_ = shot.ok;
      hover_ = shot.page == Page::kFolder ? 100 : -1;
      gp::Bitmap frame(int(kWidth), int(kHeight), PixelFormat32bppPARGB);
      Render(frame);
      frame.Save((folder / (std::wstring(shot.name) + L".png")).c_str(), &png, nullptr);
    }
  }

 private:

  // Ink-wash clouds: clusters of round puffs, each cluster one flat grey
  // shape with a paler rim along its outer edge, a row of darker ones
  // behind, like the game's menu borders.
  void DrawClouds(gp::Graphics& g, bool bottom) {
    for (int layer = 0; layer < 2; ++layer) {
      const BYTE body = layer == 0 ? 28 : 46, rim = layer == 0 ? 52 : 78;
      for (int i = 0; i < 9; ++i) {
        const int seed = i * 11 + layer * 131 + (bottom ? 977 : 0);
        const float cx = -60 + i * 118 + Noise(seed) * 40 + (layer ? 50 : 0);
        const float reach = (layer == 0 ? 40 : 22) + Noise(seed + 1) * 12;  // how far into the window
        const float base = bottom ? kHeight - reach : reach;                // the cluster's outer edge
        gp::GraphicsPath outer, inner;
        outer.SetFillMode(gp::FillModeWinding);
        inner.SetFillMode(gp::FillModeWinding);
        const int puffs = 3 + int(Noise(seed + 2) * 3);
        for (int k = 0; k < puffs; ++k) {
          const float r = 20 + Noise(seed + 10 + k) * 22;
          const float px = cx + (k - puffs / 2.0f) * 34 + Noise(seed + 20 + k) * 10;
          // Bigger puffs reach further; all sit on the band's edge.
          const float py = bottom ? base + r * 0.35f : base - r * 0.35f;
          outer.AddEllipse(px - r, py - r, r * 2, r * 2);
          const float shrink = 2.5f, shift = bottom ? 2.5f : -2.5f;  // the rim shows on the inward side
          inner.AddEllipse(px - r + shrink, py - r + shrink + shift, (r - shrink) * 2, (r - shrink) * 2);
        }
        gp::SolidBrush rim_brush(Rgb(rim, rim, rim));
        gp::SolidBrush body_brush(Rgb(body, body, body));
        g.FillPath(&rim_brush, &outer);
        g.FillPath(&body_brush, &inner);
        // One curl in the middle puff.
        gp::Pen curl(Rgb(BYTE(body + 18), BYTE(body + 18), BYTE(body + 18)), 1.6f);
        g.DrawArc(&curl, cx - 14, base + (bottom ? -6.0f : -14.0f), 26.0f, 18.0f, bottom ? 170.0f : -10.0f, 220.0f);
      }
      // The outer edge of the band is solid.
      gp::SolidBrush body_brush(Rgb(body, body, body));
      const float depth = layer == 0 ? 30 : 14;
      g.FillRectangle(&body_brush, gp::RectF(0, bottom ? kHeight - depth : 0, kWidth, depth));
    }
  }

  // A red brush stroke from (x, y), `w` long: a blunt, inked start, then the
  // bristles thinning out into ragged fingers with dry gaps between them.
  // `fraction` sets how far it has gone (the progress bar grows one).
  void BrushStroke(gp::Graphics& g, float x, float y, float w, float h, int seed, float fraction = 1.0f) {
    if (fraction <= 0) return;
    w = std::max(h, w * fraction);
    gp::SolidBrush red(Rgb(178, 24, 22));
    // The loaded part: a slightly wavering band with a rounded, heavier start.
    std::vector<gp::PointF> top, bottom_edge;
    const float body_end = w * 0.82f;
    const int steps = 16;
    for (int i = 0; i <= steps; ++i) {
      const float t = float(i) / steps;
      const float swell = t < 0.06f ? 0.7f + t * 5.0f : 1.0f;
      top.emplace_back(x + body_end * t, y + h * 0.5f - h * 0.5f * swell + (Noise(seed + i) - 0.5f) * 2.4f);
      bottom_edge.emplace_back(x + body_end * t, y + h * 0.5f + h * 0.52f * swell + (Noise(seed + 50 + i) - 0.5f) * 2.4f);
    }
    std::vector<gp::PointF> outline(top);
    outline.insert(outline.end(), bottom_edge.rbegin(), bottom_edge.rend());
    gp::GraphicsPath body;
    body.AddClosedCurve(outline.data(), int(outline.size()), 0.25f);
    g.FillPath(&red, &body);
    // The bristles: bands across the stroke's height running on past the
    // body by different lengths, each tapering to a point.
    const int bristles = 9;
    for (int i = 0; i < bristles; ++i) {
      const float top_y = y + h * i / bristles, band = h / bristles * (0.8f + Noise(seed + 700 + i) * 0.6f);
      const float start = body_end - w * 0.08f;
      const float end = body_end + (w - body_end) * (0.25f + Noise(seed + 800 + i) * 0.75f);
      const gp::PointF finger[] = {gp::PointF(start, top_y), gp::PointF(end - 6, top_y + band * 0.15f),
                                   gp::PointF(end, top_y + band * 0.5f), gp::PointF(end - 6, top_y + band * 0.85f),
                                   gp::PointF(start, top_y + band)};
      g.FillPolygon(&red, finger, 5);
    }
    // Dry gaps: thin dark streaks of different lengths, mostly late in the stroke.
    for (int i = 0; i < 4; ++i) {
      const float sy = y + 3 + Noise(seed + 200 + i) * (h - 6);
      const float sx = x + w * (0.5f + Noise(seed + 300 + i) * 0.3f);
      const float len = w * (0.06f + Noise(seed + 900 + i) * 0.3f);
      gp::Pen gap(Rgb(12, 12, 12, BYTE(140 + Noise(seed + 400 + i) * 115)), 0.7f + Noise(seed + 500 + i) * 1.2f);
      gap.SetEndCap(gp::LineCapTriangle);
      g.DrawLine(&gap, sx, sy, sx + len, sy + (Noise(seed + 600 + i) - 0.5f) * 2);
    }
  }

  // A torn paper strip.
  void Paper(gp::Graphics& g, const gp::RectF& r, int seed) {
    std::vector<gp::PointF> edge;
    const int across = int(r.Width / 12), down = std::max(2, int(r.Height / 10));
    for (int i = 0; i <= across; ++i) edge.emplace_back(r.X + r.Width * i / across, r.Y + Noise(seed + i) * 3.5f);
    for (int i = 1; i <= down; ++i) edge.emplace_back(r.GetRight() - Noise(seed + 100 + i) * 5, r.Y + r.Height * i / down);
    for (int i = across - 1; i >= 0; --i) edge.emplace_back(r.X + r.Width * i / across, r.GetBottom() - Noise(seed + 200 + i) * 3.5f);
    for (int i = down - 1; i >= 1; --i) edge.emplace_back(r.X + Noise(seed + 300 + i) * 5, r.Y + r.Height * i / down);
    gp::GraphicsPath path;
    path.AddPolygon(edge.data(), int(edge.size()));
    gp::SolidBrush paper(Rgb(200, 192, 170));
    g.FillPath(&paper, &path);
    // Mottling and fibres, faint.
    g.SetClip(&path);
    for (int i = 0; i < int(r.Width * r.Height / 260); ++i) {
      const float px = r.X + Noise(seed + 1000 + i) * r.Width, py = r.Y + Noise(seed + 3000 + i) * r.Height;
      gp::SolidBrush blot(Rgb(120, 100, 70, BYTE(10 + Noise(seed + 5000 + i) * 14)));
      const float s = 4 + Noise(seed + 7000 + i) * 16;
      g.FillEllipse(&blot, px, py, s, s * 0.6f);
    }
    g.ResetClip();
  }

  // Top right, small like the game's loading screen: TENCHU Z, two kanji.
  void DrawMark(gp::Graphics& g) {
    const gp::FontFamily hand(Family({L"Ink Free", L"Segoe Print"}));
    gp::Font word(&hand, 26, gp::FontStyleBold, gp::UnitPixel);
    gp::Font big(&hand, 34, gp::FontStyleBold, gp::UnitPixel);
    gp::SolidBrush grey(Rgb(196, 196, 196));
    gp::SolidBrush red(Rgb(196, 30, 24));
    g.DrawString(L"TENCHU", -1, &word, gp::PointF(668, 86), &grey);
    g.DrawString(L"Z", -1, &big, gp::PointF(790, 78), &red);
    const gp::FontFamily kanji(Family({L"Yu Mincho", L"MS Mincho", L"Yu Gothic", L"Meiryo"}));
    gp::Font k(&kanji, 20, gp::FontStyleBold, gp::UnitPixel);
    gp::SolidBrush orange(Rgb(226, 120, 30));
    g.DrawString(L"天", -1, &k, gp::PointF(818, 70), &orange);
    g.DrawString(L"誅", -1, &k, gp::PointF(824, 104), &orange);
  }

  void DrawPage(gp::Graphics& g) {
    const gp::FontFamily hand_family(Family({L"Ink Free", L"Segoe Print"}));
    const gp::FontFamily body_family(Family({L"Segoe UI"}));
    gp::Font heading(&hand_family, 24, gp::FontStyleBold, gp::UnitPixel);
    gp::Font line_font(&hand_family, 19, gp::FontStyleBold, gp::UnitPixel);
    gp::Font path_font(&body_family, 14, gp::FontStyleRegular, gp::UnitPixel);
    gp::Font note(&hand_family, 16, gp::FontStyleBold, gp::UnitPixel);
    gp::SolidBrush white(Rgb(236, 236, 236));
    gp::SolidBrush grey(Rgb(150, 150, 150));
    gp::SolidBrush ink(Rgb(36, 30, 24));
    gp::SolidBrush red(Rgb(226, 70, 52));
    gp::SolidBrush green(Rgb(124, 196, 84));
    const float x = 70, w = 560;
    float y = 0;
    auto banner = [&](const std::wstring& s) {
      gp::RectF size;
      g.MeasureString(s.c_str(), -1, &heading, gp::PointF(0, 0), &size);
      BrushStroke(g, 44, 82, std::max(230.0f, size.Width + 90), 40, int(s.size()) * 31);
      g.DrawString(s.c_str(), -1, &heading, gp::PointF(62, 85), &white);
      y = 146;
    };
    auto line = [&](const std::wstring& s, gp::Brush& brush, float gap = 6, gp::Font* font = nullptr) {
      gp::Font& f = font ? *font : line_font;
      gp::RectF box(x, y, w, 200), used;
      g.MeasureString(s.c_str(), -1, &f, box, &used);
      g.DrawString(s.c_str(), -1, &f, box, nullptr, &brush);
      y += used.Height + gap;
    };
    auto strip = [&](const std::wstring& s) {
      const gp::RectF r(x - 6, y, w + 12, 36);
      Paper(g, r, int(y) * 13);
      gp::StringFormat format;
      format.SetTrimming(gp::StringTrimmingEllipsisPath);
      format.SetFormatFlags(gp::StringFormatFlagsNoWrap);
      format.SetLineAlignment(gp::StringAlignmentCenter);
      g.DrawString(s.c_str(), -1, &path_font, gp::RectF(r.X + 14, r.Y, r.Width - 28, r.Height), &format, &ink);
      y += 48;
    };
    // A choice that turns on and off: a kunai points at it when on, like the
    // game's menu cursor, and a red stroke underlines it under the mouse.
    auto toggle = [&](const std::wstring& label, bool* value) {
      const int index = int(toggles_.size());
      gp::RectF size;
      g.MeasureString(label.c_str(), -1, &line_font, gp::PointF(0, 0), &size);
      if (hover_ == 100 + index) BrushStroke(g, x + 20, y + 4, size.Width + 30, 22, 700 + index * 17);
      Kunai(g, x - 2, y + 13, *value);
      g.DrawString(label.c_str(), -1, &line_font, gp::PointF(x + 30, y), *value ? &white : &grey);
      Toggle t;
      t.label = label;
      t.value = value;
      t.rect = gp::RectF(x - 10, y - 2, 50 + size.Width, 30);
      toggles_.push_back(t);
      y += 32;
    };

    switch (page_) {
      case Page::kWelcome:
        banner(L"Tenchu Z Setup");
        line(L"The Xbox 360 game, recompiled for Windows.", white, 14);
        line(L"You need your own Tenchu Z disc image (.iso).", grey);
        line(L"Nothing from the disc comes with this installer.", grey);
        AddButton('A', L"Next", true);
        AddButton('B', L"Cancel", true);
        break;
      case Page::kDisc:
        banner(mode == Mode::kRepair ? L"Repair game data" : L"Disc image");
        line(L"Your Tenchu Z (Xbox 360) disc image", grey, 10);
        strip(disc_.path.empty() ? L"None chosen. Browse, or drop the .iso here." : disc_.path.wstring());
        if (checking_) {
          line(L"Checking...", grey);
        } else if (disc_ok_) {
          line(L"Tenchu Z, " + Gigabytes(disc_.bytes), green);
        } else if (!disc_error_.empty()) {
          line(disc_error_, red);
        }
        AddButton('A', mode == Mode::kRepair ? L"Repair" : L"Next", disc_ok_ && !checking_);
        AddButton('X', L"Browse", !checking_);
        AddButton('B', mode == Mode::kRepair ? L"Cancel" : L"Back", true);
        break;
      case Page::kFolder: {
        banner(L"Install folder");
        strip(dir.wstring());
        uint64_t payload_bytes = 0;
        for (const PayloadFile& f : payload) payload_bytes += f.size;
        const uint64_t needed = disc_.bytes + payload_bytes;
        const uint64_t free_bytes = FreeBytes(dir);
        line(Gigabytes(needed) + L" needed, " + Gigabytes(free_bytes) + L" free", free_bytes < needed ? red : grey, 16,
             &note);
        toggle(L"Desktop shortcut", &desktop_shortcut_);
        toggle(L"Start menu shortcut", &start_menu_shortcut_);
        AddButton('A', L"Install", free_bytes >= needed);
        AddButton('X', L"Change", true);
        AddButton('B', L"Back", true);
        break;
      }
      case Page::kWorking: {
        banner(mode == Mode::kUninstall ? L"Uninstalling" : L"Installing");
        line(progress_.Step(), white, 18);
        const double fraction = double(progress_.done) / double(std::max<uint64_t>(1, progress_.total));
        // The bar: a faint line, inked over with a brush stroke as it fills.
        gp::SolidBrush track(Rgb(40, 40, 40));
        g.FillRectangle(&track, gp::RectF(x, y + 12, w, 3));
        BrushStroke(g, x - 4, y, w + 8, 28, 4242, float(std::min(1.0, fraction)));  // grows with it
        y += 44;
        wchar_t percent[96];
        if (mode == Mode::kUninstall) {
          std::swprintf(percent, 96, L"%d%%", int(fraction * 100));
        } else {
          std::swprintf(percent, 96, L"%d%%   %ls of %ls", int(fraction * 100), Gigabytes(progress_.done).c_str(),
                        Gigabytes(progress_.total).c_str());
        }
        line(percent, grey, 0, &note);
        if (mode != Mode::kUninstall) AddButton('B', progress_.cancel ? L"Stopping" : L"Stop", !progress_.cancel);
        break;
      }
      case Page::kDone:
        banner(mode == Mode::kRepair ? L"Repaired" : L"Installed");
        line(L"Tenchu Z is in", grey, 8);
        strip(dir.wstring());
        line(L"You can put the disc image away.", grey, 16, &note);
        toggle(L"Start Tenchu Z", &launch_);
        AddButton('A', L"Finish", true);
        break;
      case Page::kFailed:
        banner(L"Setup stopped");
        line(error_, red, 12);
        if (mode != Mode::kUninstall) line(L"Run setup again and it carries on from here.", grey, 0, &note);
        AddButton('A', L"Try again", true);
        AddButton('B', L"Close", true);
        break;
      case Page::kUninstall:
        banner(L"Uninstall Tenchu Z");
        strip(dir.wstring());
        toggle(L"Keep my saves and settings", &keep_saves_);
        AddButton('A', L"Uninstall", true);
        AddButton('B', L"Cancel", true);
        break;
      case Page::kUninstalled:
        banner(L"Uninstalled");
        line(L"Tenchu Z was removed.", white, 10);
        if (left_files_) {
          line(keep_saves_ ? L"Your saves and settings are still in its folder."
                           : L"Its folder holds files Setup didn't make, so it stays.",
               grey, 0, &note);
        }
        AddButton('A', L"Close", true);
        break;
    }
  }

  // The game's menu cursor: a kunai, point right. Dim when its item is off.
  void Kunai(gp::Graphics& g, float x, float y, bool on) {
    const BYTE a = on ? 255 : 70;
    gp::SolidBrush blade(Rgb(214, 216, 222, a));
    gp::SolidBrush grip(Rgb(70, 50, 40, a));
    const gp::PointF leaf[] = {gp::PointF(x + 8, y - 5), gp::PointF(x + 24, y), gp::PointF(x + 8, y + 5),
                               gp::PointF(x + 10, y)};
    g.FillPolygon(&blade, leaf, 4);
    g.FillRectangle(&grip, gp::RectF(x, y - 1.5f, 9, 3));
    gp::Pen ring(Rgb(160, 160, 166, a), 1.2f);
    g.DrawEllipse(&ring, x - 5, y - 2.5f, 5.0f, 5.0f);
  }

  void AddButton(wchar_t glyph, const std::wstring& label, bool enabled) {
    Button b;
    b.glyph = glyph;
    b.label = label;
    b.enabled = enabled;
    buttons_.push_back(b);
  }

  // The prompts, centred on a paper strip near the bottom: A, X, B.
  void DrawPrompts(gp::Graphics& g) {
    const gp::FontFamily hand(Family({L"Ink Free", L"Segoe Print"}));
    const gp::FontFamily sans(Family({L"Segoe UI"}));
    gp::Font label(&hand, 18, gp::FontStyleBold, gp::UnitPixel);
    gp::Font glyph_font(&sans, 11, gp::FontStyleBold, gp::UnitPixel);
    std::vector<float> widths;
    float total = 0;
    for (const Button& b : buttons_) {
      gp::RectF size;
      g.MeasureString(b.label.c_str(), -1, &label, gp::PointF(0, 0), &size);
      widths.push_back(22 + 6 + size.Width);
      total += widths.back();
    }
    total += 34.0f * float(std::max<size_t>(1, buttons_.size()) - 1);
    const float strip_w = std::max(300.0f, total + 80);
    const gp::RectF strip((kWidth - strip_w) / 2, 404, strip_w, 40);
    Paper(g, strip, 9090);
    float cx = (kWidth - total) / 2;
    const float cy = strip.Y + strip.Height / 2;
    for (size_t i = 0; i < buttons_.size(); ++i) {
      Button& b = buttons_[i];
      b.rect = gp::RectF(cx - 6, strip.Y, widths[i] + 12, strip.Height);
      const bool hot = hover_ == int(i) && b.enabled;
      const BYTE a = b.enabled ? 255 : 80;
      const gp::Color face = b.glyph == 'A' ? Rgb(92, 166, 52, a) : b.glyph == 'B' ? Rgb(200, 38, 32, a) : Rgb(40, 100, 200, a);
      gp::SolidBrush disc(face);
      g.FillEllipse(&disc, cx, cy - 11, 22.0f, 22.0f);
      gp::Pen ring(Rgb(30, 26, 22, a), 1.0f);
      g.DrawEllipse(&ring, cx, cy - 11, 22.0f, 22.0f);
      gp::StringFormat center;
      center.SetAlignment(gp::StringAlignmentCenter);
      center.SetLineAlignment(gp::StringAlignmentCenter);
      gp::SolidBrush letter(Rgb(255, 255, 255, a));
      const wchar_t glyph[2] = {b.glyph, 0};
      g.DrawString(glyph, -1, &glyph_font, gp::RectF(cx, cy - 11, 22, 22), &center, &letter);
      gp::SolidBrush text(hot ? Rgb(150, 20, 18) : Rgb(36, 30, 24, a));
      gp::StringFormat middle;
      middle.SetLineAlignment(gp::StringAlignmentCenter);
      g.DrawString(b.label.c_str(), -1, &label, gp::RectF(cx + 28, strip.Y, widths[i], strip.Height), &middle, &text);
      cx += widths[i] + 34;
    }
  }

  void DrawClose(gp::Graphics& g) {
    close_rect_ = gp::RectF(kWidth - 42, 8, 30, 30);
    gp::Pen x_pen(hover_ == 200 ? Rgb(226, 70, 52) : Rgb(170, 170, 170), 2.0f);
    x_pen.SetStartCap(gp::LineCapRound);
    x_pen.SetEndCap(gp::LineCapRound);
    const gp::RectF& r = close_rect_;
    g.DrawLine(&x_pen, r.X + 9, r.Y + 9, r.GetRight() - 9, r.GetBottom() - 9);
    g.DrawLine(&x_pen, r.GetRight() - 9, r.Y + 9, r.X + 9, r.GetBottom() - 9);
  }

  HWND hwnd_ = nullptr;
  float scale_ = 1.0f;
  Page page_ = Page::kWelcome;
  int hover_ = -1;
  int result_ = 1;
  std::vector<Button> buttons_;
  std::vector<Toggle> toggles_;
  gp::RectF close_rect_;

  Disc disc_;
  bool disc_ok_ = false, checking_ = false;
  std::wstring disc_error_;
  Disc checked_disc_;
  std::wstring checked_error_;
  bool checked_ok_ = false;
  std::thread checker_;

  bool desktop_shortcut_ = true, start_menu_shortcut_ = true, launch_ = true, keep_saves_ = true;
  Progress progress_;
  std::thread worker_;
  std::wstring error_;
  bool left_files_ = false;
  bool closing_ = false;
};

// The install folder an uninstall is for: --dir, else the registered one, else this exe's.
fs::path InstalledFolder(const fs::path& self) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
    wchar_t location[MAX_PATH] = {};
    DWORD size = sizeof(location);
    const bool found = RegQueryValueExW(key, L"InstallLocation", nullptr, nullptr, reinterpret_cast<BYTE*>(location),
                                        &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (found && fs::exists(fs::path(location) / L"tenchuz.exe")) return location;
  }
  return self.parent_path();
}

// The uninstaller can't delete its own running exe: it runs from a copy in
// %TEMP%, which waits for this one to exit and removes itself at the end.
bool RelaunchFromTemp(const fs::path& self, const fs::path& dir, const std::wstring& extra) {
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  const fs::path copy = fs::path(temp) / (L"TenchuZ-Uninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
  if (!CopyFileW(self.c_str(), copy.c_str(), FALSE)) return false;
  std::wstring command = L"\"" + copy.wstring() + L"\" --uninstall --from-temp " +
                        std::to_wstring(GetCurrentProcessId()) + L" --dir \"" + dir.wstring() + L"\"" + extra;
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(copy.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, temp, &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

void DeleteSelfLater(const fs::path& self) {
  std::wstring command = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + self.wstring() + L"\"";
  STARTUPINFOW startup = {sizeof(startup)};
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process = {};
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                     &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
}

// --yes: the whole install or uninstall without the window, printing progress.
int RunUnattended(Mode mode, const fs::path& self, std::vector<PayloadFile>& payload, fs::path iso_path, fs::path dir,
                  bool shortcuts, bool launch, bool keep_saves) {
  Progress progress;
  if (mode == Mode::kUninstall) {
    const UninstallResult result = Uninstall(dir, keep_saves, progress);
    if (!result.error.empty()) return Fail(result.error);
    std::wprintf(L"Tenchu Z was removed from %ls.\n", dir.c_str());
    return 0;
  }
  if (iso_path.empty()) return Fail(L"--yes needs --iso <your Tenchu Z disc image>. Nothing was installed.");
  InstallOptions options;
  options.self = self;
  options.payload = mode == Mode::kRepair ? std::vector<PayloadFile>{} : payload;
  std::wstring error;
  if (!CheckDisc(iso_path, options.disc, error)) return Fail(error + L" Nothing was installed.");
  std::wprintf(L"Tenchu Z disc image OK: %zu files, %ls\n", options.disc.files.size(),
               Gigabytes(options.disc.bytes).c_str());
  options.dir = dir.empty() ? SuggestedFolder() : dir;
  options.desktop_shortcut = false;
  options.start_menu_shortcut = shortcuts;
  std::wprintf(L"Install folder: %ls\n", options.dir.c_str());
  error = Install(options, progress);
  if (!error.empty()) return Fail(error);
  std::wprintf(L"Tenchu Z is installed in %ls.\n", options.dir.c_str());
  (void)launch;
  return 0;
}

// Setup is a windowed program; --pack and --yes print to the console they were started from.
void AttachParentConsole() {
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
  std::FILE* console = nullptr;
  freopen_s(&console, "CONOUT$", "w", stdout);
  freopen_s(&console, "CONOUT$", "w", stderr);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  wchar_t self_path[MAX_PATH];
  GetModuleFileNameW(nullptr, self_path, MAX_PATH);
  const fs::path self(self_path);

  fs::path iso_path, install_dir;
  bool shortcuts = true, launch = true, uninstall = false, from_temp = false, keep_saves = true;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--pack" && i + 2 < argc) {
      AttachParentConsole();
      return Pack(argv[i + 1], argv[i + 2]);
    } else if (arg == L"--iso" && i + 1 < argc) {
      iso_path = argv[++i];
    } else if (arg == L"--dir" && i + 1 < argc) {
      install_dir = argv[++i];
    } else if (arg == L"--yes") {
      g_yes = true;
    } else if (arg == L"--no-shortcut") {
      shortcuts = false;
    } else if (arg == L"--no-launch") {
      launch = false;
    } else if (arg == L"--uninstall") {
      uninstall = true;
    } else if (arg == L"--from-temp" && i + 1 < argc) {
      from_temp = true;
      // The Setup.exe that started this copy: its folder's files are free once it exits.
      if (HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, DWORD(std::wcstoul(argv[++i], nullptr, 10)))) {
        WaitForSingleObject(parent, 10000);
        CloseHandle(parent);
      }
    } else if (arg == L"--render-pages" && i + 1 < argc) {
      gp::GdiplusStartupInput startup_input;
      ULONG_PTR gdiplus = 0;
      gp::GdiplusStartup(&gdiplus, &startup_input, nullptr);
      {
        Window window;
        window.RenderPages(argv[i + 1]);
      }
      gp::GdiplusShutdown(gdiplus);
      return 0;
    } else if (arg == L"--remove-saves") {
      keep_saves = false;
    } else if (arg.rfind(L"--", 0) != 0) {
      iso_path = arg;  // a disc image dropped on the exe
    }
  }
  LocalFree(argv);
  if (g_yes) AttachParentConsole();

  std::vector<PayloadFile> payload;
  const bool installer = ReadPayload(self, payload);
  Mode mode = Mode::kInstall;
  if (uninstall) {
    mode = Mode::kUninstall;
    if (install_dir.empty()) install_dir = InstalledFolder(self);
    if (!fs::exists(install_dir / L"tenchuz.exe") && !fs::exists(install_dir / kManifest)) {
      const std::wstring message = L"Tenchu Z isn't installed in " + install_dir.wstring() + L".";
      if (!g_yes) MessageBoxW(nullptr, message.c_str(), kTitle, MB_OK | MB_ICONINFORMATION);
      return Fail(message);
    }
    // Running from inside the install folder: go on from a copy that can delete itself.
    if (!from_temp && SamePath(self.parent_path(), install_dir)) {
      return RelaunchFromTemp(self, install_dir,
                              std::wstring(g_yes ? L" --yes" : L"") + (keep_saves ? L"" : L" --remove-saves"))
                 ? 0
                 : Fail(L"Can't start the uninstaller.");
    }
  } else if (!installer) {
    mode = Mode::kRepair;
    install_dir = self.parent_path();  // repair: extract the disc next to an installed tenchuz.exe
    if (!fs::exists(install_dir / L"tenchuz.exe")) {
      const wchar_t message[] = L"This Setup.exe has no game files. Run TenchuZ-Setup.exe to install.";
      if (!g_yes) MessageBoxW(nullptr, message, kTitle, MB_OK | MB_ICONERROR);
      return Fail(message);
    }
  }

  int result;
  if (g_yes) {
    result = RunUnattended(mode, self, payload, iso_path, install_dir, shortcuts, launch, keep_saves);
  } else {
    SetProcessDPIAware();
    gp::GdiplusStartupInput startup_input;
    ULONG_PTR gdiplus = 0;
    gp::GdiplusStartup(&gdiplus, &startup_input, nullptr);
    {
      Window window;
      window.mode = mode;
      window.self = self;
      window.payload = std::move(payload);
      window.dir = install_dir;
      window.iso_arg = iso_path;
      window.from_temp = from_temp;
      result = window.Run();
    }
    gp::GdiplusShutdown(gdiplus);
  }
  CoUninitialize();
  if (from_temp) DeleteSelfLater(self);
  return result;
}
