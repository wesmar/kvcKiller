// ListViewManager.h
// ListView initialization, data refresh, row rendering and sorting,
// custom draw, and process-row data structures.
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>
#include "KvcContainers.h"

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/// One row displayed in the ListView.
struct UiProcessRow {
    WString canonicalName;                          // display-clean name, no .exe
    WString normalizedKey;                          // lower-case, no .exe
    WString displayName;                            // text shown in column 0
    DWORD   pid            = 0;
    bool    running        = false;
    bool    extra          = false;                 // not in config — live extra
    bool    predefined     = false;                 // comes from config
    bool    checked        = false;                 // checkbox state
    int     predefinedIndex = 0x7FFFFFFF;
    WString lastAction     = L"-";
};

/// Scroll / selection position saved before and restored after a list rebuild.
struct ListViewState {
    WString topKey;       // normalizedKey of the first visible row
    WString selectedKey;  // normalizedKey of the selected row
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Inserts all ListView columns and sets extended styles.
/// Must be called once after the ListView control is created.
void InitListView();

/// Rebuilds the ListView from the current config and running-process snapshot.
/// Thread-safe to call repeatedly (auto-refresh timer, WMU_REFRESH_LIST).
void RefreshProcessList();

/// Returns canonical names of all checked rows.
Vector<WString> GetSelectedProcesses();

/// Handles NM_CLICK and toggles the checkbox when the user clicks anywhere
/// on a row outside the state icon itself.
bool HandleListRowClick();

/// Handles NM_CUSTOMDRAW to colour rows by status.
LRESULT HandleListCustomDraw(LPARAM lParam);
