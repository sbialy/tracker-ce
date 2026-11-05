// NOLINTBEGIN
#ifdef FEATURE_SYNTH
#include "FileSystemAdapter.h"
#include "config.h"
#ifdef FEATURE_SYNTH_OPEN
#include "open_fileops.h"
#endif

#include "cr_section_macros.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace FileSystemAdapterBuffers
{
__NOINIT(EXTERNAL_RAM) static uint8_t fileNames[kMaxFilesListLength * (kMaxFileNameLen + 1)];
__NOINIT(EXTERNAL_RAM) static uint8_t listDataName[kMaxFilesListLength * sizeof(char**)];
}

char* FileSystemAdapter::fileNamesBuffer = nullptr;
char** FileSystemAdapter::listDataName = nullptr;
FileSystemAdapter::FileSystemAdapter(const std::string& pathPrefix,
                                     ::FileSystem* platformFileSystem)
  : platformFileSystem(platformFileSystem)
  , pathPrefix(pathPrefix)
{
}

void FileSystemAdapter::staticInit()
{
#ifdef DEBUG
    static int instanceCounter = 0;
    assert(instanceCounter++ == 0);
#endif
    fileNamesBuffer = (char*)FileSystemAdapterBuffers::fileNames;
    listDataName = (char**)FileSystemAdapterBuffers::listDataName;
}

std::vector<std::string_view> FileSystemAdapter::listFiles(
  const std::string& path,
  std::function<bool(const ::std::string& name)> matcher,
  tiny_optional<int> /*maxFiles*/)
{

    memset(listDataName, 0, kMaxFilesListLength);

    fileCount = kMaxFilesListLength;
    platformFileSystem->listDirContents((pathPrefix + path).c_str(),
                                        ::FileSystem::DirContentType::kFiles,
                                        matcher,
                                        fileNamesBuffer,
                                        kMaxFilesListLength * (kMaxFileNameLen + 1),
                                        kMaxFileNameLen,
                                        fileCount,
                                        listDataName);

    std::vector<std::string_view> fileList;

    std::ranges::transform(std::span(listDataName, fileCount),
                           std::back_inserter(fileList),
                           [](char* file) { return std::string_view(file); });

    return fileList;
}

std::optional<polysynth::FileSystem::FileHandle> FileSystemAdapter::open(
  const std::string& path,
  polysynth::FileSystem::OpenOptions options)
{
    auto [result, file] = platformFileSystem->openFile(
      (pathPrefix + path).c_str(),
      (static_cast<int>(options) & static_cast<int>(FileSystem::OpenOptions::kWrite)) != 0
        ? ::FileSystem::FileOpenMode::kWriteCreateNew
        : ::FileSystem::FileOpenMode::kRead);

    return result ? std::optional<polysynth::FileSystem::FileHandle>(file) : std::nullopt;
}

void FileSystemAdapter::close(FileHandle file)
{
    platformFileSystem->closeFile(file);
}

bool FileSystemAdapter::write(FileHandle file, std::span<const uint8_t> data)
{
    return platformFileSystem->writeFile(data.data(), data.size(), file) ==
           static_cast<int32_t>(data.size());
}

int32_t FileSystemAdapter::read(FileHandle file, std::span<uint8_t> buf)
{
    return platformFileSystem->readFile(buf.data(), buf.size(), file);
}

bool FileSystemAdapter::seek(FileHandle file, int offset)
{
    return platformFileSystem->seekFileCur(offset, file);
}

void FileSystemAdapter::createFolder(const std::string& path)
{
    platformFileSystem->mkdir(0, (pathPrefix + path).c_str());
}

#ifdef FEATURE_SYNTH_OPEN
extern "C" unsigned f_stat(const char* path, void* fileInfo); // FatFs (platforms/mk66/3rdparty/SD/fatfs/source/ff.h), FR_OK 0, FR_NO_FILE 4

polysynth::FileSystem::Presence FileSystemAdapter::statPath(const char* path)
{
    // The platform exists() folds every card error into "not there". FatFs answers FR_NO_FILE only after it walked the folder and found
    // no such entry (a read error is another code, and FR_NO_PATH, a folder that is not there, is not taken as proof either).
    constexpr unsigned kFrOk = 0;
    constexpr unsigned kFrNoFile = 4;
    const unsigned r = f_stat(path, nullptr);
    return r == kFrOk ? Presence::kPresent : r == kFrNoFile ? Presence::kAbsent : Presence::kUnknown;
}

polysynth::FileSystem::Presence FileSystemAdapter::presence(const std::string& path)
{
    return statPath((pathPrefix + path).c_str());
}

bool FileSystemAdapter::replace(const std::string& from, const std::string& to)
{
    return replaceFile(platformFileSystem, (pathPrefix + from).c_str(), (pathPrefix + to).c_str());
}

bool FileSystemAdapter::remove(const std::string& path)
{
    return platformFileSystem->remove((pathPrefix + path).c_str());
}

namespace
{
struct PlatformFileOps
{
    ::FileSystem* fs;
    polysynth::FileSystem::Presence stat(const char* path) { return FileSystemAdapter::statPath(path); }
    bool remove(const char* path) { return fs->remove(path); }
    bool rename(const char* from, const char* to) { return fs->rename(from, to); }
};
} // namespace

bool FileSystemAdapter::replaceFile(::FileSystem* fileSystem, const char* from, const char* to)
{
    PlatformFileOps ops{ fileSystem };
    return polysynth::open::replaceFileOps(ops, from, to);
}
#endif
#endif
// NOLINTEND
