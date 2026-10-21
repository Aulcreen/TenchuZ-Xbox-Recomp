// tenchuz setup - the installer.
//
// TenchuZ-Setup.exe is this program with the game's files appended
// (tools/package_release.ps1 builds it with --pack). Installing needs the
// player's own Tenchu Z disc image: Setup checks that the image's default.xex
// is the version this build was recompiled from before it installs anything,
// then writes the game files and the disc's file system (game\) into the
// install folder and makes shortcuts. The installed game runs from that
// folder alone; the disc image isn't used again. Nothing from the disc ships
// with the installer.
//
//   TenchuZ-Setup.exe [--iso <image.iso>] [--dir <folder>] [--yes]
//                     [--no-shortcut] [--no-launch]
//   Setup.exe --pack <folder> <out.exe>   (packaging)
//
// --yes answers every question with its default (unattended installs).
// Without a payload (the plain Setup.exe left in an install folder), Setup
// only (re)extracts the disc image into game\ next to it.
//
// The disc reader follows tools/xiso.py: Redump images (XGD1/2/3 video
// partition offsets) and trimmed game-partition images.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kSector = 0x800;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr uint64_t kPartitionOffsets[] = {0x0, 0xFD90000, 0x2080000, 0x18300000};
// default.xex of the release this build was recompiled from (Tenchu Z,
// Title ID 465307D7, 11,649,024 bytes).
constexpr char kXexSha256[] = "C727DEBEAAD474DFCF8517202EEBC388F9E51705CE3B01499486FA5FF47F9F06";
constexpr wchar_t kTitle[] = L"Tenchu Z setup";
constexpr char kPayloadMagic[8] = {'T', 'Z', 'S', 'E', 'T', 'U', 'P', '1'};

bool g_yes = false;  // --yes: unattended

// --- Messages ---------------------------------------------------------------

int Fail(const std::wstring& message) {
  std::fwprintf(stderr, L"\n%ls\n", message.c_str());
  if (!g_yes) MessageBoxW(nullptr, message.c_str(), kTitle, MB_OK | MB_ICONERROR);
  return 1;
}

// Yes/No question; --yes picks `unattended`.
bool Ask(const std::wstring& question, bool unattended) {
  if (g_yes) return unattended;
  return MessageBoxW(nullptr, question.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES;
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

bool InstallPayload(const fs::path& exe, const std::vector<PayloadFile>& files, const fs::path& dir) {
  File self(exe, false);
  DECOMPRESSOR_HANDLE decompressor = nullptr;
  if (!self.ok() || !CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor)) return false;
  bool ok = true;
  for (const PayloadFile& f : files) {
    const fs::path path = JoinPath(dir, f.name);
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::vector<uint8_t> packed(size_t(f.packed)), data(size_t(f.size));
    SIZE_T got = 0;
    if ((f.packed && (!self.ReadAt(f.offset, packed.data(), packed.size()) ||
                      !Decompress(decompressor, packed.data(), packed.size(), data.data(), data.size(), &got) ||
                      got != f.size))) {
      ok = false;
      break;
    }
    File out(path, true);
    if (!out.ok() || (!data.empty() && !out.Write(data.data(), data.size()))) {
      ok = false;
      break;
    }
    std::wprintf(L"  %ls\n", Widen(f.name).c_str());
  }
  CloseDecompressor(decompressor);
  return ok;
}

// --- Dialogs and shell --------------------------------------------------------

fs::path PickIso() {
  fs::path result;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    return result;
  }
  const COMDLG_FILTERSPEC filters[] = {{L"Disc images (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
  dialog->SetFileTypes(2, filters);
  dialog->SetTitle(L"Choose your Tenchu Z (Xbox 360) disc image");
  if (SUCCEEDED(dialog->Show(nullptr))) {
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

fs::path PickFolder() {
  fs::path result;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    return result;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(L"Choose where to install Tenchu Z (a \"Tenchu Z\" folder is made inside)");
  if (SUCCEEDED(dialog->Show(nullptr))) {
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
  while (!probe.empty() && !fs::exists(probe)) probe = probe.parent_path();
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

bool CreateShortcut(const fs::path& target, const fs::path& link) {
  IShellLinkW* shell_link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link)))) {
    return false;
  }
  shell_link->SetPath(target.c_str());
  shell_link->SetWorkingDirectory(target.parent_path().c_str());
  shell_link->SetDescription(L"Tenchu Z (PC recompilation)");
  IPersistFile* file = nullptr;
  bool ok = SUCCEEDED(shell_link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(link.c_str(), TRUE));
  if (file) file->Release();
  shell_link->Release();
  return ok;
}

fs::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  fs::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) result = path;
  CoTaskMemFree(path);
  return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  SetConsoleTitleW(kTitle);
  wchar_t self_path[MAX_PATH];
  GetModuleFileNameW(nullptr, self_path, MAX_PATH);
  const fs::path self(self_path);

  fs::path iso_path, install_dir;
  bool offer_shortcut = true, offer_launch = true;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--pack" && i + 2 < argc) {
      return Pack(argv[i + 1], argv[i + 2]);
    } else if (arg == L"--iso" && i + 1 < argc) {
      iso_path = argv[++i];
    } else if (arg == L"--dir" && i + 1 < argc) {
      install_dir = argv[++i];
    } else if (arg == L"--yes") {
      g_yes = true;
    } else if (arg == L"--no-shortcut") {
      offer_shortcut = false;
    } else if (arg == L"--no-launch") {
      offer_launch = false;
    } else if (arg.rfind(L"--", 0) != 0) {
      iso_path = arg;  // a disc image dropped on the exe
    }
  }

  std::vector<PayloadFile> payload;
  const bool installer = ReadPayload(self, payload);
  std::wprintf(L"Tenchu Z setup\n==============\n\n");
  if (!installer) {
    install_dir = self.parent_path();  // repair: extract the disc next to an installed tenchuz.exe
    if (!fs::exists(install_dir / L"tenchuz.exe")) {
      return Fail(L"This Setup.exe has no game files. Run TenchuZ-Setup.exe to install.");
    }
  } else if (!g_yes &&
             MessageBoxW(nullptr,
                         L"This installs Tenchu Z (PC recompilation of the Xbox 360 game).\n\n"
                         L"You need your own Tenchu Z (Xbox 360) disc image (.iso). Nothing from the game disc "
                         L"comes with this installer, and once installed the game doesn't need the disc image.",
                         kTitle, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
    return 1;
  }

  // 1. The disc image, checked before anything is installed.
  if (iso_path.empty() && !g_yes) iso_path = PickIso();
  if (iso_path.empty()) return Fail(L"Installing Tenchu Z needs your Tenchu Z disc image. Nothing was installed.");
  std::wprintf(L"Disc image: %ls\n", iso_path.c_str());
  Iso iso(iso_path);
  if (!iso.ok()) return Fail(L"Can't open the disc image. Nothing was installed.");
  std::vector<Entry> files;
  if (!iso.FindPartition() || !iso.List(files) || files.empty()) {
    return Fail(L"This isn't an Xbox 360 disc image (no XDVDFS file system). Nothing was installed.");
  }
  auto xex = std::find_if(files.begin(), files.end(),
                          [](const Entry& e) { return _stricmp(e.path.c_str(), "default.xex") == 0; });
  if (xex == files.end()) return Fail(L"This disc image has no default.xex: not a game disc. Nothing was installed.");
  std::vector<uint8_t> xex_data(xex->size);
  if (!iso.ReadEntry(*xex, 0, xex_data.data(), xex_data.size()) ||
      Sha256(xex_data.data(), xex_data.size()) != kXexSha256) {
    return Fail(L"This disc image isn't the Tenchu Z (Xbox 360) release this build was made from (default.xex "
                L"differs). Nothing was installed.");
  }
  uint64_t disc_bytes = 0;
  for (const Entry& e : files) disc_bytes += e.size;
  std::wprintf(L"Tenchu Z disc image OK: %zu files, %.2f GB\n\n", files.size(), double(disc_bytes) / (1 << 30));

  // 2. Where to install.
  if (installer && install_dir.empty()) {
    const fs::path suggested = SuggestedFolder();
    if (Ask(L"Install Tenchu Z to\n\n" + suggested.wstring() + L"\n\n(about 4.5 GB)? Choose No to pick another folder.",
            true)) {
      install_dir = suggested;
    } else {
      install_dir = PickFolder();
      if (install_dir.empty()) return Fail(L"No install folder chosen. Nothing was installed.");
    }
  }
  if (!Writable(install_dir)) {
    return Fail(L"Can't write to " + install_dir.wstring() +
                L". Choose a folder you can write to (not inside Program Files): the game keeps saves there.");
  }
  const uint64_t needed = disc_bytes + (200ull << 20);
  if (FreeBytes(install_dir) < needed) {
    return Fail(L"Not enough free space in " + install_dir.wstring() + L": about " +
                std::to_wstring((needed >> 20) / 1024 + 1) + L" GB needed.");
  }
  std::wprintf(L"Install folder: %ls\n", install_dir.c_str());

  // 3. The game files.
  if (installer) {
    std::wprintf(L"Installing the game files...\n");
    if (!InstallPayload(self, payload, install_dir)) return Fail(L"Writing the game files failed.");
  }

  // 4. The disc's file system into game\ (resumable: right-sized files are kept).
  const fs::path game_dir = install_dir / L"game";
  std::wprintf(L"Copying the game data from the disc image...\n");
  std::vector<uint8_t> buffer(16u << 20);
  uint64_t done = 0;
  int last_percent = -1;
  for (const Entry& e : files) {
    const fs::path out = JoinPath(game_dir, e.path);
    std::error_code error;
    if (fs::is_regular_file(out, error) && fs::file_size(out, error) == e.size) {
      done += e.size;
      continue;
    }
    fs::create_directories(out.parent_path(), error);
    File file(out, true);
    if (!file.ok()) return Fail(L"Can't write " + out.wstring());
    for (uint64_t offset = 0; offset < e.size;) {
      const uint32_t chunk = uint32_t(std::min<uint64_t>(e.size - offset, buffer.size()));
      if (!iso.ReadEntry(e, offset, buffer.data(), chunk) || !file.Write(buffer.data(), chunk)) {
        return Fail(L"Copying " + out.wstring() + L" failed (disk full, or a damaged disc image?).");
      }
      offset += chunk;
      done += chunk;
      const int percent = disc_bytes ? int(done * 100 / disc_bytes) : 100;
      if (percent != last_percent) {
        last_percent = percent;
        std::wprintf(L"\r  %3d%%", percent);
      }
    }
  }
  std::wprintf(L"\r  100%%\n");
  if (Sha256File(game_dir / L"default.xex") != kXexSha256) {
    return Fail(L"The copied default.xex doesn't match the disc image's. Run setup again.");
  }

  // 5. Shortcuts and start.
  const fs::path exe = install_dir / L"tenchuz.exe";
  if (installer && offer_shortcut) {
    const fs::path programs = KnownFolder(FOLDERID_Programs);
    if (!programs.empty()) CreateShortcut(exe, programs / L"Tenchu Z.lnk");
    const fs::path desktop = KnownFolder(FOLDERID_Desktop);
    if (!desktop.empty() && Ask(L"Add a Tenchu Z shortcut to the desktop?", false)) {
      CreateShortcut(exe, desktop / L"Tenchu Z.lnk");
    }
  }
  std::wprintf(L"\nTenchu Z is installed in %ls. The disc image isn't needed any more.\n", install_dir.c_str());
  if (offer_launch && Ask(L"Tenchu Z is installed. Start it now?", false)) {
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, install_dir.c_str(),
                       &startup, &process)) {
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    }
  }
  CoUninitialize();
  return 0;
}
