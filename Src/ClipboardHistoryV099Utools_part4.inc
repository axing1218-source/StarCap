        RECT textRc{ tab.left + 34, tab.top, tab.right - 10, tab.bottom };
        drawText(dc, labels[index], textRc, fg, selected ? boldFont : uiFont,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    inline void v099DrawHeader(HDC dc, const RECT& client)
    {
        fillRect(dc, client, v099Canvas());

        RECT badge = v099HeaderBadgeRect();
        // Keep the clipboard identity card visually elevated in both themes. The
        // current StarCap logo renderer is vector/AA, so it no longer needs the old
        // hard-coded RGB(230,230,230) bitmap-compositing background.
        const COLORREF badgeBg = v099DarkMode() ? RGB(46, 48, 55) : RGB(230, 230, 230);
        fillRoundRect(dc, badge, badgeBg, 18);
        RECT logo{ badge.left + 8, badge.top + 4, badge.left + 40, badge.top + 36 };
        v099DrawAppLogo(dc, logo);

        RECT title{ badge.left + 47, badge.top + 1, badge.right - 31, badge.top + 21 };
        drawText(dc, L"剪贴板", title, v099Text(), uiFont,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT count{ badge.left + 47, badge.top + 20, badge.right - 31, badge.bottom - 2 };
        drawText(dc, std::format(L"{} 条记录", items.size()), count, v099Muted(), smallFont,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT closeRc = v099CloseRect(client);
        drawText(dc, L"×", closeRc, v099Muted(), uiFont,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT search = v099SearchOuterRect(client);
        fillRoundRect(dc, search, v099Canvas(), 8);

        for (int i = 0; i < 5; ++i) {
            RECT tab = v099TabRect(client, i);
            v099DrawTabVisual(dc, tab, i, (int)activeTab == i);
        }

        RECT line{ 0, V099_HEADER_H - 1, client.right, V099_HEADER_H };
        fillRect(dc, line, v099Border());
    }

    inline int v099TextLines(const Item& item, int width)
    {
        std::wstring text = textFromData(item.data);
        int explicitLines = 1;
        for (wchar_t c : text) if (c == L'\n') ++explicitLines;
        const int charsPerLine = std::max(26, (width - 90) / 11);
        const int wrapped = (int)((text.size() + (size_t)charsPerLine - 1) / (size_t)charsPerLine);
        return std::clamp(std::max(explicitLines, wrapped), 1, 4);
    }

    inline int v099ItemHeight(const Item& item)
    {
        RECT rc{};
        int width = 760;
        if (listWnd && GetClientRect(listWnd, &rc)) width = std::max(520, (int)rc.right);
        if (item.type == ItemType::Image) return 150;
        if (item.type == ItemType::File) {
            const int n = std::max(1, (int)filePaths(item.data).size());
            return 50 + std::min(n, 4) * 28;
        }
        return 50 + v099TextLines(item, width) * 22;
    }

    inline void v099DrawFileIcon(HDC dc, int x, int y, const std::filesystem::path& path)
    {
        RECT page{ x, y, x + 18, y + 22 };
        const auto ext = path.extension().wstring();
        if (_wcsicmp(ext.c_str(), L".pdf") == 0) {
            fillRoundRect(dc, page, RGB(232, 67, 82), 4);
            drawText(dc, L"P", page, RGB(255,255,255), smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        fillRoundRect(dc, page, v099Surface(), 3);
        strokeRoundRect(dc, page, v099Border(), 1, 3);
        RECT mark{ page.left + 2, page.top + 4, page.right - 2, page.bottom - 3 };
        drawText(dc, L"·", mark, v099Muted(), boldFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    inline void v099DrawFilePreview(HDC dc, const Item& item, RECT body)
    {
        auto paths = filePaths(item.data);
        if (paths.empty()) {
            drawText(dc, L"文件或文件夹", body, v099Text(), uiFont, DT_LEFT | DT_TOP | DT_SINGLELINE);
            return;
        }

        const int rowH = 28;
        const int count = std::min((int)paths.size(), 4);
        for (int i = 0; i < count; ++i) {
            std::filesystem::path p(paths[(size_t)i]);
            const int y = body.top + i * rowH;
            v099DrawFileIcon(dc, body.left, y + 1, p);
            RECT tr{ body.left + 28, y, body.right, y + rowH };
            auto name = p.filename().wstring();
            if (name.empty()) name = paths[(size_t)i];
            drawText(dc, name, tr, v099Text(), uiFont,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    inline std::wstring v099ItemTypeLabel(const Item& item)
    {
        if (item.type == ItemType::Image) return L"图像";
        if (item.type == ItemType::File) return L"文件";
        return L"文本";
    }

    inline std::wstring v099TimeLabel(uint64_t ticks)
    {
        auto relative = relativeTime(ticks);
        ULARGE_INTEGER value{};
        value.QuadPart = ticks;
        FILETIME ft{ value.LowPart, value.HighPart };
        SYSTEMTIME utc{}, local{};
        if (!FileTimeToSystemTime(&ft, &utc) ||
            !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return relative;
        return std::format(L"{}（{:04}-{:02}-{:02} {:02}:{:02}）", relative,
            local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    }

    inline void v099DrawBottomMeta(HDC dc, const Item& item, const RECT& panel, bool expandable)
    {
        RECT timeRc{ panel.left + 14, panel.bottom - 24,
            std::min(panel.left + 300, panel.right - 185), panel.bottom - 7 };
        drawText(dc, v099TimeLabel(item.updated), timeRc, v099Faint(), smallFont,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        std::wstring meta;
