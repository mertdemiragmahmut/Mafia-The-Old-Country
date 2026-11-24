#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <atomic>
#include <string>

struct ProxyHiddAttributes {
    ULONG Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
};

using FnGetAttributes = BOOLEAN (__stdcall *)(HANDLE, ProxyHiddAttributes*);
using FnSetOutputReport = BOOLEAN (__stdcall *)(HANDLE, PVOID, ULONG);
using FnSetFeature = BOOLEAN (__stdcall *)(HANDLE, PVOID, ULONG);

extern "C" {
void* g_pfn_HidD_FlushQueue = nullptr;
void* g_pfn_HidD_FreePreparsedData = nullptr;
void* g_pfn_HidD_GetConfiguration = nullptr;
void* g_pfn_HidD_GetFeature = nullptr;
void* g_pfn_HidD_GetHidGuid = nullptr;
void* g_pfn_HidD_GetIndexedString = nullptr;
void* g_pfn_HidD_GetInputReport = nullptr;
void* g_pfn_HidD_GetManufacturerString = nullptr;
void* g_pfn_HidD_GetMsGenreDescriptor = nullptr;
void* g_pfn_HidD_GetNumInputBuffers = nullptr;
void* g_pfn_HidD_GetPhysicalDescriptor = nullptr;
void* g_pfn_HidD_GetPreparsedData = nullptr;
void* g_pfn_HidD_GetProductString = nullptr;
void* g_pfn_HidD_GetSerialNumberString = nullptr;
void* g_pfn_HidD_Hello = nullptr;
void* g_pfn_HidD_SetConfiguration = nullptr;
void* g_pfn_HidD_SetNumInputBuffers = nullptr;
void* g_pfn_HidP_GetButtonArray = nullptr;
void* g_pfn_HidP_GetButtonCaps = nullptr;
void* g_pfn_HidP_GetCaps = nullptr;
void* g_pfn_HidP_GetData = nullptr;
void* g_pfn_HidP_GetExtendedAttributes = nullptr;
void* g_pfn_HidP_GetLinkCollectionNodes = nullptr;
void* g_pfn_HidP_GetScaledUsageValue = nullptr;
void* g_pfn_HidP_GetSpecificButtonCaps = nullptr;
void* g_pfn_HidP_GetSpecificValueCaps = nullptr;
void* g_pfn_HidP_GetUsageValue = nullptr;
void* g_pfn_HidP_GetUsageValueArray = nullptr;
void* g_pfn_HidP_GetUsages = nullptr;
void* g_pfn_HidP_GetUsagesEx = nullptr;
void* g_pfn_HidP_GetValueCaps = nullptr;
void* g_pfn_HidP_GetVersionInternal = nullptr;
void* g_pfn_HidP_InitializeReportForID = nullptr;
void* g_pfn_HidP_MaxDataListLength = nullptr;
void* g_pfn_HidP_MaxUsageListLength = nullptr;
void* g_pfn_HidP_SetButtonArray = nullptr;
void* g_pfn_HidP_SetData = nullptr;
void* g_pfn_HidP_SetScaledUsageValue = nullptr;
void* g_pfn_HidP_SetUsageValue = nullptr;
void* g_pfn_HidP_SetUsageValueArray = nullptr;
void* g_pfn_HidP_SetUsages = nullptr;
void* g_pfn_HidP_TranslateUsagesToI8042ScanCodes = nullptr;
void* g_pfn_HidP_UnsetUsages = nullptr;
void* g_pfn_HidP_UsageListDifference = nullptr;
}

namespace {

static HMODULE g_realHid = nullptr;
static FnGetAttributes g_realGetAttributes = nullptr;
static FnSetOutputReport g_realSetOutputReport = nullptr;
static FnSetFeature g_realSetFeature = nullptr;
static std::once_flag g_initOnce;

static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;
static std::atomic<int> g_attrHideLogs{0};
static std::atomic<int> g_outBlockLogs{0};
static std::atomic<int> g_featLogs{0};

static void ProxyLog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_logFile) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(g_logFile, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_logFile, fmt, args);
    va_end(args);
    std::fprintf(g_logFile, "\n");
    std::fflush(g_logFile);
}

static void InitLog(HMODULE hSelf) {
    wchar_t modPath[MAX_PATH]{};
    GetModuleFileNameW(hSelf, modPath, MAX_PATH);
    std::wstring dir(modPath);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);

    std::wstring ue4ssDir = dir + L"\\ue4ss";
    DWORD attr = GetFileAttributesW(ue4ssDir.c_str());
    std::wstring logW = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        ? (ue4ssDir + L"\\DualSenseHidProxy.log")
        : (dir + L"\\DualSenseHidProxy.log");

    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logFile = _wfopen(logW.c_str(), L"w");
}

static void CloseLog() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

static bool IsSonyDualSenseVidPid(USHORT vid, USHORT pid) {
    return vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2);
}

static void EnsureRealHidLoaded() {
    std::call_once(g_initOnce, []() {
        wchar_t sysDir[MAX_PATH]{};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring realPath = std::wstring(sysDir) + L"\\hid.dll";
        g_realHid = LoadLibraryW(realPath.c_str());
        if (!g_realHid) {
            ProxyLog("HID-Proxy: ERROR failed to load System32\\hid.dll err=%lu", GetLastError());
            return;
        }

        g_realGetAttributes = reinterpret_cast<FnGetAttributes>(GetProcAddress(g_realHid, "HidD_GetAttributes"));
        g_realSetOutputReport = reinterpret_cast<FnSetOutputReport>(GetProcAddress(g_realHid, "HidD_SetOutputReport"));
        g_realSetFeature = reinterpret_cast<FnSetFeature>(GetProcAddress(g_realHid, "HidD_SetFeature"));

        g_pfn_HidD_FlushQueue = GetProcAddress(g_realHid, "HidD_FlushQueue");
        g_pfn_HidD_FreePreparsedData = GetProcAddress(g_realHid, "HidD_FreePreparsedData");
        g_pfn_HidD_GetConfiguration = GetProcAddress(g_realHid, "HidD_GetConfiguration");
        g_pfn_HidD_GetFeature = GetProcAddress(g_realHid, "HidD_GetFeature");
        g_pfn_HidD_GetHidGuid = GetProcAddress(g_realHid, "HidD_GetHidGuid");
        g_pfn_HidD_GetIndexedString = GetProcAddress(g_realHid, "HidD_GetIndexedString");
        g_pfn_HidD_GetInputReport = GetProcAddress(g_realHid, "HidD_GetInputReport");
        g_pfn_HidD_GetManufacturerString = GetProcAddress(g_realHid, "HidD_GetManufacturerString");
        g_pfn_HidD_GetMsGenreDescriptor = GetProcAddress(g_realHid, "HidD_GetMsGenreDescriptor");
        g_pfn_HidD_GetNumInputBuffers = GetProcAddress(g_realHid, "HidD_GetNumInputBuffers");
        g_pfn_HidD_GetPhysicalDescriptor = GetProcAddress(g_realHid, "HidD_GetPhysicalDescriptor");
        g_pfn_HidD_GetPreparsedData = GetProcAddress(g_realHid, "HidD_GetPreparsedData");
        g_pfn_HidD_GetProductString = GetProcAddress(g_realHid, "HidD_GetProductString");
        g_pfn_HidD_GetSerialNumberString = GetProcAddress(g_realHid, "HidD_GetSerialNumberString");
        g_pfn_HidD_Hello = GetProcAddress(g_realHid, "HidD_Hello");
        g_pfn_HidD_SetConfiguration = GetProcAddress(g_realHid, "HidD_SetConfiguration");
        g_pfn_HidD_SetNumInputBuffers = GetProcAddress(g_realHid, "HidD_SetNumInputBuffers");
        g_pfn_HidP_GetButtonArray = GetProcAddress(g_realHid, "HidP_GetButtonArray");
        g_pfn_HidP_GetButtonCaps = GetProcAddress(g_realHid, "HidP_GetButtonCaps");
        g_pfn_HidP_GetCaps = GetProcAddress(g_realHid, "HidP_GetCaps");
        g_pfn_HidP_GetData = GetProcAddress(g_realHid, "HidP_GetData");
        g_pfn_HidP_GetExtendedAttributes = GetProcAddress(g_realHid, "HidP_GetExtendedAttributes");
        g_pfn_HidP_GetLinkCollectionNodes = GetProcAddress(g_realHid, "HidP_GetLinkCollectionNodes");
        g_pfn_HidP_GetScaledUsageValue = GetProcAddress(g_realHid, "HidP_GetScaledUsageValue");
        g_pfn_HidP_GetSpecificButtonCaps = GetProcAddress(g_realHid, "HidP_GetSpecificButtonCaps");
        g_pfn_HidP_GetSpecificValueCaps = GetProcAddress(g_realHid, "HidP_GetSpecificValueCaps");
        g_pfn_HidP_GetUsageValue = GetProcAddress(g_realHid, "HidP_GetUsageValue");
        g_pfn_HidP_GetUsageValueArray = GetProcAddress(g_realHid, "HidP_GetUsageValueArray");
        g_pfn_HidP_GetUsages = GetProcAddress(g_realHid, "HidP_GetUsages");
        g_pfn_HidP_GetUsagesEx = GetProcAddress(g_realHid, "HidP_GetUsagesEx");
        g_pfn_HidP_GetValueCaps = GetProcAddress(g_realHid, "HidP_GetValueCaps");
        g_pfn_HidP_GetVersionInternal = GetProcAddress(g_realHid, "HidP_GetVersionInternal");
        g_pfn_HidP_InitializeReportForID = GetProcAddress(g_realHid, "HidP_InitializeReportForID");
        g_pfn_HidP_MaxDataListLength = GetProcAddress(g_realHid, "HidP_MaxDataListLength");
        g_pfn_HidP_MaxUsageListLength = GetProcAddress(g_realHid, "HidP_MaxUsageListLength");
        g_pfn_HidP_SetButtonArray = GetProcAddress(g_realHid, "HidP_SetButtonArray");
        g_pfn_HidP_SetData = GetProcAddress(g_realHid, "HidP_SetData");
        g_pfn_HidP_SetScaledUsageValue = GetProcAddress(g_realHid, "HidP_SetScaledUsageValue");
        g_pfn_HidP_SetUsageValue = GetProcAddress(g_realHid, "HidP_SetUsageValue");
        g_pfn_HidP_SetUsageValueArray = GetProcAddress(g_realHid, "HidP_SetUsageValueArray");
        g_pfn_HidP_SetUsages = GetProcAddress(g_realHid, "HidP_SetUsages");
        g_pfn_HidP_TranslateUsagesToI8042ScanCodes = GetProcAddress(g_realHid, "HidP_TranslateUsagesToI8042ScanCodes");
        g_pfn_HidP_UnsetUsages = GetProcAddress(g_realHid, "HidP_UnsetUsages");
        g_pfn_HidP_UsageListDifference = GetProcAddress(g_realHid, "HidP_UsageListDifference");

        ProxyLog("HID-Proxy: Resolved System32\\hid.dll (%p)", g_realHid);
    });
}

} // namespace

extern "C" {

BOOLEAN __stdcall HidD_GetAttributes(HANDLE HidDeviceObject, ProxyHiddAttributes* Attributes) {
    EnsureRealHidLoaded();
    if (!g_realGetAttributes) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    BOOLEAN ok = g_realGetAttributes(HidDeviceObject, Attributes);
    if (ok && Attributes && IsSonyDualSenseVidPid(Attributes->VendorID, Attributes->ProductID)) {
        if (g_attrHideLogs.fetch_add(1) < 10) {
            ProxyLog("HID-Proxy: HidD_GetAttributes hid Sony DualSense (VID=0x%04X PID=0x%04X) from game process",
                     Attributes->VendorID, Attributes->ProductID);
        }
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }
    return ok;
}

BOOLEAN __stdcall HidD_SetOutputReport(HANDLE HidDeviceObject, PVOID ReportBuffer, ULONG ReportBufferLength) {
    EnsureRealHidLoaded();
    if (g_realGetAttributes) {
        ProxyHiddAttributes attrs{};
        attrs.Size = sizeof(attrs);
        if (g_realGetAttributes(HidDeviceObject, &attrs) &&
            IsSonyDualSenseVidPid(attrs.VendorID, attrs.ProductID)) {
            if (g_outBlockLogs.fetch_add(1) < 10) {
                ProxyLog("HID-Proxy: Blocked game HidD_SetOutputReport on DualSense (len=%lu)", ReportBufferLength);
            }
            return TRUE;
        }
    }
    if (!g_realSetOutputReport) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return g_realSetOutputReport(HidDeviceObject, ReportBuffer, ReportBufferLength);
}

BOOLEAN __stdcall HidD_SetFeature(HANDLE HidDeviceObject, PVOID ReportBuffer, ULONG ReportBufferLength) {
    EnsureRealHidLoaded();
    if (g_realGetAttributes) {
        ProxyHiddAttributes attrs{};
        attrs.Size = sizeof(attrs);
        if (g_realGetAttributes(HidDeviceObject, &attrs) &&
            IsSonyDualSenseVidPid(attrs.VendorID, attrs.ProductID)) {
            if (g_featLogs.fetch_add(1) < 10) {
                ProxyLog("HID-Proxy: Observed game HidD_SetFeature on DualSense (len=%lu)", ReportBufferLength);
            }
        }
    }
    if (!g_realSetFeature) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return g_realSetFeature(HidDeviceObject, ReportBuffer, ReportBufferLength);
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitLog(hModule);
        ProxyLog("HID-Proxy: hid.dll attached to process PID=%lu", GetCurrentProcessId());
        EnsureRealHidLoaded();
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        CloseLog();
        if (lpReserved == nullptr && g_realHid) {
            FreeLibrary(g_realHid);
            g_realHid = nullptr;
        }
    }
    return TRUE;
}
