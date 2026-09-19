/*
 * a9tc - A列車で行こう9 繁體中文化 proxy DLL（最小版）
 *
 * 只做當初驗證過能穩定運作的三件事，其餘一律不做：
 *   1. 轉發 DirectInput8Create 給系統的 dinput8
 *   2. IAT hook 字型建立函式，把字型換成繁中字型
 *   3. 背景執行緒掃記憶體，把日文原地覆寫成中文
 *
 * 編譯:
 *   x86_64-w64-mingw32-gcc -shared -O2 -static -static-libgcc \
 *       -o dinput8.dll a9tc_min.c -Wl,--kill-at
 * 執行:
 *   WINEDLLOVERRIDES="dinput8=n,b" %command%
 */

/* ── 散布版：絕不寫入任何遊戲檔案 ──────────────────────
 * 與本機版（../a9tc_min.c）是獨立的兩份程式，互不影響。
 *
 * 本機版會把中文字元「追加」到 data/bmf/un01.bin、un02.bin，並清空
 * fe0x 與 fs0x 快取，那會永久改動遊戲資料檔。散布版改成攔截 CreateFileW：
 *   un01.bin / un02.bin → 在暫存區組出「原檔 + 中文字元」，回傳暫存檔
 *   fe0x.bin 與 fs0x.bin → 整個導向暫存區，字型快取不落在遊戲資料夾
 * 結果：遊戲資料夾一個位元組都不會被改，Steam 驗證檔案完全通過。
 * log 與診斷輸出也一併移到暫存區。
 */
#include <windows.h>
#include <wctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

/* ---- D3DX 字型描述（照 d3dx9.h，避免相依 DirectX SDK） ---- */
typedef struct {
    INT   Height; UINT Width; UINT Weight; UINT MipLevels; BOOL Italic;
    BYTE  CharSet; BYTE OutputPrecision; BYTE Quality; BYTE PitchAndFamily;
    WCHAR FaceName[32];
} A9_FONTDESCW;

typedef HRESULT (WINAPI *PFN_D3DXFont)(void *, const A9_FONTDESCW *, void **);
typedef HRESULT (WINAPI *PFN_DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

static WCHAR g_face[LF_FACESIZE] = L"";   /* 空 = 由下方候選清單自動挑 */
/* 三個平台各自常見的繁中字型，由上而下取第一個裝得到的。
   Windows 沒有 Noto、macOS 沒有微軟正黑體，所以不能寫死單一個。 */
static const WCHAR *g_face_try[] = {
    L"Noto Sans CJK TC", L"Noto Sans TC", L"Source Han Sans TC",
    L"Microsoft JhengHei", L"微軟正黑體",
    L"PingFang TC", L"蘋方-繁", L"Heiti TC",
    L"MS Gothic", NULL
};
static BYTE  g_charset = DEFAULT_CHARSET;
static int   g_fontscale = 100;   /* #fontscale=：字型大小百分比，100 = 原樣。
                                     遊戲介面是固定像素排版，調太大字會擠出框外，
                                     建議 110~130 之間試。 */
static int   g_delay   = 20;
static int   g_patch   = 0;
static int   g_nofont  = 0;
static WCHAR g_find[256] = L"";
static int   g_dumpja  = 0;   /* #dumpja：把「含假名但沒被翻到」的字串記下來 */   /* #find=：唯讀診斷，找字串在哪塊記憶體 */   /* #nofont：完全不掛字型 hook */

static HMODULE       g_real_di8 = NULL;
static PFN_DI8Create g_di8create = NULL;
static PFN_D3DXFont  o_d3dxfont = NULL;
static HFONT (WINAPI *o_createfont)(const LOGFONTW *) = NULL;
/* 遊戲同時用 CreateFontW 和 CreateFontIndirectW，點陣字型圖集是走前者，
   只掛 Indirect 版的話 #fontscale 完全沒作用。 */
typedef HFONT (WINAPI *PFN_CreateFontW)(int,int,int,int,int,DWORD,DWORD,DWORD,
                                        DWORD,DWORD,DWORD,DWORD,DWORD,LPCWSTR);
static PFN_CreateFontW o_createfontw = NULL;
static FILE *g_log = NULL;
static HINSTANCE g_self = NULL;
static WCHAR g_tmpdir[MAX_PATH] = L"";   /* 我們自己的暫存資料夾 */

/* 取得（必要時建立）暫存資料夾。所有寫入一律落在這裡，不碰遊戲資料夾。 */
static const WCHAR *tmpdir(void)
{
    if (!g_tmpdir[0]) {
        WCHAR t[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, t)) wcscpy(t, L"C:\\windows\\temp\\");
        wsprintfW(g_tmpdir, L"%sa9tc", t);
        CreateDirectoryW(g_tmpdir, NULL);
    }
    return g_tmpdir;
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

static void lg(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); fflush(g_log);
    va_end(ap);
}

/* ---------------- 對照表 ---------------- */
#define MAX_ENTRIES 16384   /* 對照表條數上限。超過會被無聲丟棄——
                              曾因 4096 上限把最短的一批譯文全砍掉。 */
/* 對照表只存「日文原文的雜湊」與譯文，不存原文本身。
   這樣散布出去的檔案裡不含任何遊戲原文，比對速度也不受影響——
   本來就要掃到字串才比對，改成算雜湊再查表甚至更快。 */
typedef unsigned long long u64;
static struct { u64 h; WCHAR *dst; } g_tbl[MAX_ENTRIES];

/* FNV-1a 64 位元，以長度起始，長度不同必然得到不同雜湊 */
static u64 hash_str(const WCHAR *s, int n)
{
    u64 h = 1469598103934665603ULL ^ (u64)(unsigned)n;
    for (int i = 0; i < n; i++) { h ^= (u64)(unsigned)s[i]; h *= 1099511628211ULL; }
    return h;
}
static int g_tbl_n = 0;
static int g_dropped = 0;   /* 超過上限被丟掉的條數，一定要報出來 */
/* 對照表字串的專屬區塊。一定要跟遊戲的記憶體分開、而且掃描時跳過——
   否則掃描器會掃到我們自己的日文原文、比對成功、把它覆寫成中文，
   那條對照就此失效。散在堆積各處的 malloc 沒辦法整塊排除，所以改成
   一次配置、自己切。 */
static BYTE *g_arena_lo = NULL, *g_arena_hi = NULL;

static void load_table(void)
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    WCHAR *sl = wcsrchr(path, L'\\');
    if (!sl) return;
    wcscpy(sl + 1, L"a9tc.txt");

    FILE *f = _wfopen(path, L"rb");
    if (!f) { lg("[cfg] 找不到 a9tc.txt\n"); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return; }
    char *raw = (char *)malloc(sz + 1);
    if (!raw) { fclose(f); return; }
    sz = (long)fread(raw, 1, sz, f); raw[sz] = 0; fclose(f);

    /* 上限：每個 UTF-8 位元組最多產生一個 WCHAR，所以 sz+1 個 WCHAR 一定夠 */
    WCHAR *arena = (WCHAR *)malloc((size_t)(sz + 1) * sizeof(WCHAR));
    if (!arena) { free(raw); return; }
    WCHAR *ap = arena, *aend = arena + sz + 1;
    g_arena_lo = (BYTE *)arena;

    char *p = raw;
    if (sz >= 3 && (unsigned char)p[0] == 0xEF) p += 3;     /* BOM */

    while (*p) {
        char *eol = strchr(p, '\n'); if (eol) *eol = 0;
        char *cr = strchr(p, '\r');  if (cr)  *cr  = 0;

        if      (!strncmp(p, "#font=",    6)) {
            /* 字串類指令要先剪掉行末註解與前後空白，否則「#font=   # 說明」
               會把整串當成字型名稱，自動挑選就不會啟動。 */
            char buf[256]; size_t n = 0;
            const char *q = p + 6;
            while (*q == ' ' || *q == '\t') q++;
            while (*q && *q != '#' && n < sizeof buf - 1) buf[n++] = *q++;
            while (n && (buf[n-1] == ' ' || buf[n-1] == '\t')) n--;
            buf[n] = 0;
            MultiByteToWideChar(CP_UTF8, 0, buf, -1, g_face, LF_FACESIZE);
            g_face[LF_FACESIZE-1] = 0;
        }
        else if (!strncmp(p, "#charset=", 9)) g_charset = (BYTE)atoi(p + 9);
        else if (!strncmp(p, "#fontscale=", 11)) {
            g_fontscale = atoi(p + 11);
            if (g_fontscale < 50)  g_fontscale = 50;
            if (g_fontscale > 300) g_fontscale = 300;
        }
        else if (!strncmp(p, "#delay=",   7)) { g_delay = atoi(p + 7); if (g_delay < 1) g_delay = 1; }
        else if (!strncmp(p, "#find=",    6)) { MultiByteToWideChar(CP_UTF8,0,p+6,-1,g_find,256); g_find[255]=0; }
        else if (!strncmp(p, "#dumpja",   7)) g_dumpja = 1;
        else if (!strncmp(p, "#nofont",   7)) g_nofont = 1;
        else if (!strncmp(p, "#patch",    6)) g_patch = 1;
        else if (*p && *p != '#' && g_tbl_n >= MAX_ENTRIES) g_dropped++;
        else if (*p && *p != '#') {
            char *tab = strchr(p, '\t');
            if (tab) {
                *tab = 0;
                const char *a = p, *b = tab + 1;
                if (*a && *b) {
                    char *end = NULL;
                    u64 h = strtoull(a, &end, 16);
                    int nb = MultiByteToWideChar(CP_UTF8,0,b,-1,NULL,0);
                    WCHAR *wb = ap;
                    if (end && end != a && ap + nb <= aend) {
                        ap += nb;
                        MultiByteToWideChar(CP_UTF8,0,b,-1,wb,nb);
                        g_tbl[g_tbl_n].h   = h;
                        g_tbl[g_tbl_n].dst = wb;
                        g_tbl_n++;
                    }
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(raw);
    g_arena_hi = (BYTE *)ap;
    lg("[cfg] 載入 %d 條對照, 延遲 %d 秒, 表區塊 %p~%p（掃描時跳過）\n",
       g_tbl_n, g_delay, (void *)g_arena_lo, (void *)g_arena_hi);
    if (g_dropped)
        lg("[cfg] ★ 警告：超過上限 %d，有 %d 條沒載入！請調高 MAX_ENTRIES\n",
           MAX_ENTRIES, g_dropped);
}



/* ── 字型檔導向：遊戲資料夾唯讀 ──────────────────────────
   遊戲會以 CreateFileW 開 data\bmf\ 底下六個檔：
     un01.bin / un02.bin  字元清單（要哪些字就畫哪些字）
     fe0x.bin, fs0x.bin   產生出來的點陣字型快取
   我們在暫存區放一份「原檔 + 中文字元」的 un0N，以及空的快取檔，
   然後把這六個檔名的開檔一律導過去。遊戲資料夾完全不會被寫入。 */

static HANDLE (WINAPI *o_createfilew)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE) = NULL;

/* 遊戲傳進來的可能是絕對路徑也可能是相對路徑（exe 裡存的是 "data\\bmf\\"），
   斜線方向與大小寫也不保證。所以改成「先取檔名比對，再確認上層目錄叫 bmf」，
   不要去比對路徑前綴。 */
static const WCHAR *bmf_name(LPCWSTR path)
{
    static const WCHAR *names[] = { L"un01.bin", L"un02.bin", L"fe01.bin",
                                    L"fe02.bin", L"fs01.bin", L"fs02.bin", NULL };
    if (!path) return NULL;
    const WCHAR *n = path;
    for (const WCHAR *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') n = p + 1;
    int hit = 0;
    for (int i = 0; names[i]; i++) if (!_wcsicmp(n, names[i])) { hit = 1; break; }
    if (!hit) return NULL;
    /* 上層目錄必須是 bmf，免得誤攔到別處的同名檔 */
    if (n - path < 5) return NULL;
    const WCHAR *e = n - 1;                    /* 指向分隔符 */
    const WCHAR *b = e;
    while (b > path && b[-1] != L'\\' && b[-1] != L'/') b--;
    if (e - b != 3) return NULL;
    if (towlower(b[0]) != L'b' || towlower(b[1]) != L'm' || towlower(b[2]) != L'f') return NULL;
    return n;
}

static HANDLE WINAPI h_createfilew(LPCWSTR name, DWORD acc, DWORD share,
                                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                   DWORD flags, HANDLE tmpl)
{
    const WCHAR *n = bmf_name(name);
    if (n) {
        static int shown = 0;
        WCHAR re[MAX_PATH];
        wsprintfW(re, L"%s\\%s", tmpdir(), n);
        if (shown < 12) { shown++; lg("[bmf] 導向 %ls → %ls\n", name, re); }
        return o_createfilew(re, acc, share, sa, disp, flags, tmpl);
    }
    return o_createfilew(name, acc, share, sa, disp, flags, tmpl);
}

/* 把譯文用到的字元，追加到暫存區的 un0N.bin（原檔唯讀讀入，不回寫） */
static void build_charset(const WCHAR *gamedir, const WCHAR *fname)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH];
    wsprintfW(src, L"%s\\data\\bmf\\%s", gamedir, fname);
    wsprintfW(dst, L"%s\\%s", tmpdir(), fname);

    FILE *f = _wfopen(src, L"rb");
    if (!f) { lg("[bmf] 讀不到 %ls\n", src); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 4 * 1024 * 1024) { fclose(f); return; }
    BYTE *raw = (BYTE *)malloc((size_t)sz + 2);
    if (!raw) { fclose(f); return; }
    sz = (long)fread(raw, 1, (size_t)sz, f); fclose(f);

    /* 原檔已有哪些字：用 64K 的位元圖記，比排序查找簡單也夠快 */
    BYTE *have = (BYTE *)calloc(65536 / 8, 1);
    if (!have) { free(raw); return; }
    int start = (sz >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) ? 2 : 0;
    for (long i = start; i + 1 < sz; i += 2) {
        unsigned c = (unsigned)raw[i] | ((unsigned)raw[i + 1] << 8);
        have[c >> 3] |= (BYTE)(1u << (c & 7));
    }

    FILE *o = _wfopen(dst, L"wb");
    if (!o) { free(raw); free(have); return; }
    fwrite(raw, 1, (size_t)sz, o);

    int added = 0;
    for (int i = 0; i < g_tbl_n; i++) {
        for (const WCHAR *w = g_tbl[i].dst; *w; w++) {
            unsigned c = (unsigned)*w;
            if (c == L'\n' || (have[c >> 3] & (1u << (c & 7)))) continue;
            have[c >> 3] |= (BYTE)(1u << (c & 7));
            BYTE two[2] = { (BYTE)(c & 0xFF), (BYTE)(c >> 8) };
            fwrite(two, 1, 2, o); added++;
        }
    }
    fclose(o); free(raw); free(have);
    lg("[bmf] %ls：原檔 %ld bytes，追加 %d 個字元 → 暫存區\n", fname, sz, added);
}

/* 暫存區備妥六個檔：兩個字元清單 + 四個空快取（原廠也是 0 bytes） */
static void prepare_bmf(void)
{
    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR *sl = wcsrchr(exe, L'\\');
    if (!sl) return;
    *sl = 0;
    build_charset(exe, L"un01.bin");
    build_charset(exe, L"un02.bin");
    static const WCHAR *cache[] = { L"fe01.bin", L"fe02.bin", L"fs01.bin", L"fs02.bin" };
    for (int i = 0; i < 4; i++) {
        WCHAR p[MAX_PATH];
        wsprintfW(p, L"%s\\%ls", tmpdir(), cache[i]);
        FILE *f = _wfopen(p, L"wb");   /* 清成 0 bytes，讓字型快取重新產生 */
        if (f) fclose(f);
    }
}

/* 字型：候選清單由上而下取第一個裝得到的 */
static int CALLBACK ef_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD t, LPARAM p)
{
    (void)lf; (void)tm; (void)t; *(int *)p = 1; return 0;
}
static int font_exists(const WCHAR *face)
{
    LOGFONTW lf; memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    HDC dc = GetDC(NULL); if (!dc) return 0;
    int found = 0;
    EnumFontFamiliesExW(dc, &lf, ef_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}
static void pick_font(void)
{
    if (g_face[0]) { lg("[font] 使用設定指定的 %ls\n", g_face); return; }
    for (int i = 0; g_face_try[i]; i++) {
        if (font_exists(g_face_try[i])) {
            wcsncpy(g_face, g_face_try[i], LF_FACESIZE - 1);
            lg("[font] 自動選用 %ls\n", g_face);
            return;
        }
    }
    lg("[font] ★ 候選字型都找不到，將不替換字型（中文可能顯示為方框）\n");
}

/* 對照表雜湊索引：用 (長度, 首字) 分桶。
   沒有索引的話每個字串都要線性比 1792 條，再乘上「試 4 個起始偏移」
   就是 7000 次比對，掃描成本直接爆掉、遊戲會卡住。 */
#define IDX_SIZE 8192
static int g_idx_head[IDX_SIZE];
static int g_idx_next[MAX_ENTRIES];

static unsigned idx_hash(u64 h)
{ return (unsigned)(h & (IDX_SIZE - 1)); }

static void build_index(void)
{
    for (int i = 0; i < IDX_SIZE; i++) g_idx_head[i] = -1;
    for (int i = 0; i < g_tbl_n; i++) {
        unsigned h = idx_hash(g_tbl[i].h);
        g_idx_next[i] = g_idx_head[h];
        g_idx_head[h] = i;
    }
    lg("[index] 雜湊索引建立完成（%d 條）\n", g_tbl_n);
}

/* ---------------- IAT hook ---------------- */
static void *hook_iat(const char *dll, const char *fn, void *newfn)
{
    HMODULE base = GetModuleHandleW(NULL);
    if (!base) return NULL;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return NULL;

    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)base + rva);
         imp->Name; imp++) {
        if (_stricmp((const char *)((BYTE *)base + imp->Name), dll)) continue;
        if (!imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *o = (IMAGE_THUNK_DATA *)((BYTE *)base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *c = (IMAGE_THUNK_DATA *)((BYTE *)base + imp->FirstThunk);
        for (; o->u1.AddressOfData; o++, c++) {
            if (o->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)((BYTE *)base + o->u1.AddressOfData);
            if (strcmp((const char *)n->Name, fn)) continue;
            DWORD old;
            if (!VirtualProtect(&c->u1.Function, sizeof(void *), PAGE_EXECUTE_READWRITE, &old))
                return NULL;
            void *prev = (void *)c->u1.Function;
            c->u1.Function = (DWORD_PTR)newfn;
            VirtualProtect(&c->u1.Function, sizeof(void *), old, &old);
            lg("[hook] %s!%s\n", dll, fn);
            return prev;
        }
    }
    return NULL;
}

static HRESULT WINAPI h_d3dxfont(void *dev, const A9_FONTDESCW *d, void **out)
{
    if (!o_d3dxfont) return 0x8007000E;
    if (!d) return o_d3dxfont(dev, d, out);
    A9_FONTDESCW c = *d;
    wcsncpy(c.FaceName, g_face, 31); c.FaceName[31] = 0;
    c.CharSet = g_charset;
    if (g_fontscale != 100 && c.Height)
        c.Height = c.Height * g_fontscale / 100;
    return o_d3dxfont(dev, &c, out);
}

static HFONT WINAPI h_createfontw(int h, int w, int esc, int ori, int weight,
                                  DWORD ital, DWORD und, DWORD stk, DWORD cs,
                                  DWORD op, DWORD cp, DWORD q, DWORD pf, LPCWSTR face)
{
    if (!o_createfontw) return NULL;
    /* 只動大小，字型名稱與字集一律照原樣傳回去。
       換字型會連帶改掉行高與基線，造成行距跑掉、字被裁——
       中文字形是走 CreateFontIndirectW 那條路徑產生的，這裡不需要換。 */
    if (g_fontscale != 100 && h) h = h * g_fontscale / 100;
    return o_createfontw(h, w, esc, ori, weight, ital, und, stk,
                         cs, op, cp, q, pf, face);
}

static HFONT WINAPI h_createfont(const LOGFONTW *lf)
{
    if (!lf || !o_createfont) return o_createfont ? o_createfont(lf) : NULL;
    LOGFONTW c = *lf;
    wcsncpy(c.lfFaceName, g_face, LF_FACESIZE - 1);
    c.lfFaceName[LF_FACESIZE - 1] = 0;
    c.lfCharSet = g_charset;
    if (g_fontscale != 100 && c.lfHeight)   /* lfHeight 可能是負數，比例縮放會自動保留符號 */
        c.lfHeight = c.lfHeight * g_fontscale / 100;
    return o_createfont(&c);
}


/* 命中過的記憶體區段。
   全記憶體掃一趟要 9~10 秒（遊戲佔 2.3 GB），但字串其實只集中在幾塊區段裡。
   記住它們，平常只掃這幾塊（約 26 MB），成本差 90 倍。
   每 20 輪做一次全掃，用來發現新配置的區段。 */
#define MAX_HOT 64
static BYTE  *g_hot[MAX_HOT];
static int    g_hot_n = 0;

static void hot_add(BYTE *base)
{
    for (int i = 0; i < g_hot_n; i++) if (g_hot[i] == base) return;
    if (g_hot_n < MAX_HOT) g_hot[g_hot_n++] = base;
}

/* ---------------- 記憶體改寫 ----------------
 * 只碰 State==MEM_COMMIT 且 Protect 剛好等於 PAGE_READWRITE 的區段。
 * 精確比對很重要：PAGE_GUARD 之類的旗標一旦被遮罩掉，寫進防護頁會當掉。
 * 譯文只在塞得進「原字串 + 其後的 NUL 填充」時才寫，絕不越界。
 */
static int patch_once(int full)
{
    int done = 0;
    int hot_i = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE *a = (BYTE *)si.lpMinimumApplicationAddress;
    BYTE *mx = (BYTE *)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;

    while (1) {
        if (full) {
            if (a >= mx) break;
            if (VirtualQuery(a, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        } else {
            if (hot_i >= g_hot_n) break;
            BYTE *hb = g_hot[hot_i++];
            if (VirtualQuery(hb, &mbi, sizeof(mbi)) != sizeof(mbi)) continue;
        }
        BYTE *next = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE
            && mbi.RegionSize >= 64
            && !((BYTE *)mbi.BaseAddress < g_arena_hi && next > g_arena_lo)) {
            WCHAR *w = (WCHAR *)mbi.BaseAddress;
            WCHAR *end = (WCHAR *)((BYTE *)mbi.BaseAddress + mbi.RegionSize) - 1;
            while (w < end) {
                if (!*w) { w++; continue; }
                WCHAR *st = w;
                while (w < end && *w >= 0x20) w++;
                /* 收尾可以是 NUL，也可以是換行——遊戲的導引文字是一整塊用 \n
                   分行的文字，只認 NUL 的話整塊都會被跳過。 */
                if (w < end && (!*w || *w == 0x0a || *w == 0x0d)) {
                    int nl  = (*w != 0);        /* 被換行收尾：後面緊接著下一行，沒有空間可用 */
                    int len = (int)(w - st);
                    if (len >= 2 && len <= 127) {
                        int    hit = -1;
                        WCHAR *ms  = st;      /* 實際比對／改寫的起點 */
                        int    ml  = len;
                        u64 hh = hash_str(ms, ml);
                        for (int i = g_idx_head[idx_hash(hh)]; i >= 0; i = g_idx_next[i])
                            if (g_tbl[i].h == hh) { hit = i; break; }
                        /* 完整比對失敗，且開頭那個字的低位元組是 0x75 ——
                           記憶體裡字串前面常黏著這個雜訊字（聵 U+8075 佔 1573 次、
                           u U+0075 佔 99 次，合計 99.8%），跳過它再試一次。
                           先完整比對、失敗才退一格，所以只會增加命中不會減少。
                           限定 0x75 是為了避開「逆ポール vs ポール」這類誤判：
                           那 14 組危險配對的首字低位元組都不是 0x75。 */
                        if (hit < 0 && len >= 3 && (st[0] & 0xFF) == 0x75) {
                            ms = st + 1; ml = len - 1;
                            hh = hash_str(ms, ml);
                            for (int i = g_idx_head[idx_hash(hh)]; i >= 0; i = g_idx_next[i])
                                if (g_tbl[i].h == hh) { hit = i; break; }
                        }
                        if (hit >= 0) {
                            int pad = 0;
                            if (!nl) {                       /* NUL 收尾才有後面的填充可以借用 */
                                WCHAR *z = w + 1;
                                while (z < end && !*z && pad < 64) { pad++; z++; }
                            }
                            int dl = (int)wcslen(g_tbl[hit].dst);
                            if (dl <= ml + pad) {
                                memcpy(ms, g_tbl[hit].dst, (size_t)dl * sizeof(WCHAR));
                                if (nl) {
                                    /* 寫 NUL 會把整塊文字從這裡截斷，只能補空白 */
                                    for (int k = dl; k < ml; k++) ms[k] = L' ';
                                } else {
                                    for (int k = dl; k <= ml + pad; k++) ms[k] = 0;
                                }
                                done++;
                                hot_add((BYTE *)mbi.BaseAddress);
                            }
                        }
                    }
                }
                w++;
            }
        }
        if (full) {
            if (next <= a) break;
            a = next;
        }
    }
    return done;
}

static DWORD WINAPI patch_thread(LPVOID u)     /* 熱區：高頻、便宜 */
{
    (void)u;
    Sleep((DWORD)g_delay * 1000);
    int total = 0;
    for (int round = 1; ; round++) {
        int n = patch_once(g_hot_n == 0);      /* 還沒有熱區就先全掃一次 */
        if (n > 0) {
            total += n;
            lg("[patch] 熱區第 %d 輪：改寫 %d 處，累計 %d，熱區 %d 塊\n",
               round, n, total, g_hot_n);
        }
        Sleep(150);
    }
    return 0;
}

/* 全掃放在另一條執行緒，才不會拖慢熱區掃描。
   它的任務是「發現新區段」—— 找到一次之後那塊就進熱區清單，
   之後由高頻執行緒負責，反應就快了。 */
static DWORD WINAPI fullscan_thread(LPVOID u)
{
    (void)u;
    Sleep((DWORD)g_delay * 1000 + 3000);
    for (;;) {
        int before = g_hot_n;
        int n = patch_once(1);
        if (n > 0 || g_hot_n != before)
            lg("[patch] 全掃：改寫 %d 處，熱區 %d → %d 塊\n", n, before, g_hot_n);
        Sleep(2000);
    }
    return 0;
}

/* ---------------- 匯出 ---------------- */

__declspec(dllexport)
HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID *o, LPUNKNOWN u)
{
    if (!g_di8create) return 0x8007000E;
    return g_di8create(h, v, r, o, u);
}


/* 唯讀診斷：在所有可讀記憶體裡找指定字串，回報位址與保護屬性。
   完全不寫入，只是要弄清楚我們的改寫為什麼碰不到它。 */
static const char *prot_name(DWORD p)
{
    switch (p & 0xFF) {
    case PAGE_READONLY:          return "READONLY";
    case PAGE_READWRITE:         return "READWRITE";
    case PAGE_WRITECOPY:         return "WRITECOPY";
    case PAGE_EXECUTE_READ:      return "EXEC_READ";
    case PAGE_EXECUTE_READWRITE: return "EXEC_RW";
    case PAGE_EXECUTE_WRITECOPY: return "EXEC_WC";
    case PAGE_NOACCESS:          return "NOACCESS";
    default:                     return "其他";
    }
}

static DWORD WINAPI find_thread(LPVOID u)
{
    (void)u;
    size_t need = wcslen(g_find), nb = need * sizeof(WCHAR);
    if (!need) return 0;
    lg("[find] 開始搜尋（%u 字）\n", (unsigned)need);

    for (int pass = 1; pass <= 40; pass++) {
        Sleep(5000);
        SYSTEM_INFO si; GetSystemInfo(&si);
        BYTE *a = (BYTE *)si.lpMinimumApplicationAddress;
        BYTE *mx = (BYTE *)si.lpMaximumApplicationAddress;
        MEMORY_BASIC_INFORMATION mbi;
        int hits = 0;
        while (a < mx && VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            BYTE *next = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
            DWORD pr = mbi.Protect & 0xFF;
            int readable = (mbi.State == MEM_COMMIT)
                        && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
                        && (pr==PAGE_READONLY||pr==PAGE_READWRITE||pr==PAGE_WRITECOPY
                          ||pr==PAGE_EXECUTE_READ||pr==PAGE_EXECUTE_READWRITE
                          ||pr==PAGE_EXECUTE_WRITECOPY);
            if (readable && mbi.RegionSize >= nb) {
                BYTE *q = (BYTE *)mbi.BaseAddress, *e = q + mbi.RegionSize - nb;
                for (; q <= e; q += 2) {
                    if (*(WCHAR *)q != g_find[0]) continue;
                    if (memcmp(q, g_find, nb)) continue;
                    lg("[find] 第 %d 輪 命中 %p  保護=%s(0x%lx)  型態=%s  區塊=%p+%u\n",
                       pass, q, prot_name(mbi.Protect), (unsigned long)mbi.Protect,
                       mbi.Type == MEM_IMAGE ? "IMAGE" : mbi.Type == MEM_MAPPED ? "MAPPED" : "PRIVATE",
                       mbi.BaseAddress, (unsigned)mbi.RegionSize);
                    if (++hits >= 6) break;
                    q += nb;
                }
            }
            if (hits >= 6 || next <= a) break;
            a = next;
        }
        if (hits) { lg("[find] 第 %d 輪共 %d 處，停止搜尋\n", pass, hits); return 0; }
        if (pass % 6 == 0) lg("[find] 第 %d 輪：還沒找到（請點選該地圖）\n", pass);
    }
    lg("[find] 逾時，始終找不到\n");
    return 0;
}


/* 找出漏網之魚：掃記憶體裡含假名的 UTF-16 字串，
   凡是不在對照表裡的就記下來。執行時才組合出來的字串也抓得到。
   唯讀，只寫 log。 */
#define MISS_BITS 18
#define MISS_SIZE (1 << MISS_BITS)          /* 262144 格，夠裝整局遊戲的漏網字串 */
static unsigned g_miss[MISS_SIZE];          /* 0 代表空位 */
static int      g_miss_n = 0;
static FILE    *g_missf = NULL;

/* 只認假名的話，「娯楽施設」「資材工場」「発電所」這種純漢字的日文標籤
   一條都挖不到。改成只要有漢字或假名就收，我們自己寫進去的中文譯文會一起
   被撈出來，但那在文字檔裡減掉即可，不必為它加程式。 */
static int has_cjk(const WCHAR *w, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned c = w[i];
        if ((c >= 0x3040 && c <= 0x30ff) || (c >= 0x4e00 && c <= 0x9fff)) return 1;
    }
    return 0;
}
static int ja_ok(const WCHAR *w, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned c = w[i];
        if (!((c >= 0x20 && c < 0x7f) || (c >= 0x3000 && c <= 0x30ff)
           || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xff01 && c <= 0xff60)
           || c == 0xffe5)) return 0;
    }
    return 1;
}
static int miss_seen(const WCHAR *w, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned)w[i]; h *= 16777619u; }
    if (!h) h = 1;                          /* 0 留給空位 */
    unsigned i = h & (MISS_SIZE - 1);
    for (;;) {
        if (!g_miss[i]) {
            if (g_miss_n >= MISS_SIZE / 2) return 1;   /* 半滿就停止收錄，不要退化 */
            g_miss[i] = h; g_miss_n++; return 0;
        }
        if (g_miss[i] == h) return 1;
        i = (i + 1) & (MISS_SIZE - 1);
    }
}

static DWORD WINAPI dumpja_thread(LPVOID u)
{
    (void)u;
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    WCHAR *sl = wcsrchr(path, L'\\');
    (void)sl;
    wsprintfW(path, L"%s\\a9_missing.txt", tmpdir());
    g_missf = _wfopen(path, L"wb");
    if (g_missf) { fputc(0xEF,g_missf); fputc(0xBB,g_missf); fputc(0xBF,g_missf); }
    Sleep((DWORD)g_delay * 1000 + 5000);

    for (;;) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        BYTE *a = (BYTE *)si.lpMinimumApplicationAddress;
        BYTE *mx = (BYTE *)si.lpMaximumApplicationAddress;
        MEMORY_BASIC_INFORMATION mbi;
        int found = 0;
        while (a < mx && VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            BYTE *next = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE
                && mbi.RegionSize >= 64
                && !((BYTE *)mbi.BaseAddress < g_arena_hi && next > g_arena_lo)) {
                WCHAR *w = (WCHAR *)mbi.BaseAddress;
                WCHAR *end = (WCHAR *)((BYTE *)mbi.BaseAddress + mbi.RegionSize) - 1;
                while (w < end) {
                    if (!*w) { w++; continue; }
                    WCHAR *st = w;
                    while (w < end && *w >= 0x20) w++;
                    if (w < end && (!*w || *w == 0x0a || *w == 0x0d)) {
                        int len = (int)(w - st);
                        if (len >= 2 && len <= 127 && has_cjk(st, len) && ja_ok(st, len)) {
                            int known = 0;
                            u64 kh = hash_str(st, len);
                            for (int i = g_idx_head[idx_hash(kh)]; i >= 0; i = g_idx_next[i])
                                if (g_tbl[i].h == kh) { known = 1; break; }
                            if (!known && !miss_seen(st, len) && g_missf) {
                                char buf[512];
                                int nb = WideCharToMultiByte(CP_UTF8,0,st,len,buf,sizeof(buf)-1,NULL,NULL);
                                if (nb > 0) { buf[nb]=0; fprintf(g_missf, "%s\n", buf); fflush(g_missf); found++; }
                            }
                        }
                    }
                    w++;
                }
            }
            if (next <= a) break;
            a = next;
        }
        if (found) lg("[missing] 本輪新增 %d 條未翻字串（累計 %d）\n", found, g_miss_n);
        Sleep(10000);
    }
    return 0;
}

static void init(void)
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    WCHAR *sl = wcsrchr(path, L'\\');
    (void)sl;
    {   /* log 一律寫在暫存區，遊戲資料夾保持唯讀 */
        WCHAR lp[MAX_PATH];
        wsprintfW(lp, L"%s\\a9tc.log", tmpdir());
        g_log = _wfopen(lp, L"wb");
    }
    lg("[init] a9tc 散布版啟動（不寫入任何遊戲檔案）\n");

    WCHAR sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    wcscat(sys, L"\\dinput8.dll");
    g_real_di8 = LoadLibraryW(sys);
    lg("[init] LoadLibrary(%ls)\n        回傳 %p，自己是 %p → %s\n",
       sys, (void *)g_real_di8, (void *)g_self,
       (g_real_di8 == (HMODULE)g_self) ? "**撈到自己！無限遞迴**" : "不同模組，OK");
    if (g_real_di8 && g_real_di8 != (HMODULE)g_self) {
        g_di8create = (PFN_DI8Create)GetProcAddress(g_real_di8, "DirectInput8Create");
        lg("[init] DirectInput8Create = %p（我們的是 %p）\n",
           (void *)g_di8create, (void *)DirectInput8Create);
        if ((void *)g_di8create == (void *)DirectInput8Create) {
            lg("[init] **指向我們自己，強制放棄轉發**\n");
            g_di8create = NULL;
        }
    } else {
        g_di8create = NULL;
        lg("[init] 無法取得真 dinput8，不轉發\n");
    }

    load_table();
    build_index();

    /* 先把暫存區的字型檔備妥，再掛導向——順序反了遊戲會開到空檔 */
    prepare_bmf();
    o_createfilew = (HANDLE (WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE))
        hook_iat("KERNEL32.dll", "CreateFileW", h_createfilew);
    lg("[hook] KERNEL32.dll!CreateFileW → %s（字型檔導向暫存區 %ls）\n",
       o_createfilew ? "OK" : "失敗", tmpdir());

    if (!g_nofont) {
        pick_font();
        o_createfont = (HFONT (WINAPI *)(const LOGFONTW *))
            hook_iat("GDI32.dll", "CreateFontIndirectW", h_createfont);
        o_createfontw = (PFN_CreateFontW)
            hook_iat("GDI32.dll", "CreateFontW", h_createfontw);
        if (o_createfontw) lg("[hook] GDI32.dll!CreateFontW\n");
        static const char *dx[] = { "d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_41.dll", "d3dx9_40.dll", NULL };
        for (int i = 0; dx[i]; i++) {
            void *prev = hook_iat(dx[i], "D3DXCreateFontIndirectW", h_d3dxfont);
            if (prev) { o_d3dxfont = (PFN_D3DXFont)prev; break; }
        }
    } else {
        lg("[init] #nofont：不掛字型 hook\n");
    }

    if (g_dumpja) {
        HANDLE t = CreateThread(NULL, 0, dumpja_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    if (g_find[0]) {
        HANDLE t = CreateThread(NULL, 0, find_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    if (g_patch && g_tbl_n > 0) {
        HANDLE t1 = CreateThread(NULL, 0, patch_thread, NULL, 0, NULL);
        if (t1) CloseHandle(t1);
        HANDLE t2 = CreateThread(NULL, 0, fullscan_thread, NULL, 0, NULL);
        if (t2) CloseHandle(t2);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) { g_self = inst; DisableThreadLibraryCalls(inst); init(); }
    return TRUE;
}
