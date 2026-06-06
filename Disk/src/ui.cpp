#include "ui.h"
#include "imgui.h"
#include <filesystem>
#include <algorithm>
#include <cstring>
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::string FormatSize(uintmax_t bytes) {
    char buf[64];
    if (bytes >= 1ULL << 30)
        snprintf(buf, sizeof(buf), "%.2f GB", (double)bytes / (1ULL << 30));
    else if (bytes >= 1ULL << 20)
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1ULL << 20));
    else if (bytes >= 1ULL << 10)
        snprintf(buf, sizeof(buf), "%.0f KB", (double)bytes / (1ULL << 10));
    else
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    return std::string(buf);
}

static ImVec4 CategoryColor(FileCategory cat) {
    switch (cat) {
    case FileCategory::Application: return ImVec4(0.35f, 0.55f, 0.90f, 1.0f);
    case FileCategory::Media:       return ImVec4(0.25f, 0.75f, 0.55f, 1.0f);
    case FileCategory::Document:    return ImVec4(0.95f, 0.75f, 0.25f, 1.0f);
    case FileCategory::Junk:        return ImVec4(0.90f, 0.35f, 0.30f, 1.0f);
    default:                        return ImVec4(0.60f, 0.60f, 0.65f, 1.0f);
    }
}

// Maps a file extension to a deterministic color.
// Well-known extensions get hand-picked colors; unknowns use FNV-1a hash.
static ImU32 ExtensionColor(const std::string& ext, bool hovered) {
    struct KnownExt { const char* ext; uint8_t r, g, b; };
    static const KnownExt known[] = {
        { ".mp4",  50, 200,  90 }, { ".mkv",  40, 180,  80 }, { ".avi",  60, 190,  70 },
        { ".mp3", 100, 210, 100 }, { ".flac",  80, 200,  80 }, { ".wav",  70, 195,  85 },
        { ".jpg", 230, 160,  40 }, { ".jpeg", 230, 160,  40 }, { ".png", 220, 170,  50 },
        { ".gif", 200, 150,  60 }, { ".bmp",  210, 155,  55 }, { ".webp",215, 160,  45 },
        { ".exe",  80, 120, 230 }, { ".dll",   70, 110, 220 }, { ".msi",  60, 100, 210 },
        { ".pdf", 220,  70,  60 }, { ".docx",  60, 140, 230 }, { ".doc",  70, 130, 220 },
        { ".xlsx", 50, 180, 100 }, { ".pptx", 230, 100,  50 }, { ".txt", 180, 180, 180 },
        { ".zip", 190, 130,  60 }, { ".rar",  180, 120,  55 }, { ".7z",  200, 140,  65 },
        { ".tmp", 110, 110, 110 }, { ".log",  120, 120, 120 }, { ".bak", 100, 100, 100 },
    };
    for (const auto& k : known) {
        if (ext == k.ext) {
            if (hovered) return IM_COL32(
                std::min(255, (int)k.r + 60),
                std::min(255, (int)k.g + 60),
                std::min(255, (int)k.b + 60), 220);
            return IM_COL32(k.r, k.g, k.b, 180);
        }
    }
    // Unknown: deterministic hash
    uint32_t h = 2166136261u;
    for (char c : ext) h = (h ^ (uint8_t)c) * 16777619u;
    uint8_t r = 80 + (h & 0x7F);
    uint8_t g = 80 + ((h >> 8) & 0x7F);
    uint8_t b = 80 + ((h >> 16) & 0x7F);
    if (hovered) return IM_COL32(
        std::min(255, (int)r + 60), std::min(255, (int)g + 60), std::min(255, (int)b + 60), 220);
    return IM_COL32(r, g, b, 180);
}

// ─── RAM cleanup (your implementation) ───────────────────────────────────────

static void CleanSystemRam() {
    DWORD aProcesses[1024], cbNeeded, cProcesses;
    if (!EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded)) return;
    cProcesses = cbNeeded / sizeof(DWORD);
    for (unsigned int i = 0; i < cProcesses; i++) {
        if (aProcesses[i] != 0) {
            HANDLE hProcess = OpenProcess(
                PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, aProcesses[i]);
            if (hProcess != NULL) {
                EmptyWorkingSet(hProcess);
                CloseHandle(hProcess);
            }
        }
    }
}

// ─── Constructor ─────────────────────────────────────────────────────────────

DiskUI::DiskUI() {
    lastDiskRefresh = std::chrono::steady_clock::now() - std::chrono::seconds(10);
}

// ─── Disk info (cached, refreshes every 2 s) ─────────────────────────────────

void DiskUI::RefreshDiskInfo(const std::string& drivePath) {
    auto now = std::chrono::steady_clock::now();
    if (diskInfoValid &&
        std::chrono::duration_cast<std::chrono::seconds>(now - lastDiskRefresh).count() < 2)
        return;
    try {
        fs::space_info si = fs::space(drivePath);
        const double GB = 1024.0 * 1024.0 * 1024.0;
        diskInfo.totalDisk = (float)(si.capacity / GB);
        diskInfo.freeDisk = (float)(si.available / GB);
        diskInfo.usedDisk = diskInfo.totalDisk - diskInfo.freeDisk;
        diskInfo.usagePercent = (diskInfo.totalDisk > 0)
            ? (diskInfo.usedDisk / diskInfo.totalDisk) * 100.0f : 0.0f;
        diskInfoValid = true;
        lastDiskRefresh = now;
    }
    catch (...) {}
}

// ─── Main render ─────────────────────────────────────────────────────────────

void DiskUI::Render(GLFWwindow* window) {
    RefreshDiskInfo("C:\\");

    int wi, hi;
    glfwGetWindowSize(window, &wi, &hi);
    float fw = (float)wi, fh = (float)hi;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(fw, fh));
    ImGuiWindowFlags wflags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##main", nullptr, wflags);

    const float TOP_H = 50.0f;
    RenderTopBar(fw);
    ImGui::SetCursorPosY(TOP_H + 8.0f);

    float contentH = fh - TOP_H - 8.0f;
    switch (currentScreen) {
    case AppScreen::Overview:   RenderOverview(fw, contentH);   break;
    case AppScreen::FileTree:   RenderFileTree(fw, contentH);   break;
    case AppScreen::LargeFiles: RenderLargeFiles(fw, contentH); break;
    case AppScreen::Junk:       RenderJunk(fw, contentH);       break;
    case AppScreen::Ram:        RenderRam(fw, contentH);        break;
    }

    ImGui::End();
}

// ─── Top navigation bar ──────────────────────────────────────────────────────

void DiskUI::RenderTopBar(float windowWidth) {
    ImGui::SetCursorPos(ImVec2(12.0f, 14.0f));
    ImGui::Text("Disk Cleaner");

    ImGui::SameLine(windowWidth / 2.0f - 200.0f);

    struct NavItem { const char* label; AppScreen screen; };
    NavItem items[] = {
        { "Overview",    AppScreen::Overview   },
        { "Folders",     AppScreen::FileTree   },
        { "Large Files", AppScreen::LargeFiles },
        { "Cleanup",     AppScreen::Junk       },
        { "Ram",         AppScreen::Ram        },
    };
    for (int i = 0; i < 5; i++) {
        bool active = (currentScreen == items[i].screen);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.95f, 1.0f));
        if (ImGui::Button(items[i].label, ImVec2(100, 28)))
            currentScreen = items[i].screen;
        if (active) ImGui::PopStyleColor();
        if (i < 4) ImGui::SameLine(0, 6);
    }

    ImGui::Separator();
}

// ─── Overview ────────────────────────────────────────────────────────────────

void DiskUI::RenderOverview(float windowWidth, float windowHeight) {
    float leftW = windowWidth * 0.38f;
    float rightW = windowWidth - leftW - 16.0f;

    ImGui::BeginChild("##left", ImVec2(leftW, windowHeight - 8), true);

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "STORAGE STATUS");
    ImGui::Spacing();
    ImGui::Text("Total:     %.2f GB", diskInfo.totalDisk);
    ImGui::Text("Used:      %.2f GB", diskInfo.usedDisk);
    ImGui::Text("Free:      %.2f GB", diskInfo.freeDisk);
    ImGui::Spacing();

    char usageBuf[32];
    snprintf(usageBuf, sizeof(usageBuf), "%.1f%%", diskInfo.usagePercent);
    ImVec4 barColor = (diskInfo.usagePercent > 80)
        ? ImVec4(0.9f, 0.3f, 0.2f, 1.0f) : ImVec4(0.25f, 0.55f, 0.95f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
    ImGui::ProgressBar(diskInfo.usagePercent / 100.0f, ImVec2(-1, 20), usageBuf);
    ImGui::PopStyleColor();
    ImGui::Separator();

    if (scanner.IsScanning()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Scanning...");
        std::string path = scanner.GetCurrentScanPath();
        if (path.size() > 45) path = "..." + path.substr(path.size() - 42);
        ImGui::TextWrapped("%s", path.c_str());
        ImGui::Text("Scanned: %zu files", scanner.GetTotalFilesScanned());
        if (scanner.UsedMft())
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.5f, 1.0f), "[MFT - fast scan]");
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 12), "");
        if (ImGui::Button("Stop", ImVec2(-1, 28))) scanner.StopScan();
    }
    else {
        if (ImGui::Button("Scan C:\\ Drive", ImVec2(-1, 36)))
            scanner.StartFullScan("C:\\");

        const std::map<FileCategory, CategoryStats>& results = scanner.GetResults();
        bool hasData = false;
        for (std::map<FileCategory, CategoryStats>::const_iterator it = results.begin();
            it != results.end(); ++it)
            if (it->second.fileCount > 0) { hasData = true; break; }

        if (hasData) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "CATEGORY BREAKDOWN");
            ImGui::Spacing();
            for (std::map<FileCategory, CategoryStats>::const_iterator it = results.begin();
                it != results.end(); ++it) {
                FileCategory         cat = it->first;
                const CategoryStats& stats = it->second;
                if (stats.fileCount == 0) continue;
                float sizeGB = (float)((double)stats.totalSize / (1024.0 * 1024.0 * 1024.0));
                float ratio = (diskInfo.usedDisk > 0) ? (sizeGB / diskInfo.usedDisk) : 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                ImVec4 col = CategoryColor(cat);
                ImGui::TextColored(col, "●"); ImGui::SameLine();
                ImGui::Text("%s", stats.name.c_str());
                ImGui::Text("   %zu files - %s", stats.fileCount,
                    FormatSize(stats.totalSize).c_str());
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
                ImGui::ProgressBar(ratio, ImVec2(-1, 8), "");
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }
        }
    }
    ImGui::EndChild();

    // Right panel: treemap
    ImGui::SameLine(0, 8);
    ImGui::BeginChild("##right", ImVec2(rightW, windowHeight - 8), true);
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "FOLDER MAP");
    ImGui::Spacing();

    std::shared_ptr<FolderNode> root = scanner.GetRootNode();
    if (!scanner.IsScanning() && root && root->totalSize > 0) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float  mapW = rightW - 20.0f;
        float  mapH = windowHeight - 130.0f;

        LayoutTreemap(root.get(), pos.x, pos.y, mapW, mapH);
        ImVec2 mouse = ImGui::GetMousePos();
        DrawTreemap(root.get(), mouse.x, mouse.y);

        const FolderNode* hovered = FindHoveredNode(root.get(), mouse.x, mouse.y);
        if (hovered)
            ImGui::SetTooltip("%s\n%s (%zu files)\nDominant: %s",
                hovered->name.c_str(),
                FormatSize(hovered->totalSize).c_str(),
                hovered->fileCount,
                hovered->dominantExtension.empty() ? "--" : hovered->dominantExtension.c_str());

        ImGui::Dummy(ImVec2(mapW, mapH));

        // Extension color legend
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Color legend:");
        ImGui::SameLine();

        struct ExtEntry { std::string ext; uintmax_t size; };
        std::vector<ExtEntry> topExts;
        for (const auto& kv : root->extSizes)
            topExts.push_back({ kv.first, kv.second });
        std::sort(topExts.begin(), topExts.end(),
            [](const ExtEntry& a, const ExtEntry& b) { return a.size > b.size; });
        if (topExts.size() > 8) topExts.resize(8);

        for (size_t li = 0; li < topExts.size(); li++) {
            ImU32 col32 = ExtensionColor(topExts[li].ext, false);
            float lr = ((col32 >> 0) & 0xFF) / 255.0f;
            float lg = ((col32 >> 8) & 0xFF) / 255.0f;
            float lb = ((col32 >> 16) & 0xFF) / 255.0f;
            ImGui::TextColored(ImVec4(lr, lg, lb, 1.0f), "# %s", topExts[li].ext.c_str());
            if (li + 1 < topExts.size()) ImGui::SameLine(0, 12);
        }
    }
    else if (!scanner.IsScanning()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Run a scan to see the folder map.");
    }
    ImGui::EndChild();
}

// ─── Treemap layout ───────────────────────────────────────────────────────────

void DiskUI::LayoutTreemap(FolderNode* node, float x, float y, float w, float h) {
    if (!node || node->children.empty() || node->totalSize == 0) return;

    std::vector<std::shared_ptr<FolderNode>> children = node->children;
    std::sort(children.begin(), children.end(),
        [](const std::shared_ptr<FolderNode>& a, const std::shared_ptr<FolderNode>& b)
        { return a->totalSize > b->totalSize; });

    float cx = x, cy = y, cw = w, ch = h;
    for (size_t i = 0; i < children.size(); i++) {
        FolderNode* child = children[i].get();
        if (child->totalSize == 0) continue;
        float ratio = (float)((double)child->totalSize / (double)node->totalSize);
        if (ratio < 0.001f) ratio = 0.001f;
        if (ratio > 1.0f)   ratio = 1.0f;
        bool  horiz = (cw >= ch);
        float sw = horiz ? cw * ratio : cw;
        float sh = horiz ? ch : ch * ratio;
        child->treemapX = cx; child->treemapY = cy;
        child->treemapW = sw; child->treemapH = sh;
        if (!child->children.empty() && sw > 4 && sh > 4)
            LayoutTreemap(child, cx + 1, cy + 1, sw - 2, sh - 2);
        if (horiz) cx += sw; else cy += sh;
    }
}

// ─── Treemap draw (extension-based coloring) ─────────────────────────────────

void DiskUI::DrawTreemap(const FolderNode* node, float mouseX, float mouseY) {
    if (!node) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (size_t i = 0; i < node->children.size(); i++) {
        const FolderNode* child = node->children[i].get();
        if (child->treemapW < 2 || child->treemapH < 2) continue;

        bool hovered = (mouseX >= child->treemapX &&
            mouseX <= child->treemapX + child->treemapW &&
            mouseY >= child->treemapY &&
            mouseY <= child->treemapY + child->treemapH);

        ImU32 fill;
        if (!child->dominantExtension.empty())
            fill = ExtensionColor(child->dominantExtension, hovered);
        else
            fill = hovered ? IM_COL32(160, 160, 170, 220) : IM_COL32(80, 80, 90, 160);

        dl->AddRectFilled(
            ImVec2(child->treemapX, child->treemapY),
            ImVec2(child->treemapX + child->treemapW, child->treemapY + child->treemapH),
            fill, 2.0f);
        dl->AddRect(
            ImVec2(child->treemapX, child->treemapY),
            ImVec2(child->treemapX + child->treemapW, child->treemapY + child->treemapH),
            IM_COL32(15, 15, 20, 200), 2.0f, 0, 1.0f);

        if (child->treemapW > 60 && child->treemapH > 20) {
            std::string label = child->name;
            if (label.size() > 20) label = label.substr(0, 18) + "..";
            dl->AddText(ImVec2(child->treemapX + 4, child->treemapY + 4),
                IM_COL32(240, 240, 245, 230), label.c_str());
            if (child->treemapH > 36) {
                std::string info = FormatSize(child->totalSize);
                if (!child->dominantExtension.empty())
                    info += "  " + child->dominantExtension;
                dl->AddText(ImVec2(child->treemapX + 4, child->treemapY + 18),
                    IM_COL32(200, 200, 210, 200), info.c_str());
            }
        }

        if (!child->children.empty())
            DrawTreemap(child, mouseX, mouseY);
    }
}

const FolderNode* DiskUI::FindHoveredNode(const FolderNode* node, float mx, float my) {
    if (!node) return nullptr;
    for (size_t i = 0; i < node->children.size(); i++) {
        const FolderNode* child = node->children[i].get();
        if (mx >= child->treemapX && mx <= child->treemapX + child->treemapW &&
            my >= child->treemapY && my <= child->treemapY + child->treemapH) {
            const FolderNode* deeper = FindHoveredNode(child, mx, my);
            return deeper ? deeper : child;
        }
    }
    return nullptr;
}

// ─── Folder tree ─────────────────────────────────────────────────────────────

static void RenderFolderTreeNode(FolderNode* node, FolderNode*& selected) {
    if (!node) return;
    ImGuiTreeNodeFlags tflags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node == selected)       tflags |= ImGuiTreeNodeFlags_Selected;
    if (node->children.empty()) tflags |= ImGuiTreeNodeFlags_Leaf;
    bool open = ImGui::TreeNodeEx(node->name.c_str(), tflags);
    if (ImGui::IsItemClicked()) selected = node;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), "%s",
        FormatSize(node->totalSize).c_str());
    if (open) {
        std::vector<std::shared_ptr<FolderNode>> sorted = node->children;
        std::sort(sorted.begin(), sorted.end(),
            [](const std::shared_ptr<FolderNode>& a, const std::shared_ptr<FolderNode>& b)
            { return a->totalSize > b->totalSize; });
        for (size_t i = 0; i < sorted.size(); i++)
            RenderFolderTreeNode(sorted[i].get(), selected);
        ImGui::TreePop();
    }
}

void DiskUI::RenderFileTree(float windowWidth, float windowHeight) {
    std::shared_ptr<FolderNode> root = scanner.GetRootNode();
    if (!root || root->totalSize == 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Start a scan from the Overview tab first.");
        return;
    }
    float treeW = windowWidth * 0.5f - 8.0f;
    ImGui::BeginChild("##tree", ImVec2(treeW, windowHeight - 8), true);
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "FOLDER TREE");
    ImGui::Spacing();
    std::vector<std::shared_ptr<FolderNode>> sorted = root->children;
    std::sort(sorted.begin(), sorted.end(),
        [](const std::shared_ptr<FolderNode>& a, const std::shared_ptr<FolderNode>& b)
        { return a->totalSize > b->totalSize; });
    for (size_t i = 0; i < sorted.size(); i++)
        RenderFolderTreeNode(sorted[i].get(), selectedFolder);
    ImGui::EndChild();

    ImGui::SameLine(0, 8);
    ImGui::BeginChild("##detail", ImVec2(0, windowHeight - 8), true);
    if (selectedFolder) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "FOLDER DETAILS");
        ImGui::Spacing();
        ImGui::Text("Path:        %s", selectedFolder->fullPath.c_str());
        ImGui::Text("Size:        %s", FormatSize(selectedFolder->totalSize).c_str());
        ImGui::Text("Files:       %zu", selectedFolder->fileCount);
        ImGui::Text("Subfolders:  %zu", selectedFolder->children.size());
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "LARGEST SUBFOLDERS");
        ImGui::Spacing();
        std::vector<std::shared_ptr<FolderNode>> sub = selectedFolder->children;
        std::sort(sub.begin(), sub.end(),
            [](const std::shared_ptr<FolderNode>& a, const std::shared_ptr<FolderNode>& b)
            { return a->totalSize > b->totalSize; });
        if (sub.size() > 20) sub.resize(20);
        for (size_t i = 0; i < sub.size(); i++) {
            float ratio = (selectedFolder->totalSize > 0)
                ? (float)((double)sub[i]->totalSize / (double)selectedFolder->totalSize) : 0.0f;
            ImGui::Text("%s", sub[i]->name.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
            ImGui::Text("%s", FormatSize(sub[i]->totalSize).c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.25f, 0.55f, 0.95f, 0.8f));
            ImGui::ProgressBar(ratio, ImVec2(-1, 6), "");
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        if (ImGui::Button("Open in Explorer", ImVec2(160, 28))) {
            std::wstring wpath(selectedFolder->fullPath.begin(), selectedFolder->fullPath.end());
            ShellExecuteW(nullptr, L"explore", wpath.c_str(), nullptr, nullptr, SW_SHOWDEFAULT);
        }
    }
    else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Select a folder from the tree to see details.");
    }
    ImGui::EndChild();
}

// ─── Large files ─────────────────────────────────────────────────────────────

void DiskUI::RenderLargeFiles(float windowWidth, float windowHeight) {
    const std::vector<LargeFile>& largeFiles = scanner.GetLargeFiles();
    if (largeFiles.empty() && !scanner.IsScanning()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Run a scan first (files 50 MB+ will be listed here).");
        return;
    }
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        "LARGE FILES (%zu found, 50 MB+)", largeFiles.size());
    ImGui::Spacing();
    ImGui::BeginChild("##large", ImVec2(0, windowHeight - 50), true);
    ImGui::Columns(3, "largecols");
    ImGui::SetColumnWidth(0, windowWidth - 260.0f);
    ImGui::SetColumnWidth(1, 110.0f);
    ImGui::SetColumnWidth(2, 110.0f);
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "FILE PATH");  ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "SIZE");       ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "CATEGORY");   ImGui::NextColumn();
    ImGui::Separator();
    for (size_t i = 0; i < largeFiles.size(); i++) {
        const LargeFile& f = largeFiles[i];
        std::string display = f.path;
        if (display.size() > 70) display = "..." + display.substr(display.size() - 67);
        ImGui::PushID((int)i);
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "%s", display.c_str());
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            std::wstring wpath(f.path.begin(), f.path.end());
            std::wstring args = std::wstring(L"/select,\"") + wpath + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWDEFAULT);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Double-click to show in Explorer\n%s", f.path.c_str());
        ImGui::PopID();
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", FormatSize(f.size).c_str());
        ImGui::NextColumn();
        const char* catName = "Other";
        switch (f.category) {
        case FileCategory::Application: catName = "Application"; break;
        case FileCategory::Media:       catName = "Media";       break;
        case FileCategory::Document:    catName = "Document";    break;
        case FileCategory::Junk:        catName = "Junk";        break;
        default: break;
        }
        ImGui::TextColored(CategoryColor(f.category), "%s", catName);
        ImGui::NextColumn();
    }
    ImGui::Columns(1);
    ImGui::EndChild();
}

// ─── Junk cleaner ────────────────────────────────────────────────────────────

void DiskUI::RenderJunk(float windowWidth, float windowHeight) {
    struct JunkLocation {
        std::string label;
        std::string path;
        uintmax_t   size = 0;
        bool        calculated = false;
    };
    static std::vector<JunkLocation> locs;
    static std::vector<int>          sel;
    static bool                      sizesReady = false;

    if (locs.empty()) {
        const char* windir = getenv("WINDIR");
        const char* tmp = getenv("TEMP");
        locs.push_back({ "Windows Temp Folder",
            std::string(windir ? windir : "C:\\Windows") + "\\Temp", 0, false });
        locs.push_back({ "User Temp Folder",
            std::string(tmp ? tmp : "C:\\Temp"), 0, false });
        locs.push_back({ "Prefetch Cache",    "C:\\Windows\\Prefetch", 0, false });
        locs.push_back({ "Windows Update Downloads",
            "C:\\Windows\\SoftwareDistribution\\Download", 0, false });
        sel.assign(locs.size(), 0);
    }

    if (!sizesReady) {
        for (size_t i = 0; i < locs.size(); i++) {
            if (locs[i].calculated) continue;
            try {
                for (const auto& e : fs::recursive_directory_iterator(locs[i].path,
                    fs::directory_options::skip_permission_denied)) {
                    if (fs::is_regular_file(e.status()))
                        try { locs[i].size += fs::file_size(e.path()); }
                    catch (...) {}
                }
            }
            catch (...) {}
            locs[i].calculated = true;
        }
        sizesReady = true;
    }

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "JUNK FILE CLEANER");
    ImGui::Spacing();
    ImGui::TextWrapped("The locations below can be safely cleaned. Review your selections before deleting.");
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    uintmax_t selectedTotal = 0;
    ImGui::BeginChild("##junk", ImVec2(0, windowHeight - 110), false);
    for (size_t i = 0; i < locs.size(); i++) {
        ImGui::PushID((int)i);
        bool chk = (sel[i] != 0);
        if (ImGui::Checkbox("##sel", &chk)) sel[i] = chk ? 1 : 0;
        ImGui::PopID();
        ImGui::SameLine();
        if (sel[i]) selectedTotal += locs[i].size;
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "%s", locs[i].label.c_str());
        ImGui::SameLine(windowWidth - 200.0f);
        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.3f, 1.0f), "%s", FormatSize(locs[i].size).c_str());
        ImGui::Text("   %s", locs[i].path.c_str());
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::Separator(); ImGui::Spacing();
    ImGui::Text("Space to free: %s", FormatSize(selectedTotal).c_str());
    ImGui::SameLine();

    bool anySelected = false;
    for (size_t i = 0; i < sel.size(); i++) if (sel[i]) { anySelected = true; break; }
    if (!anySelected) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.15f, 1.0f));
    if (ImGui::Button("Clean Selected", ImVec2(160, 28))) confirmDeleteOpen = true;
    ImGui::PopStyleColor();
    if (!anySelected) ImGui::EndDisabled();

    if (confirmDeleteOpen) { ImGui::OpenPopup("Confirm"); confirmDeleteOpen = false; }
    if (ImGui::BeginPopupModal("Confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("All files in the selected folders will be permanently deleted!");
        ImGui::Text("Total: %s  -  Are you sure?", FormatSize(selectedTotal).c_str());
        ImGui::Spacing();
        if (ImGui::Button("Yes, Delete", ImVec2(120, 0))) {
            for (size_t i = 0; i < locs.size(); i++) {
                if (!sel[i]) continue;
                try {
                    for (const auto& e : fs::directory_iterator(locs[i].path,
                        fs::directory_options::skip_permission_denied))
                        try { fs::remove_all(e.path()); }
                    catch (...) {}
                    locs[i].size = 0; sel[i] = 0;
                }
                catch (...) {}
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ─── RAM screen (your implementation, unchanged) ─────────────────────────────

void DiskUI::RenderRam(float windowWidth, float windowHeight) {
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);

    unsigned long long totalRAM = 0;
    unsigned long long emptyRam = 0;
    DWORD              usagePercentage = 0;

    if (GlobalMemoryStatusEx(&memStatus)) {
        totalRAM = memStatus.ullTotalPhys / (1024 * 1024 * 1024);
        emptyRam = memStatus.ullAvailPhys / (1024 * 1024 * 1024);
        usagePercentage = memStatus.dwMemoryLoad;
    }

    unsigned long long usedRam = totalRAM - emptyRam;

    static float animatedUsage = 0.0f;
    animatedUsage += (usagePercentage - animatedUsage) * 0.03f;

    ImGui::BeginGroup();

    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "RAM INFORMATION");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SetWindowFontScale(1.2f);
    ImGui::Text("Total RAM: %llu GB", totalRAM);
    ImGui::Text("Used RAM:  %llu GB", usedRam);
    ImGui::Text("Free RAM:  %llu GB", emptyRam);
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Memory Usage: %lu%%", usagePercentage);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
    if (ImGui::Button("Clean Ram", ImVec2(150, 40)))
        CleanSystemRam();
    ImGui::PopStyleVar();

    ImGui::EndGroup();
    ImGui::SameLine(0.0f, 50.0f);

    // Animated fluid tank
    ImGui::BeginGroup();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 cursorP = ImGui::GetCursorScreenPos();

    float tankWidth = 60.0f;
    float tankHeight = 160.0f;
    float fillHeight = tankHeight * (animatedUsage / 100.0f);

    drawList->AddRectFilled(cursorP,
        ImVec2(cursorP.x + tankWidth, cursorP.y + tankHeight),
        IM_COL32(40, 40, 40, 255), 5.0f);

    ImVec2 fillStart = ImVec2(cursorP.x, cursorP.y + tankHeight - fillHeight);
    ImVec2 fillEnd = ImVec2(cursorP.x + tankWidth, cursorP.y + tankHeight);

    ImU32 fluidColor = (animatedUsage > 85.0f)
        ? IM_COL32(220, 50, 50, 255)
        : IM_COL32(30, 144, 255, 255);

    drawList->AddRectFilled(fillStart, fillEnd, fluidColor, 5.0f);
    drawList->AddRect(cursorP,
        ImVec2(cursorP.x + tankWidth, cursorP.y + tankHeight),
        IM_COL32(200, 200, 200, 255), 5.0f, 0, 2.0f);

    ImGui::Dummy(ImVec2(tankWidth, tankHeight));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
    ImGui::Text("%.1f%%", animatedUsage);
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 50.0f);

    // Info text
    ImGui::BeginGroup();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "RAM Optimization and Its Effects");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "RAM cleaning utilities use the Windows API (EmptyWorkingSet) to force background "
        "applications to surrender their memory, instantly shifting their idle data out of "
        "physical RAM and into the Pagefile (virtual memory on your SSD/HDD).\n\n"
        "While this process causes absolutely no physical or hardware damage to your computer "
        "components, it can introduce temporary system performance drawbacks. Forcing data to "
        "move from ultra-fast RAM to a much slower storage drive can cause micro-stutters, "
        "brief application freezes, and increased CPU usage when those background programs "
        "are refocused.\n\n"
        "Therefore, it is best used as a manual \"emergency button\" after closing heavy "
        "software, rather than an automated loop."
    );
    ImGui::Spacing();
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Fun Fact:");
    ImGui::SetWindowFontScale(1.1f);
    ImGui::TextWrapped("Windows already performs RAM cleanup; we just trigger it to do so");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndGroup();
}