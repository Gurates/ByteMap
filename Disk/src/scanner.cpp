#include "scanner.h"
#include "mft_scanner.h"
#include <algorithm>
#include <execution>

namespace fs = std::filesystem;

DiskScanner::DiskScanner()
    : isScanning(false), shouldStop(false), progress(0.0f), totalFilesScanned(0), usedMft(false)
{
    results[FileCategory::Application] = { "Applications (.exe, .dll)", 0, 0 };
    results[FileCategory::Media] = { "Media (.mp4, .jpg, .mp3)",  0, 0 };
    results[FileCategory::Document] = { "Documents (.pdf, .docx)",   0, 0 };
    results[FileCategory::Junk] = { "Junk Files (.tmp, .log)",   0, 0 };
    results[FileCategory::Other] = { "Other",                     0, 0 };
}

DiskScanner::~DiskScanner() {
    StopScan();
}

void DiskScanner::StopScan() {
    shouldStop = true;
    if (scanThread.joinable())
        scanThread.join();
    shouldStop = false;
}

void DiskScanner::Reset() {
    StopScan();
    for (auto& pair : results) {
        pair.second.totalSize = 0;
        pair.second.fileCount = 0;
    }
    largeFiles.clear();
    rootNode.reset();
    totalFilesScanned = 0;
    currentScannedSize = 0;
    progress = 0.0f;
    usedMft = false;
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentScanPath = "";
    }
}

void DiskScanner::StartFullScan(const std::string& rootPath) {
    if (isScanning) return;
    Reset();
    isScanning = true;

    if (scanThread.joinable())
        scanThread.join();

    scanThread = std::thread(&DiskScanner::ScanWorker, this, rootPath);
}

void DiskScanner::ScanWorker(std::string rootPath) {
    if (IsNtfsVolume(rootPath)) {
        ScanWorkerMft(rootPath);
    }
    else {
        ScanWorkerClassic(rootPath);
    }
}

void DiskScanner::ScanWorkerMft(const std::string& rootPath) {
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentScanPath = "Reading Master File Table (MFT) from disk... Please wait.";
    }

    MftScanResult mftRes = EnumerateMft(rootPath, shouldStop);

    if (shouldStop) {
        progress = 1.0f;
        isScanning = false;
        return;
    }

    if (!mftRes.ok) {
        ScanWorkerClassic(rootPath);
        return;
    }

    usedMft = true;
    auto root = std::make_shared<FolderNode>();
    root->name = rootPath;
    root->fullPath = rootPath;

    std::unordered_map<DWORDLONG, std::shared_ptr<FolderNode>> dirMap;

    DWORDLONG rootRef = 0;
    for (const auto& rec : mftRes.records) {
        if (rec.isDir && (rec.fileRef & 0xFFFFFFFFFFFFULL) == 5) {
            rootRef = rec.fileRef;
            break;
        }
    }
    if (rootRef == 0) {
        for (const auto& rec : mftRes.records) {
            if (rec.isDir && rec.parentRef == rec.fileRef) {
                rootRef = rec.fileRef;
                break;
            }
        }
    }
    dirMap[rootRef] = root;

    for (const auto& rec : mftRes.records) {
        if (shouldStop) break;
        if (rec.isDir && rec.fileRef != rootRef) {
            auto node = std::make_shared<FolderNode>();
            node->name = std::string(rec.name.begin(), rec.name.end());
            dirMap[rec.fileRef] = node;
        }
    }

    for (const auto& rec : mftRes.records) {
        if (shouldStop) break;
        if (rec.isDir && rec.fileRef != rootRef) {
            auto it = dirMap.find(rec.fileRef);
            auto pIt = dirMap.find(rec.parentRef);
            if (it != dirMap.end()) {
                if (pIt != dirMap.end()) {
                    it->second->parent = pIt->second.get();
                    pIt->second->children.push_back(it->second);
                    std::string pFp = pIt->second->fullPath;
                    if (!pFp.empty() && pFp.back() == '\\') pFp.pop_back();
                    it->second->fullPath = pFp + "\\" + it->second->name;
                }
                else {
                    it->second->parent = root.get();
                    root->children.push_back(it->second);
                    std::string rFp = rootPath;
                    if (!rFp.empty() && rFp.back() == '\\') rFp.pop_back();
                    it->second->fullPath = rFp + "\\" + it->second->name;
                }
            }
        }
    }

    std::map<FileCategory, CategoryStats> localResults = results;
    std::vector<LargeFile> localLargeFiles;

    std::vector<MftRecord*> filesOnly;
    filesOnly.reserve(mftRes.records.size());
    for (auto& rec : mftRes.records) {
        if (!rec.isDir) {
            rec.size = -1;
            filesOnly.push_back(&rec);
        }
    }

    std::for_each(std::execution::par, filesOnly.begin(), filesOnly.end(), [&](MftRecord* rec) {
        if (shouldStop) return;

        auto pIt = dirMap.find(rec->parentRef);
        FolderNode* curr = (pIt != dirMap.end()) ? pIt->second.get() : root.get();

        std::wstring wFp(curr->fullPath.begin(), curr->fullPath.end());
        if (!wFp.empty() && wFp.back() == L'\\') wFp.pop_back();
        std::wstring wName(rec->name.begin(), rec->name.end());

        std::wstring wFullPath = L"\\\\?\\" + wFp + L"\\" + wName;

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(wFullPath.c_str(), GetFileExInfoStandard, &fad)) {
            rec->size = ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        }
        else {
            rec->size = -2;
        }
        });

    if (shouldStop) { progress = 1.0f; isScanning = false; return; }

    for (auto* rec : filesOnly) {
        if (shouldStop) break;
        if (rec->size < 0) continue;

        totalFilesScanned++;
        currentScannedSize += rec->size;

        std::string narrowName(rec->name.begin(), rec->name.end());
        auto pIt = dirMap.find(rec->parentRef);
        FolderNode* curr = (pIt != dirMap.end()) ? pIt->second.get() : root.get();

        std::string fp = curr->fullPath;
        if (!fp.empty() && fp.back() == '\\') fp.pop_back();
        std::string fullPathStr = fp + "\\" + narrowName;

        if (totalFilesScanned % 2000 == 0) {
            std::lock_guard<std::mutex> lock(pathMutex);
            currentScanPath = fullPathStr;
        }

        std::string ext = "";
        size_t dotPos = narrowName.find_last_of('.');
        if (dotPos != std::string::npos) {
            ext = narrowName.substr(dotPos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }

        FileCategory cat = CategorizeFile(ext);
        localResults[cat].totalSize += rec->size;
        localResults[cat].fileCount++;

        if (rec->size > 50ULL * 1024 * 1024) {
            localLargeFiles.push_back({ fullPathStr, (uintmax_t)rec->size, cat });
        }

 
        while (curr) {
            curr->totalSize += rec->size;
            curr->fileCount++;
            curr->categorySizes[static_cast<int>(cat)] += rec->size;

            if (!ext.empty()) {
                curr->extSizes[ext] += rec->size;
                if (curr->dominantExtension.empty() || curr->extSizes[ext] > curr->extSizes[curr->dominantExtension]) {
                    curr->dominantExtension = ext;
                }
            }
            curr = curr->parent;
        }
    }

    std::sort(localLargeFiles.begin(), localLargeFiles.end(),
        [](const LargeFile& a, const LargeFile& b) { return a.size > b.size; });
    if (localLargeFiles.size() > 100) localLargeFiles.resize(100);

    {
        std::lock_guard<std::mutex> lock(resultsMutex);
        results = localResults;
        largeFiles = localLargeFiles;
        rootNode = root;
    }

    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentScanPath = shouldStop ? "Scan stopped." : "MFT Scan Complete! (Parallel Speed)";
    }

    progress = 1.0f;
    isScanning = false;
}

void DiskScanner::ScanWorkerClassic(const std::string& rootPath) {
    usedMft = false;
    auto options = fs::directory_options::skip_permission_denied;
    auto root = std::make_shared<FolderNode>();
    root->name = rootPath;
    root->fullPath = rootPath;

    std::map<std::string, std::shared_ptr<FolderNode>> folderMap;
    folderMap[rootPath] = root;

    std::map<FileCategory, CategoryStats> localResults = results;
    std::vector<LargeFile> localLargeFiles;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(rootPath, options)) {
            if (shouldStop) break;

            if (totalFilesScanned % 100 == 0) {
                std::lock_guard<std::mutex> lock(pathMutex);
                currentScanPath = entry.path().string();
            }

            if (fs::is_directory(entry.status())) {
                std::string dirPath = entry.path().string();
                if (folderMap.find(dirPath) == folderMap.end()) {
                    auto node = std::make_shared<FolderNode>();
                    node->name = entry.path().filename().string();
                    node->fullPath = dirPath;

                    std::string parentPath = entry.path().parent_path().string();
                    auto parentIt = folderMap.find(parentPath);
                    if (parentIt != folderMap.end()) {
                        node->parent = parentIt->second.get();
                        parentIt->second->children.push_back(node);
                    }
                    folderMap[dirPath] = node;
                }
                continue;
            }

            if (!fs::is_regular_file(entry.status())) continue;

            uintmax_t fileSize = 0;
            try { fileSize = fs::file_size(entry.path()); }
            catch (...) { continue; }

            currentScannedSize += fileSize;

            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            FileCategory cat = CategorizeFile(ext);
            localResults[cat].totalSize += fileSize;
            localResults[cat].fileCount += 1;
            totalFilesScanned++;

            std::string dirPath = entry.path().parent_path().string();
            auto folderIt = folderMap.find(dirPath);
            while (folderIt != folderMap.end()) {
                folderIt->second->totalSize += fileSize;
                folderIt->second->fileCount++;
                folderIt->second->categorySizes[static_cast<int>(cat)] += fileSize;

                if (!ext.empty()) {
                    folderIt->second->extSizes[ext] += fileSize;
                    if (folderIt->second->dominantExtension.empty() ||
                        folderIt->second->extSizes[ext] > folderIt->second->extSizes[folderIt->second->dominantExtension]) {
                        folderIt->second->dominantExtension = ext;
                    }
                }

                if (folderIt->second->parent) {
                    dirPath = folderIt->second->parent->fullPath;
                    folderIt = folderMap.find(dirPath);
                }
                else break;
            }

            if (fileSize > 50ULL * 1024 * 1024)
                localLargeFiles.push_back({ entry.path().string(), fileSize, cat });
        }
    }
    catch (...) {}

    std::sort(localLargeFiles.begin(), localLargeFiles.end(),
        [](const LargeFile& a, const LargeFile& b) { return a.size > b.size; });
    if (localLargeFiles.size() > 100) localLargeFiles.resize(100);

    {
        std::lock_guard<std::mutex> lock(resultsMutex);
        results = localResults;
        largeFiles = localLargeFiles;
        rootNode = root;
    }

    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentScanPath = shouldStop ? "Scan stopped." : "Scan complete!";
    }

    progress = 1.0f;
    isScanning = false;
}

FileCategory DiskScanner::CategorizeFile(const std::string& extension) {
    if (extension == ".exe" || extension == ".dll" || extension == ".msi" ||
        extension == ".sys" || extension == ".com" || extension == ".bat")
        return FileCategory::Application;

    if (extension == ".mp4" || extension == ".mkv" || extension == ".avi" ||
        extension == ".mov" || extension == ".wmv" || extension == ".png" ||
        extension == ".jpg" || extension == ".jpeg" || extension == ".gif" ||
        extension == ".bmp" || extension == ".webp" || extension == ".mp3" ||
        extension == ".flac" || extension == ".wav" || extension == ".aac")
        return FileCategory::Media;

    if (extension == ".pdf" || extension == ".docx" || extension == ".doc" ||
        extension == ".txt" || extension == ".xlsx" || extension == ".xls" ||
        extension == ".pptx" || extension == ".csv" || extension == ".odt")
        return FileCategory::Document;

    if (extension == ".tmp" || extension == ".log" || extension == ".bak" ||
        extension == ".old" || extension == ".cache" || extension == ".dmp" ||
        extension == ".etl" || extension == ".temp")
        return FileCategory::Junk;

    return FileCategory::Other;
}