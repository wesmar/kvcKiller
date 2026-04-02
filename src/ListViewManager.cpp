// ListViewManager.cpp
// © WESMAR Marek Wesołowski
//
// Owns the ListView control lifecycle: column setup, process enumeration,
// row diffing/sorting, checkbox state, and custom-draw coloring.
// All Win32 handles used here (hListView, hShowExtraCheck, etc.) are declared
// in GlobalData.h and set up by WM_CREATE in main.cpp.

#include "ListViewManager.h"
#include "GlobalData.h"
#include "Utils.h"
#include "ProcessKiller.h"
#include "ConfigReader.h"
#include "Resource.h"
#include "UIHelpers.h"
#include "ProcessOperations.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <map>
#include <set>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

// ---------------------------------------------------------------------------
// Module-private state
// ---------------------------------------------------------------------------

// Mirrors the rows currently visible in the ListView, in display order.
// Kept in sync by ApplyRowsToListView; consumed by GetSelectedProcesses
// and HandleListRowClick to avoid repeated ListView_GetItemText calls.
static std::vector<UiProcessRow> g_visibleRows;

// ---------------------------------------------------------------------------
// Internal: process enumeration helpers
// ---------------------------------------------------------------------------

// Returns true when the "Show extra processes" checkbox is ticked.
static bool IsExtraViewEnabled() {
    return hShowExtraCheck &&
        SendMessageW(hShowExtraCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// Returns true when "Hide inactive built-in" is ticked.
static bool IsInactiveBuiltInHidden() {
    return hHideInactiveBuiltInCheck &&
        SendMessageW(hHideInactiveBuiltInCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// Strips the directory prefix from a full path and returns the bare filename
// without the .exe extension (used as the display name for extra/live rows).
static std::wstring ExtractDisplayNameFromPath(const std::wstring& fullPath) {
    const size_t slash = fullPath.find_last_of(L"\\/");
    const std::wstring fileName = (slash == std::wstring::npos) ? fullPath : fullPath.substr(slash + 1);
    return StripExeExtension(fileName);
}

// Returns true if a process has a Debugger set in IFEO,
// meaning the process was previously "paralyzed" and not yet restored.
static bool HasParalyzedBackup(const std::wstring& processName) {
    return IsProcessParalyzed(processName);
}

// Snapshots every running process via TH32CS_SNAPPROCESS and returns a map
// keyed by normalized name (lowercase, no extension).  Only the first entry
// per key is kept — if multiple instances exist, the lowest-PID one wins
// because Process32First/Next enumerates in ascending PID order.
static std::map<std::wstring, ProcessInfo> EnumerateAllRunningProcesses() {
    std::map<std::wstring, ProcessInfo> running;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return running;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            std::wstring displayName = StripExeExtension(pe.szExeFile);
            std::wstring key         = NormalizeProcessKey(displayName);
            if (key.empty() || running.contains(key)) continue;
            running[key] = ProcessInfo{ pe.th32ProcessID, displayName, true };
        } while (Process32NextW(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    return running;
}

// ---------------------------------------------------------------------------
// Internal: row flag / text helpers
// ---------------------------------------------------------------------------

// Encodes per-row display state into the LVITEM.lParam field so that
// NM_CUSTOMDRAW can color rows without another map lookup per paint cycle.
static LPARAM BuildRowFlags(const UiProcessRow& row) {
    LPARAM flags = 0;
    if (row.running)                           flags |= kRowFlagRunning;
    if (row.extra)                             flags |= kRowFlagExtra;
    if (row.lastAction == kActionKilled)       flags |= kRowFlagKilled;
    if (row.lastAction == kActionParalyzed)    flags |= kRowFlagParalyzed;
    if (row.lastAction == kActionRestored)     flags |= kRowFlagRestored;
    if (row.lastAction == kActionRestarted)    flags |= kRowFlagRestarted;
    return flags;
}

// Inserts a brand-new row at rowIndex.  Sets all five sub-items in one pass.
// INDEXTOSTATEIMAGEMASK(1) = unchecked, INDEXTOSTATEIMAGEMASK(2) = checked.
static void InsertProcessRow(int rowIndex, const UiProcessRow& row) {
    LVITEMW item{};
    item.mask      = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
    item.iItem     = rowIndex;
    item.pszText   = const_cast<LPWSTR>(row.displayName.c_str());
    item.lParam    = BuildRowFlags(row);
    item.stateMask = LVIS_STATEIMAGEMASK;
    item.state     = INDEXTOSTATEIMAGEMASK(row.checked ? 2 : 1);
    ListView_InsertItem(hListView, &item);

    std::wstring pidText    = FormatPid(row.pid);
    std::wstring statusText = row.running ? kStatusRunning : kStatusNotRunning;
    const wchar_t* srcText  = row.predefined ? kSourceReg : (row.extra ? kActionLive : kSourceSaved);

    ListView_SetItemText(hListView, rowIndex, 1, const_cast<LPWSTR>(srcText));
    ListView_SetItemText(hListView, rowIndex, 2, const_cast<LPWSTR>(pidText.c_str()));
    ListView_SetItemText(hListView, rowIndex, 3, const_cast<LPWSTR>(statusText.c_str()));
    ListView_SetItemText(hListView, rowIndex, 4, const_cast<LPWSTR>(row.lastAction.c_str()));
}

// Updates an existing row in-place without changing its index.
// Used by ApplyRowsToListView for rows that already exist in the control —
// avoids the flicker of delete + re-insert.
static void UpdateProcessRow(int rowIndex, const UiProcessRow& row) {
    LVITEMW item{};
    item.mask      = LVIF_PARAM | LVIF_STATE;
    item.iItem     = rowIndex;
    item.lParam    = BuildRowFlags(row);
    item.stateMask = LVIS_STATEIMAGEMASK;
    item.state     = INDEXTOSTATEIMAGEMASK(row.checked ? 2 : 1);
    ListView_SetItem(hListView, &item);

    std::wstring pidText    = FormatPid(row.pid);
    std::wstring statusText = row.running ? kStatusRunning : kStatusNotRunning;

    ListView_SetItemText(hListView, rowIndex, 0, const_cast<LPWSTR>(row.displayName.c_str()));
    ListView_SetItemText(hListView, rowIndex, 1,
        const_cast<LPWSTR>(row.predefined ? kSourceReg : (row.extra ? kActionLive : kSourceSaved)));
    ListView_SetItemText(hListView, rowIndex, 2, const_cast<LPWSTR>(pidText.c_str()));
    ListView_SetItemText(hListView, rowIndex, 3, const_cast<LPWSTR>(statusText.c_str()));
    ListView_SetItemText(hListView, rowIndex, 4, const_cast<LPWSTR>(row.lastAction.c_str()));
}

// ---------------------------------------------------------------------------
// Internal: checkbox-state preservation
// ---------------------------------------------------------------------------

// Walks the current ListView and records which items are checked, keyed by
// normalized process name.  Called before rebuilding the row list so that
// user selections survive a full refresh.
static std::map<std::wstring, bool> CaptureCheckedStates() {
    std::map<std::wstring, bool> checked;
    if (!hListView) return checked;

    const int itemCount = ListView_GetItemCount(hListView);
    for (int i = 0; i < itemCount; ++i) {
        std::wstring key;
        // Prefer the cached g_visibleRows entry; fall back to asking the
        // control itself (happens during the very first refresh).
        if (i < static_cast<int>(g_visibleRows.size())) {
            key = g_visibleRows[i].normalizedKey;
        } else {
            wchar_t buffer[260]{};
            ListView_GetItemText(hListView, i, 0, buffer, static_cast<int>(std::size(buffer)));
            key = NormalizeProcessKey(buffer);
        }
        checked[key] = ListView_GetCheckState(hListView, i) != FALSE;
    }
    return checked;
}

// ---------------------------------------------------------------------------
// Internal: sort comparators
// ---------------------------------------------------------------------------

// Case-insensitive wide-string comparison returning -1 / 0 / +1.
static int CompareInsensitive(const std::wstring& left, const std::wstring& right) {
    const int v = _wcsicmp(left.c_str(), right.c_str());
    return v < 0 ? -1 : v > 0 ? 1 : 0;
}

// Generic ascending comparison for any ordered type, returning -1 / 0 / +1.
template <typename T>
static int CompareAscending(T left, T right) {
    return left < right ? -1 : right < left ? 1 : 0;
}

// Natural (config-file) ordering: predefined rows sort by their config index;
// everything else sorts by name → PID → last action so the list is stable
// across refreshes when no explicit column sort is active.
static int CompareNaturalRowOrder(const UiProcessRow& left, const UiProcessRow& right) {
    if (left.predefined && right.predefined)
        return CompareAscending(left.predefinedIndex, right.predefinedIndex);

    int compare = CompareInsensitive(left.displayName, right.displayName);
    if (compare != 0) return compare;

    compare = CompareAscending(left.pid, right.pid);
    if (compare != 0) return compare;

    return CompareInsensitive(left.lastAction, right.lastAction);
}

// Column-header sort comparison.  Column indices match the ListView columns:
//   0 = Name, 1 = Source (predefined vs. not), 2 = PID,
//   3 = Status (running bool), 4 = Last Action.
static int CompareSortedColumn(const UiProcessRow& left, const UiProcessRow& right) {
    switch (g_sortColumn) {
    case 0: return CompareInsensitive(left.displayName, right.displayName);
    case 1: return CompareAscending(!left.predefined, !right.predefined);
    case 2: return CompareAscending(left.pid, right.pid);
    case 3: return (left.running != right.running) ? (left.running ? -1 : 1) : 0;
    case 4: return CompareInsensitive(left.lastAction, right.lastAction);
    default: return 0;
    }
}

// Full sort predicate used by std::stable_sort.
// Rule 1: predefined rows always sit above live-extra rows regardless of
//         the active column sort (keeps config targets visually grouped).
// Rule 2: within a tier, apply the column sort, then natural order as a
//         tiebreaker so the result is deterministic.
static bool IsRowLess(const UiProcessRow& left, const UiProcessRow& right) {
    // Predefined rows always come before live-extra rows
    if (left.predefined != right.predefined)
        return left.predefined && !right.predefined;

    int compare = 0;
    if (g_sortColumn != kSortColumnNatural)
        compare = CompareSortedColumn(left, right);
    if (compare == 0) compare = CompareNaturalRowOrder(left, right);
    if (compare == 0) compare = CompareInsensitive(left.normalizedKey, right.normalizedKey);
    if (compare == 0) compare = CompareAscending(left.pid, right.pid);
    if (compare == 0) compare = CompareInsensitive(left.lastAction, right.lastAction);
    if (compare == 0) return false;

    return g_sortAscending ? (compare < 0) : (compare > 0);
}

// ---------------------------------------------------------------------------
// Internal: scroll / selection state preservation
// ---------------------------------------------------------------------------

// Finds the first row whose normalizedKey matches. Returns -1 if not found.
static int FindRowIndexByKey(const std::vector<UiProcessRow>& rows, const std::wstring& key) {
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (rows[i].normalizedKey == key) return i;
    return -1;
}

// Saves the normalized keys of the top-visible and selected rows before
// a rebuild so we can scroll back to the same logical position afterward.
static ListViewState CaptureListViewState() {
    ListViewState state;
    if (!hListView || g_visibleRows.empty()) return state;

    const int topIndex = ListView_GetTopIndex(hListView);
    if (topIndex >= 0 && topIndex < static_cast<int>(g_visibleRows.size()))
        state.topKey = g_visibleRows[topIndex].normalizedKey;

    const int selectedIndex = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(g_visibleRows.size()))
        state.selectedKey = g_visibleRows[selectedIndex].normalizedKey;

    return state;
}

// Re-applies the previously captured scroll/selection state to the (possibly
// reordered) rows vector.  Uses ListView_Scroll to align the view to the
// correct pixel row since ListView_EnsureVisible only guarantees visibility,
// not the exact top-of-viewport position.
static void RestoreListViewState(const ListViewState& state,
                                 const std::vector<UiProcessRow>& rows) {
    if (!hListView) return;

    // Clear all selections first to avoid stale highlights.
    ListView_SetItemState(hListView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);

    if (!state.selectedKey.empty()) {
        const int idx = FindRowIndexByKey(rows, state.selectedKey);
        if (idx >= 0)
            ListView_SetItemState(hListView, idx,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
    }

    if (!state.topKey.empty()) {
        const int targetIndex = FindRowIndexByKey(rows, state.topKey);
        if (targetIndex >= 0) {
            ListView_EnsureVisible(hListView, targetIndex, FALSE);

            // EnsureVisible may have scrolled past the target; nudge it back
            // to the exact row using pixel-level scroll.
            RECT rowRect{};
            if (ListView_GetItemRect(hListView, 0, &rowRect, LVIR_BOUNDS)) {
                const int rowHeight  = rowRect.bottom - rowRect.top;
                const int currentTop = ListView_GetTopIndex(hListView);
                if (rowHeight > 0 && currentTop != targetIndex)
                    ListView_Scroll(hListView, 0, (targetIndex - currentTop) * rowHeight);
            }
        }
    }
}

// Efficiently diffs the current ListView contents against \p rows,
// updating / inserting / deleting items as needed, then restores focus.
// WM_SETREDRAW suppression eliminates the visible repaint during the diff.
static void ApplyRowsToListView(const std::vector<UiProcessRow>& rows) {
    const ListViewState state = CaptureListViewState();
    SendMessageW(hListView, WM_SETREDRAW, FALSE, 0);

    const int currentCount = ListView_GetItemCount(hListView);
    // Rows that exist in both old and new lists → update in place (no flicker).
    const int sharedCount  = min(currentCount, static_cast<int>(rows.size()));

    for (int i = 0; i < sharedCount; ++i)
        UpdateProcessRow(i, rows[i]);

    // New rows beyond the old count → append.
    for (int i = sharedCount; i < static_cast<int>(rows.size()); ++i)
        InsertProcessRow(i, rows[i]);

    // Remove stale tail rows.  Always delete from the end to avoid index
    // shifting invalidating the loop counter.
    while (ListView_GetItemCount(hListView) > static_cast<int>(rows.size()))
        ListView_DeleteItem(hListView, ListView_GetItemCount(hListView) - 1);

    g_visibleRows = rows;
    RestoreListViewState(state, g_visibleRows);

    SendMessageW(hListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hListView, nullptr, TRUE);
}

// ===========================================================================
// Public API
// ===========================================================================

// Inserts all five ListView columns and applies extended styles + visual theme.
// Column widths set here are placeholders; LayoutMainWindow overrides them.
void InitListView() {
    LVCOLUMNW lvc = {};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    const std::wstring sColName       = LoadStr(IDS_COL_PROCESS_NAME);
    const std::wstring sColSource     = LoadStr(IDS_COL_SOURCE);
    const std::wstring sColPid        = LoadStr(IDS_COL_PID);
    const std::wstring sColStatus     = LoadStr(IDS_COL_STATUS);
    const std::wstring sColLastAction = LoadStr(IDS_COL_LAST_ACTION);

    // Widths here are placeholders; LayoutMainWindow overrides them on first paint.
    lvc.iSubItem = 0; lvc.pszText = const_cast<LPWSTR>(sColName.c_str());       lvc.cx = kColNameMinCx;    ListView_InsertColumn(hListView, 0, &lvc);
    lvc.iSubItem = 1; lvc.pszText = const_cast<LPWSTR>(sColSource.c_str());     lvc.cx = kColSourceCx;     ListView_InsertColumn(hListView, 1, &lvc);
    lvc.iSubItem = 2; lvc.pszText = const_cast<LPWSTR>(sColPid.c_str());        lvc.cx = kColPidCx;        ListView_InsertColumn(hListView, 2, &lvc);
    lvc.iSubItem = 3; lvc.pszText = const_cast<LPWSTR>(sColStatus.c_str());     lvc.cx = kColStatusCx;     ListView_InsertColumn(hListView, 3, &lvc);
    lvc.iSubItem = 4; lvc.pszText = const_cast<LPWSTR>(sColLastAction.c_str()); lvc.cx = kColLastActionCx; ListView_InsertColumn(hListView, 4, &lvc);

    // Full-row select + gridlines + per-row checkboxes + double-buffering to
    // suppress flicker during the NM_CUSTOMDRAW repaints.
    ListView_SetExtendedListViewStyle(
        hListView,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);

    SetWindowTheme(hListView, g_isDarkMode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

// Main refresh entry point, called on the 2-second timer and after any
// user action (kill / restore / checkbox toggle / column header click).
//
// Pipeline:
//   1. Read config → build target set.
//   2. Snapshot running processes via toolhelp.
//   3. Restore previously saved checkbox states.
//   4. Build UiProcessRow list: predefined targets first, then known-path
//      non-built-ins (with .bak or stored action), then optionally all live
//      processes as "extra" rows.
//   5. Compute action-label transitions (killed→restarted, restored→none, etc.).
//   6. Sort and diff-apply to the ListView.
void RefreshProcessList() {
    auto processNames = ConfigReader::ReadProcessList();
    const bool hideInactiveBuiltIn = IsInactiveBuiltInHidden();
    const auto knownPaths = ConfigReader::ReadKnownProcessPaths();
    const auto allRunning = EnumerateAllRunningProcesses();

    std::vector<std::wstring> exeNames;
    std::set<std::wstring> targetKeys;
    for (const auto& name : processNames) {
        exeNames.push_back(name + L".exe");
        targetKeys.insert(NormalizeProcessKey(name));
    }

    // Build a deduplicated running-targets map (first match wins per key).
    auto running = ProcessKiller::FindProcesses(exeNames);
    std::map<std::wstring, ProcessInfo> runningTargets;
    for (const auto& proc : running) {
        const std::wstring key = NormalizeProcessKey(proc.name);
        if (!runningTargets.contains(key)) runningTargets[key] = proc;
    }

    const auto checkedStates = CaptureCheckedStates();
    std::vector<UiProcessRow> rows;
    rows.reserve(processNames.size() + 64);
    std::set<std::wstring> visibleKeys; // tracks which keys already have a row

    int runningCount = 0;

    // --- Pass 1: predefined config targets ---
    for (int i = 0; i < static_cast<int>(processNames.size()); ++i) {
        const auto& name             = processNames[i];
        const std::wstring canonical = StripExeExtension(name);
        const std::wstring key       = NormalizeProcessKey(canonical);

        UiProcessRow row;
        row.canonicalName   = canonical;
        row.normalizedKey   = key;
        row.displayName     = canonical;
        row.predefined      = true;
        row.predefinedIndex = i;

        const auto checkedIt = checkedStates.find(key);
        row.checked = checkedIt != checkedStates.end() && checkedIt->second;

        const auto runningIt = runningTargets.find(key);
        row.running = runningIt != runningTargets.end();
        row.pid     = row.running ? runningIt->second.pid : 0;
        row.lastAction = ReadLastAction(name);

        // --- Action label state machine ---
        // The label must reflect what *just happened*, not what was stored,
        // so we compare the current running state against the previous tick.
        const bool wasRunning = WasRunningPreviously(name);
        if (row.running) {
            if (row.lastAction == kActionKilled || row.lastAction == kActionParalyzed) {
                // Was killed/paralyzed last tick, now running again → restarted.
                row.lastAction = kActionRestarted;
                StoreLastAction(name, row.lastAction);
            } else if (!wasRunning &&
                       row.lastAction != kActionRestored &&
                       row.lastAction != kActionLive) {
                // Wasn't running last tick, now is — something launched it.
                row.lastAction = kActionRestarted;
                StoreLastAction(name, row.lastAction);
            }
        } else {
            if (row.lastAction == kActionRestored && wasRunning) {
                // Was restored last tick, now stopped → clear the label.
                row.lastAction = kActionNone;
                StoreLastAction(name, row.lastAction);
            } else if (row.lastAction == kActionRestarted) {
                // Restarted but now stopped again → clear.
                row.lastAction = kActionNone;
                StoreLastAction(name, row.lastAction);
            } else if (row.lastAction != kActionKilled &&
                       row.lastAction != kActionParalyzed &&
                       row.lastAction != kActionRestored) {
                // No action recorded — initialise from IFEO so the state
                // survives across app restarts (covers the "cold open" case).
                if (HasParalyzedBackup(canonical)) {
                    row.lastAction = kActionParalyzed;
                } else {
                    row.lastAction = kActionNone;
                }
                StoreLastAction(name, row.lastAction);
            }
        }

        StoreRunningState(name, row.running);
        if (row.running) ++runningCount;

        // When "hide inactive built-in" is on, skip rows with no interesting state.
        const bool keepVisible = row.running || row.lastAction != kActionNone;
        if (hideInactiveBuiltIn && !keepVisible) {
            continue;
        }

        rows.push_back(row);
        visibleKeys.insert(key);
    }

    // --- Pass 2: non-built-in processes that the user previously interacted with ---
    // These come from ConfigReader::ReadKnownProcessPaths() (persisted after
    // a kill/restore).  Show them as long as a .bak backup exists or a stored
    // action label remains — they may need manual restoration.
    for (const auto& entry : knownPaths) {
        const std::wstring key = NormalizeProcessKey(entry.processName);
        if (key.empty() || targetKeys.contains(key) || visibleKeys.contains(key)) {
            continue;
        }

        const auto runningIt = allRunning.find(key);
        const bool isRunning = runningIt != allRunning.end();
        // Pass the processName (without .exe extension) to HasParalyzedBackup
        const bool backupExists = HasParalyzedBackup(entry.processName);

        UiProcessRow row;
        row.canonicalName = ExtractDisplayNameFromPath(entry.fullPath);
        if (row.canonicalName.empty()) {
            row.canonicalName = StripExeExtension(entry.processName);
        }
        row.normalizedKey = key;
        row.displayName   = row.canonicalName;
        row.pid           = isRunning ? runningIt->second.pid : 0;
        row.running       = isRunning;
        row.predefined    = false;
        row.extra         = false;

        const auto checkedIt = checkedStates.find(key);
        row.checked = checkedIt != checkedStates.end() && checkedIt->second;

        row.lastAction = ReadLastAction(entry.processName);
        // If a .bak backup exists but no action was recorded, infer "killed"
        // (covers the case where the app restarted since the last kill).
        if (backupExists && row.lastAction == kActionNone) {
            row.lastAction = kActionKilled;
        }

        // Same state-machine logic as for predefined rows above.
        const bool wasRunning = WasRunningPreviously(entry.processName);
        if (row.running) {
            if (row.lastAction == kActionKilled) {
                row.lastAction = kActionRestarted;
                StoreLastAction(entry.processName, row.lastAction);
            } else if (!wasRunning &&
                       row.lastAction != kActionRestored &&
                       row.lastAction != kActionLive) {
                row.lastAction = kActionRestarted;
                StoreLastAction(entry.processName, row.lastAction);
            }
        } else {
            if (row.lastAction == kActionRestored && wasRunning) {
                row.lastAction = kActionNone;
                StoreLastAction(entry.processName, row.lastAction);
            } else if (row.lastAction == kActionRestarted) {
                row.lastAction = kActionNone;
                StoreLastAction(entry.processName, row.lastAction);
            }
        }

        StoreRunningState(entry.processName, row.running);

        // Keep visible only if there's something actionable to show.
        const bool keepVisible = backupExists || row.lastAction != kActionNone;
        if (!keepVisible) {
            continue;
        }

        rows.push_back(row);
        visibleKeys.insert(key);
    }

    // --- Pass 3: optional "extra" rows — every other running process ---
    // Only appended when the "Show extra processes" checkbox is on.
    // These rows are purely informational; they carry kActionLive as label
    // and kRowFlagExtra for the blue tint in custom draw.
    int extraCount = 0;
    if (IsExtraViewEnabled()) {
        for (const auto& [key, proc] : allRunning) {
            if (targetKeys.contains(key) || visibleKeys.contains(key)) continue;

            UiProcessRow row;
            row.canonicalName = proc.name;
            row.normalizedKey = key;
            row.displayName   = proc.name;
            row.pid           = proc.pid;
            row.running       = true;
            row.extra         = true;
            row.predefined    = false;
            row.lastAction    = kActionLive;

            const auto checkedIt = checkedStates.find(key);
            row.checked = checkedIt != checkedStates.end() && checkedIt->second;

            rows.push_back(row);
            visibleKeys.insert(key);
            ++extraCount;
        }
    }

    std::stable_sort(rows.begin(), rows.end(), IsRowLess);
    ApplyRowsToListView(rows);
    UpdateProcessCount(static_cast<int>(processNames.size()), runningCount, extraCount);
}

// Returns the canonicalName of every checked row (respects the cached
// g_visibleRows so we don't have to round-trip through ListView_GetItemText).
std::vector<std::wstring> GetSelectedProcesses() {
    std::vector<std::wstring> selected;
    const int count = min(ListView_GetItemCount(hListView),
                          static_cast<int>(g_visibleRows.size()));
    for (int i = 0; i < count; ++i)
        if (ListView_GetCheckState(hListView, i))
            selected.push_back(g_visibleRows[i].canonicalName);
    return selected;
}

// Handles NM_CLICK on the ListView.  We intercept clicks ourselves instead of
// relying on LVS_EX_CHECKBOXES auto-toggle because we also need to keep
// g_visibleRows.checked in sync without an extra round-trip to the control.
// Returns true if a row item (non-checkbox area) was clicked and the
// checkbox was toggled; false otherwise (caller may do default processing).
bool HandleListRowClick() {
    if (!hListView) {
        return false;
    }

    DWORD pos = GetMessagePos();
    POINT pt{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
    ScreenToClient(hListView, &pt);

    LVHITTESTINFO hit{};
    hit.pt = pt;
    const int index = ListView_SubItemHitTest(hListView, &hit);
    if (index < 0) {
        return false;
    }

    // Only react to clicks on the item text / icon area, not the checkbox
    // image itself (LVHT_ONITEMSTATEICON) — the checkbox already toggles
    // natively for that zone.
    if ((hit.flags & LVHT_ONITEM) == 0 || (hit.flags & LVHT_ONITEMSTATEICON) != 0) {
        return false;
    }

    const bool checked = ListView_GetCheckState(hListView, index) != FALSE;
    ListView_SetCheckState(hListView, index, checked ? FALSE : TRUE);
    // Mirror the change into our cache immediately so GetSelectedProcesses
    // doesn't need to re-query the control on the next call.
    if (index < static_cast<int>(g_visibleRows.size())) {
        g_visibleRows[index].checked = !checked;
    }

    return true;
}

// NM_CUSTOMDRAW handler — applies per-row foreground/background colors based
// on the flags packed into NMCUSTOMDRAW.lItemlParam by BuildRowFlags().
//
// Dark and light palettes are defined inline; selected rows always use the
// system highlight colors regardless of their action state.
LRESULT HandleListCustomDraw(LPARAM lParam) {
    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);

    switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        // Ask for item-level notifications.
        return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT:
        // Ask for sub-item level notifications so we can color individual cells.
        return CDRF_NOTIFYSUBITEMDRAW;

    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        // Selected rows always use the system highlight colours
        if ((draw->nmcd.uItemState & CDIS_SELECTED) != 0) {
            draw->clrText   = g_isDarkMode ? RGB(255, 255, 255) : RGB(18, 32, 48);
            draw->clrTextBk = g_isDarkMode ? RGB(52, 84, 132) : RGB(196, 220, 252);
            return CDRF_DODEFAULT;
        }

        // Map row flags to theme-appropriate colors.
        // Priority: killed > restored > restarted > extra > running > default.
        const LPARAM flags = draw->nmcd.lItemlParam;
        if (g_isDarkMode) {
            if      (flags & kRowFlagParalyzed) { draw->clrText = RGB(255, 160,  80); draw->clrTextBk = RGB(80, 44, 16); }
            else if (flags & kRowFlagKilled)    { draw->clrText = RGB(255, 186, 186); draw->clrTextBk = RGB(74, 42, 42); }
            else if (flags & kRowFlagRestored)  { draw->clrText = RGB(170, 239, 184); draw->clrTextBk = RGB(35, 72, 47); }
            else if (flags & kRowFlagRestarted) { draw->clrText = RGB(255, 221, 164); draw->clrTextBk = RGB(84, 64, 30); }
            else if (flags & kRowFlagExtra)     { draw->clrText = RGB(173, 212, 255); draw->clrTextBk = RGB(34, 50, 76); }
            else if (flags & kRowFlagRunning)   { draw->clrText = RGB(167, 229, 181); draw->clrTextBk = RGB(33, 60, 44); }
            else                                { draw->clrText = RGB(214, 214, 214); draw->clrTextBk = RGB(40, 40, 40); }
        } else {
            if      (flags & kRowFlagParalyzed) { draw->clrText = RGB(160,  72,   0); draw->clrTextBk = RGB(255, 238, 210); }
            else if (flags & kRowFlagKilled)    { draw->clrText = RGB(122,  36,  36); draw->clrTextBk = RGB(252, 232, 232); }
            else if (flags & kRowFlagRestored)  { draw->clrText = RGB( 28,  92,  52); draw->clrTextBk = RGB(228, 246, 234); }
            else if (flags & kRowFlagRestarted) { draw->clrText = RGB(116,  78,  16); draw->clrTextBk = RGB(255, 244, 221); }
            else if (flags & kRowFlagExtra)     { draw->clrText = RGB( 35,  64, 112); draw->clrTextBk = RGB(232, 240, 252); }
            else if (flags & kRowFlagRunning)   { draw->clrText = RGB( 26,  88,  54); draw->clrTextBk = RGB(236, 248, 240); }
            else                                { draw->clrText = RGB( 92,  92,  92); draw->clrTextBk = RGB(247, 247, 249); }
        }
        return CDRF_DODEFAULT;
    }
    }

    return CDRF_DODEFAULT;
}
