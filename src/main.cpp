// Namer-C — native Windows port of Namer (github.com/ByTheSeaL/Namer).
//
// Single-window Win32 app: describe a thing on the left, pick a context and
// an OpenRouter model, and get name ideas in the list on the right. Supports
// result history, iterating on a liked name, and re-prompting.
//
// Threading: all OpenRouter calls run on detached worker threads and post
// their outcome back with PostMessage. Requests are soft-cancelled by
// bumping g_reqSeq — replies carrying a stale sequence number are discarded.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../res/resource.h"
#include "json.hpp"
#include "llm.hpp"
#include "prefs.hpp"
#include "util.hpp"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winhttp.lib")

constexpr char APP_VERSION[] = "0.1.0";

constexpr UINT WM_APP_MODELS  = WM_APP + 1;  // lParam: std::vector<std::string>*
constexpr UINT WM_APP_RESULTS = WM_APP + 2;  // lParam: ResultMsg*

constexpr char HINT_DEFAULT[] =
    "Double-click a result for similar names; right-click for more options.";

const char* CONTEXTS[] = {"Code", "Fiction", "Paper / Technical",
                          "Product / Project", "General"};
constexpr int NUM_CONTEXTS = 5;

struct HistEntry {
    std::vector<llm::NameIdea> rows;
    std::string status;  // e.g. "More like "Foo"" — shown when viewing this entry
};

struct ResultMsg {
    LONG seq;
    bool ok;
    std::vector<llm::NameIdea> rows;
    std::string status;
    std::string error;
};

// ---- globals ----------------------------------------------------------------

HINSTANCE g_hInst;
HWND g_hwnd;
HWND g_ctxRadios[NUM_CONTEXTS], g_desc, g_model, g_freeOnly;
HWND g_back, g_fwd, g_histLbl, g_list, g_status, g_generate;
HFONT g_font;

std::vector<std::string> g_models;
prefs::Prefs g_prefs;

std::vector<HistEntry> g_history;
int g_histPos = -1;
std::string g_lastDescription;

volatile LONG g_reqSeq = 0;
bool g_inFlight = false;
bool g_statusIsError = false;
int g_comboSepIndex = -1;

// ---- small helpers ----------------------------------------------------------

// Run fn on a detached native thread. CreateThread (rather than std::thread)
// keeps us off the winpthreads runtime that some MinGW builds require, and
// behaves identically under MSVC.
void runDetached(std::function<void()> fn) {
    auto* heap = new std::function<void()>(std::move(fn));
    HANDLE t = CreateThread(nullptr, 0,
        [](LPVOID p) -> DWORD {
            std::unique_ptr<std::function<void()>> f(
                (std::function<void()>*)p);
            (*f)();
            return 0;
        },
        heap, 0, nullptr);
    if (t)
        CloseHandle(t);
    else
        delete heap;
}

void setText(HWND h, const std::string& utf8) {
    SetWindowTextW(h, widen(utf8).c_str());
}

void setStatus(const std::string& text, bool isError = false) {
    g_statusIsError = isError;
    setText(g_status, text);
    InvalidateRect(g_status, nullptr, TRUE);
}

void copyToClipboard(const std::string& utf8) {
    std::wstring w = widen(utf8);
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        memcpy(GlobalLock(mem), w.c_str(), bytes);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

std::string selectedContext() {
    for (int i = 0; i < NUM_CONTEXTS; i++)
        if (Button_GetCheck(g_ctxRadios[i]) == BST_CHECKED) return CONTEXTS[i];
    return CONTEXTS[0];
}

std::string selectedName() {
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0) return "";
    wchar_t buf[512];
    ListView_GetItemText(g_list, i, 0, buf, 512);
    return narrow(buf);
}

// ---- model combo ------------------------------------------------------------

constexpr wchar_t COMBO_SEPARATOR[] = L"─── all models ───";

// Rebuild the dropdown: recents first, then (optionally free-only) models.
// Keeps whatever is typed in the edit part.
void rebuildModelCombo() {
    std::wstring current(512, 0);
    GetWindowTextW(g_model, &current[0], 512);
    current.resize(wcslen(current.c_str()));

    SendMessageW(g_model, CB_RESETCONTENT, 0, 0);
    g_comboSepIndex = -1;

    bool freeOnly = Button_GetCheck(g_freeOnly) == BST_CHECKED;
    for (const std::string& m : g_prefs.recentModels)
        SendMessageW(g_model, CB_ADDSTRING, 0, (LPARAM)widen(m).c_str());
    if (!g_prefs.recentModels.empty()) {
        g_comboSepIndex = (int)SendMessageW(g_model, CB_ADDSTRING, 0,
                                            (LPARAM)COMBO_SEPARATOR);
    }
    for (const std::string& m : g_models) {
        if (freeOnly && m.rfind(":free") != m.size() - 5) continue;
        SendMessageW(g_model, CB_ADDSTRING, 0, (LPARAM)widen(m).c_str());
    }

    if (current.empty() && !g_prefs.lastModel.empty())
        current = widen(g_prefs.lastModel);
    if (current.empty() && !g_models.empty())
        current = widen(g_models[0]);
    SetWindowTextW(g_model, current.c_str());
}

// ---- history / results ------------------------------------------------------

void updateNav() {
    int total = (int)g_history.size();
    char buf[32];
    snprintf(buf, sizeof buf, "%d / %d", total ? g_histPos + 1 : 0, total);
    setText(g_histLbl, buf);
    EnableWindow(g_back, g_histPos > 0);
    EnableWindow(g_fwd, g_histPos >= 0 && g_histPos < total - 1);
}

void render() {
    ListView_DeleteAllItems(g_list);
    if (g_histPos >= 0 && g_histPos < (int)g_history.size()) {
        const HistEntry& e = g_history[g_histPos];
        for (size_t i = 0; i < e.rows.size(); i++) {
            std::wstring name = widen(e.rows[i].name);
            std::wstring why = widen(e.rows[i].rationale);
            LVITEMW item = {};
            item.mask = LVIF_TEXT;
            item.iItem = (int)i;
            item.pszText = &name[0];
            ListView_InsertItem(g_list, &item);
            ListView_SetItemText(g_list, (int)i, 1, &why[0]);
        }
        setStatus(e.status.empty() ? HINT_DEFAULT : e.status);
        setText(g_generate, "Regenerate");
    }
    updateNav();
}

void finishRequest() {
    g_inFlight = false;
    setText(g_generate, g_history.empty() ? "Generate" : "Regenerate");
    EnableWindow(g_model, TRUE);
}

void showResults(std::vector<llm::NameIdea> rows, const std::string& status) {
    g_history.resize(g_histPos + 1);  // truncate forward history
    g_history.push_back({std::move(rows), status});
    g_histPos = (int)g_history.size() - 1;
    finishRequest();
    render();
}

// Errors are shown in the status line and never enter history.
void showError(const std::string& message) {
    finishRequest();
    setStatus("Error: " + message, true);
}

// ---- LLM requests -----------------------------------------------------------

void startRequest(const std::string& description, const std::string& extra,
                  const std::string& status) {
    std::string key = llm::getApiKey();
    if (key.empty()) {
        setStatus("Error: no OpenRouter key set. Add one in File > Settings "
                  "(get a key at openrouter.ai/keys).", true);
        return;
    }
    std::string model = getWindowTextUtf8(g_model);
    if (model.empty() || model == narrow(COMBO_SEPARATOR)) {
        setStatus("Error: choose a model first.", true);
        return;
    }
    prefs::rememberModel(g_prefs, model);

    LONG seq = InterlockedIncrement(&g_reqSeq);
    g_inFlight = true;
    setText(g_generate, "Cancel");
    EnableWindow(g_model, FALSE);
    setStatus("Asking " + model + "…  (click Cancel to stop)");

    std::string context = selectedContext();
    HWND hwnd = g_hwnd;
    runDetached([hwnd, seq, description, context, model, extra, status]() {
        auto* msg = new ResultMsg{seq, false, {}, status, ""};
        try {
            msg->rows = llm::suggest(description, context, model, extra);
            msg->ok = true;
        } catch (const std::exception& e) {
            msg->error = e.what();
        }
        if (!PostMessageW(hwnd, WM_APP_RESULTS, 0, (LPARAM)msg)) delete msg;
    });
}

void onGenerate() {
    if (g_inFlight) {  // button doubles as Cancel while a request is running
        InterlockedIncrement(&g_reqSeq);
        finishRequest();
        setStatus(g_history.empty() ? HINT_DEFAULT : "Cancelled.");
        return;
    }
    std::string description = getWindowTextUtf8(g_desc);
    if (description.find_first_not_of(" \t\r\n") == std::string::npos) {
        setStatus("Error: describe the thing you want to name first.", true);
        return;
    }
    if (description != g_lastDescription) {  // new thing — start fresh history
        g_lastDescription = description;
        g_history.clear();
        g_histPos = -1;
        updateNav();
    }
    startRequest(description, "", "");
}

void iterate(const std::string& name) {
    if (name.empty() || g_inFlight) return;
    std::string extra = llm::ITERATE_SIMILAR;
    size_t at = extra.find("{name}");
    extra.replace(at, 6, name);
    startRequest(g_lastDescription, extra, "More like \"" + name + "\"");
}

void reprompt(const std::string& name) {
    if (name.empty()) return;
    std::string text = getWindowTextUtf8(g_desc);
    if (!text.empty() && text.back() != '\n') text += "\r\n";
    text += "I like the earlier suggestion \"" + name +
            "\" - build on it. What I like about it: ";
    setText(g_desc, text);
    int len = GetWindowTextLengthW(g_desc);
    SendMessageW(g_desc, EM_SETSEL, len, len);
    SetFocus(g_desc);
}

// ---- settings dialog --------------------------------------------------------

INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG:
            SetWindowTextW(GetDlgItem(dlg, IDC_KEY), widen(llm::getApiKey()).c_str());
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_SHOWKEY: {
                    bool show = IsDlgButtonChecked(dlg, IDC_SHOWKEY) == BST_CHECKED;
                    HWND key = GetDlgItem(dlg, IDC_KEY);
                    SendMessageW(key, EM_SETPASSWORDCHAR, show ? 0 : L'●', 0);
                    InvalidateRect(key, nullptr, TRUE);
                    return TRUE;
                }
                case IDOK: {
                    llm::saveApiKey(getWindowTextUtf8(GetDlgItem(dlg, IDC_KEY)));
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
    }
    return FALSE;
}

// ---- window -----------------------------------------------------------------

HWND makeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int id,
             DWORD exStyle = 0) {
    HWND h = CreateWindowExW(exStyle, cls, text,
                             WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             g_hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

void createControls() {
    for (int i = 0; i < NUM_CONTEXTS; i++) {
        g_ctxRadios[i] = makeCtl(L"BUTTON", widen(CONTEXTS[i]).c_str(),
                                 BS_AUTORADIOBUTTON | WS_TABSTOP |
                                     (i == 0 ? WS_GROUP : 0),
                                 IDC_CTX_BASE + i);
    }
    Button_SetCheck(g_ctxRadios[0], BST_CHECKED);

    g_desc = makeCtl(L"EDIT", L"",
                     ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                         WS_VSCROLL | WS_TABSTOP,
                     IDC_DESC, WS_EX_CLIENTEDGE);

    g_model = makeCtl(L"COMBOBOX", L"",
                      CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
                      IDC_MODEL);
    g_freeOnly = makeCtl(L"BUTTON", L"Free only",
                         BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FREEONLY);

    g_back = makeCtl(L"BUTTON", L"◀", WS_TABSTOP, IDC_BACK);
    g_fwd = makeCtl(L"BUTTON", L"▶", WS_TABSTOP, IDC_FWD);
    g_histLbl = makeCtl(L"STATIC", L"0 / 0", SS_CENTERIMAGE, IDC_HISTLBL);

    g_list = makeCtl(WC_LISTVIEWW, L"",
                     LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                     IDC_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(
        g_list, LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    wchar_t nameHdr[] = L"Name";
    col.pszText = nameHdr;
    col.cx = 160;
    ListView_InsertColumn(g_list, 0, &col);
    wchar_t whyHdr[] = L"Why";
    col.pszText = whyHdr;
    col.cx = 300;
    ListView_InsertColumn(g_list, 1, &col);

    g_status = makeCtl(L"STATIC", widen(HINT_DEFAULT).c_str(), 0, IDC_STATUS);
    g_generate = makeCtl(L"BUTTON", L"Generate",
                         BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_GENERATE);

    updateNav();
}

void layout(int w, int h) {
    const int M = 10;               // outer margin
    const int leftW = w * 46 / 100; // left panel share
    const int rightX = M + leftW + M;
    const int rightW = w - rightX - M;

    // left: context radios stacked, description below
    int y = M;
    for (int i = 0; i < NUM_CONTEXTS; i++) {
        MoveWindow(g_ctxRadios[i], M, y, leftW, 20, TRUE);
        y += 22;
    }
    y += 4;
    MoveWindow(g_desc, M, y, leftW, h - y - M, TRUE);

    // right: model row, nav row, list, status, generate
    int ry = M;
    MoveWindow(g_freeOnly, rightX + rightW - 74, ry + 2, 74, 20, TRUE);
    MoveWindow(g_model, rightX, ry, rightW - 82, 400, TRUE);
    ry += 30;
    MoveWindow(g_back, rightX, ry, 28, 24, TRUE);
    MoveWindow(g_fwd, rightX + 32, ry, 28, 24, TRUE);
    MoveWindow(g_histLbl, rightX + 68, ry, 90, 24, TRUE);
    ry += 30;
    int statusH = 32, btnH = 30;
    int listH = h - ry - statusH - btnH - M - 12;
    MoveWindow(g_list, rightX, ry, rightW, listH, TRUE);
    ListView_SetColumnWidth(g_list, 0, rightW * 32 / 100);
    ListView_SetColumnWidth(g_list, 1, rightW - rightW * 32 / 100 -
                                           GetSystemMetrics(SM_CXVSCROLL) - 4);
    ry += listH + 6;
    MoveWindow(g_status, rightX, ry, rightW, statusH, TRUE);
    ry += statusH + 6;
    MoveWindow(g_generate, rightX, ry, rightW, btnH, TRUE);
}

void resultsContextMenu(int x, int y) {
    std::string name = selectedName();
    if (name.empty()) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_CTX_COPY, L"&Copy name");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_CTX_SIMILAR, L"Show &similar");
    AppendMenuW(menu, MF_STRING, IDM_CTX_REPROMPT, L"&Re-prompt with this…");
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            layout(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize = {720, 420};
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            if ((HWND)lParam == g_status && g_statusIsError) {
                HDC dc = (HDC)wParam;
                SetTextColor(dc, RGB(190, 30, 30));
                SetBkMode(dc, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            break;

        case WM_APP_MODELS: {
            std::unique_ptr<std::vector<std::string>> models(
                (std::vector<std::string>*)lParam);
            g_models = std::move(*models);
            rebuildModelCombo();
            return 0;
        }

        case WM_APP_RESULTS: {
            std::unique_ptr<ResultMsg> res((ResultMsg*)lParam);
            if (res->seq != g_reqSeq) return 0;  // stale (cancelled) request
            if (res->ok)
                showResults(std::move(res->rows), res->status);
            else
                showError(res->error);
            return 0;
        }

        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lParam;
            if (hdr->hwndFrom != g_list) break;
            if (hdr->code == NM_DBLCLK) {
                iterate(selectedName());
                return 0;
            }
            if (hdr->code == NM_RCLICK) {
                POINT pt;
                GetCursorPos(&pt);
                resultsContextMenu(pt.x, pt.y);
                return 0;
            }
            if (hdr->code == LVN_GETINFOTIP) {
                auto* tip = (NMLVGETINFOTIPW*)lParam;
                wchar_t name[512], why[1024];
                ListView_GetItemText(g_list, tip->iItem, 0, name, 512);
                ListView_GetItemText(g_list, tip->iItem, 1, why, 1024);
                std::wstring text = std::wstring(name) + L"\n" + why;
                wcsncpy(tip->pszText, text.c_str(), tip->cchTextMax - 1);
                tip->pszText[tip->cchTextMax - 1] = 0;
                return 0;
            }
            break;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_GENERATE: onGenerate(); return 0;
                case IDC_FREEONLY: rebuildModelCombo(); return 0;
                case IDC_MODEL:
                    // Don't let the separator row be a selection.
                    if (HIWORD(wParam) == CBN_SELCHANGE &&
                        ComboBox_GetCurSel(g_model) == g_comboSepIndex)
                        ComboBox_SetCurSel(g_model, -1);
                    return 0;
                case IDC_BACK:
                    if (g_histPos > 0) { g_histPos--; render(); }
                    return 0;
                case IDC_FWD:
                    if (g_histPos < (int)g_history.size() - 1) { g_histPos++; render(); }
                    return 0;
                case IDM_SETTINGS:
                    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_SETTINGS),
                                    hwnd, SettingsProc, 0);
                    return 0;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
                case IDM_COPY_NAME:
                case IDM_CTX_COPY:
                    copyToClipboard(selectedName());
                    return 0;
                case IDM_CTX_SIMILAR:
                    iterate(selectedName());
                    return 0;
                case IDM_CTX_REPROMPT:
                    reprompt(selectedName());
                    return 0;
                case IDM_CLEAR_DESC:
                    setText(g_desc, "");
                    SetFocus(g_desc);
                    return 0;
                case IDM_ABOUT:
                    MessageBoxW(hwnd,
                                widen(std::string("Namer ") + APP_VERSION +
                                      " (native)\n\nName ideas for anything - code, "
                                      "fiction, papers, products.\nPowered by "
                                      "OpenRouter.\n\n"
                                      "github.com/ByTheSeaL/Namer-C").c_str(),
                                L"About Namer", MB_OK | MB_ICONINFORMATION);
                    return 0;
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HMENU buildMenu() {
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_SETTINGS, L"&Settings…");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_COPY_NAME, L"&Copy selected name");
    AppendMenuW(edit, MF_STRING, IDM_CLEAR_DESC, L"C&lear description");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About Namer");
    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInst;
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Segoe UI");
    g_prefs = prefs::load();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"NamerMain";
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);

    g_hwnd = CreateWindowW(L"NamerMain", L"Namer", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 960, 600,
                           nullptr, buildMenu(), hInst, nullptr);
    createControls();
    rebuildModelCombo();
    ShowWindow(g_hwnd, nCmdShow);

    // Fetch the live model list in the background.
    HWND hwnd = g_hwnd;
    runDetached([hwnd]() {
        auto* models = new std::vector<std::string>(llm::listModels());
        if (!PostMessageW(hwnd, WM_APP_MODELS, 0, (LPARAM)models)) delete models;
    });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // Tab navigation between controls; Enter stays usable in the
        // multiline description edit thanks to ES_WANTRETURN.
        if (IsDialogMessageW(g_hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
