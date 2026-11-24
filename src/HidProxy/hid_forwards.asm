; x64 tail-jump forwarders for System32\hid.dll pass-through exports

EXTERN g_pfn_HidD_FlushQueue:QWORD
EXTERN g_pfn_HidD_FreePreparsedData:QWORD
EXTERN g_pfn_HidD_GetConfiguration:QWORD
EXTERN g_pfn_HidD_GetFeature:QWORD
EXTERN g_pfn_HidD_GetHidGuid:QWORD
EXTERN g_pfn_HidD_GetIndexedString:QWORD
EXTERN g_pfn_HidD_GetInputReport:QWORD
EXTERN g_pfn_HidD_GetManufacturerString:QWORD
EXTERN g_pfn_HidD_GetMsGenreDescriptor:QWORD
EXTERN g_pfn_HidD_GetNumInputBuffers:QWORD
EXTERN g_pfn_HidD_GetPhysicalDescriptor:QWORD
EXTERN g_pfn_HidD_GetPreparsedData:QWORD
EXTERN g_pfn_HidD_GetProductString:QWORD
EXTERN g_pfn_HidD_GetSerialNumberString:QWORD
EXTERN g_pfn_HidD_Hello:QWORD
EXTERN g_pfn_HidD_SetConfiguration:QWORD
EXTERN g_pfn_HidD_SetNumInputBuffers:QWORD
EXTERN g_pfn_HidP_GetButtonArray:QWORD
EXTERN g_pfn_HidP_GetButtonCaps:QWORD
EXTERN g_pfn_HidP_GetCaps:QWORD
EXTERN g_pfn_HidP_GetData:QWORD
EXTERN g_pfn_HidP_GetExtendedAttributes:QWORD
EXTERN g_pfn_HidP_GetLinkCollectionNodes:QWORD
EXTERN g_pfn_HidP_GetScaledUsageValue:QWORD
EXTERN g_pfn_HidP_GetSpecificButtonCaps:QWORD
EXTERN g_pfn_HidP_GetSpecificValueCaps:QWORD
EXTERN g_pfn_HidP_GetUsageValue:QWORD
EXTERN g_pfn_HidP_GetUsageValueArray:QWORD
EXTERN g_pfn_HidP_GetUsages:QWORD
EXTERN g_pfn_HidP_GetUsagesEx:QWORD
EXTERN g_pfn_HidP_GetValueCaps:QWORD
EXTERN g_pfn_HidP_GetVersionInternal:QWORD
EXTERN g_pfn_HidP_InitializeReportForID:QWORD
EXTERN g_pfn_HidP_MaxDataListLength:QWORD
EXTERN g_pfn_HidP_MaxUsageListLength:QWORD
EXTERN g_pfn_HidP_SetButtonArray:QWORD
EXTERN g_pfn_HidP_SetData:QWORD
EXTERN g_pfn_HidP_SetScaledUsageValue:QWORD
EXTERN g_pfn_HidP_SetUsageValue:QWORD
EXTERN g_pfn_HidP_SetUsageValueArray:QWORD
EXTERN g_pfn_HidP_SetUsages:QWORD
EXTERN g_pfn_HidP_TranslateUsagesToI8042ScanCodes:QWORD
EXTERN g_pfn_HidP_UnsetUsages:QWORD
EXTERN g_pfn_HidP_UsageListDifference:QWORD

.code

HidD_FlushQueue PROC
    jmp qword ptr [g_pfn_HidD_FlushQueue]
HidD_FlushQueue ENDP

HidD_FreePreparsedData PROC
    jmp qword ptr [g_pfn_HidD_FreePreparsedData]
HidD_FreePreparsedData ENDP

HidD_GetConfiguration PROC
    jmp qword ptr [g_pfn_HidD_GetConfiguration]
HidD_GetConfiguration ENDP

HidD_GetFeature PROC
    jmp qword ptr [g_pfn_HidD_GetFeature]
HidD_GetFeature ENDP

HidD_GetHidGuid PROC
    jmp qword ptr [g_pfn_HidD_GetHidGuid]
HidD_GetHidGuid ENDP

HidD_GetIndexedString PROC
    jmp qword ptr [g_pfn_HidD_GetIndexedString]
HidD_GetIndexedString ENDP

HidD_GetInputReport PROC
    jmp qword ptr [g_pfn_HidD_GetInputReport]
HidD_GetInputReport ENDP

HidD_GetManufacturerString PROC
    jmp qword ptr [g_pfn_HidD_GetManufacturerString]
HidD_GetManufacturerString ENDP

HidD_GetMsGenreDescriptor PROC
    jmp qword ptr [g_pfn_HidD_GetMsGenreDescriptor]
HidD_GetMsGenreDescriptor ENDP

HidD_GetNumInputBuffers PROC
    jmp qword ptr [g_pfn_HidD_GetNumInputBuffers]
HidD_GetNumInputBuffers ENDP

HidD_GetPhysicalDescriptor PROC
    jmp qword ptr [g_pfn_HidD_GetPhysicalDescriptor]
HidD_GetPhysicalDescriptor ENDP

HidD_GetPreparsedData PROC
    jmp qword ptr [g_pfn_HidD_GetPreparsedData]
HidD_GetPreparsedData ENDP

HidD_GetProductString PROC
    jmp qword ptr [g_pfn_HidD_GetProductString]
HidD_GetProductString ENDP

HidD_GetSerialNumberString PROC
    jmp qword ptr [g_pfn_HidD_GetSerialNumberString]
HidD_GetSerialNumberString ENDP

HidD_Hello PROC
    jmp qword ptr [g_pfn_HidD_Hello]
HidD_Hello ENDP

HidD_SetConfiguration PROC
    jmp qword ptr [g_pfn_HidD_SetConfiguration]
HidD_SetConfiguration ENDP

HidD_SetNumInputBuffers PROC
    jmp qword ptr [g_pfn_HidD_SetNumInputBuffers]
HidD_SetNumInputBuffers ENDP

HidP_GetButtonArray PROC
    jmp qword ptr [g_pfn_HidP_GetButtonArray]
HidP_GetButtonArray ENDP

HidP_GetButtonCaps PROC
    jmp qword ptr [g_pfn_HidP_GetButtonCaps]
HidP_GetButtonCaps ENDP

HidP_GetCaps PROC
    jmp qword ptr [g_pfn_HidP_GetCaps]
HidP_GetCaps ENDP

HidP_GetData PROC
    jmp qword ptr [g_pfn_HidP_GetData]
HidP_GetData ENDP

HidP_GetExtendedAttributes PROC
    jmp qword ptr [g_pfn_HidP_GetExtendedAttributes]
HidP_GetExtendedAttributes ENDP

HidP_GetLinkCollectionNodes PROC
    jmp qword ptr [g_pfn_HidP_GetLinkCollectionNodes]
HidP_GetLinkCollectionNodes ENDP

HidP_GetScaledUsageValue PROC
    jmp qword ptr [g_pfn_HidP_GetScaledUsageValue]
HidP_GetScaledUsageValue ENDP

HidP_GetSpecificButtonCaps PROC
    jmp qword ptr [g_pfn_HidP_GetSpecificButtonCaps]
HidP_GetSpecificButtonCaps ENDP

HidP_GetSpecificValueCaps PROC
    jmp qword ptr [g_pfn_HidP_GetSpecificValueCaps]
HidP_GetSpecificValueCaps ENDP

HidP_GetUsageValue PROC
    jmp qword ptr [g_pfn_HidP_GetUsageValue]
HidP_GetUsageValue ENDP

HidP_GetUsageValueArray PROC
    jmp qword ptr [g_pfn_HidP_GetUsageValueArray]
HidP_GetUsageValueArray ENDP

HidP_GetUsages PROC
    jmp qword ptr [g_pfn_HidP_GetUsages]
HidP_GetUsages ENDP

HidP_GetUsagesEx PROC
    jmp qword ptr [g_pfn_HidP_GetUsagesEx]
HidP_GetUsagesEx ENDP

HidP_GetValueCaps PROC
    jmp qword ptr [g_pfn_HidP_GetValueCaps]
HidP_GetValueCaps ENDP

HidP_GetVersionInternal PROC
    jmp qword ptr [g_pfn_HidP_GetVersionInternal]
HidP_GetVersionInternal ENDP

HidP_InitializeReportForID PROC
    jmp qword ptr [g_pfn_HidP_InitializeReportForID]
HidP_InitializeReportForID ENDP

HidP_MaxDataListLength PROC
    jmp qword ptr [g_pfn_HidP_MaxDataListLength]
HidP_MaxDataListLength ENDP

HidP_MaxUsageListLength PROC
    jmp qword ptr [g_pfn_HidP_MaxUsageListLength]
HidP_MaxUsageListLength ENDP

HidP_SetButtonArray PROC
    jmp qword ptr [g_pfn_HidP_SetButtonArray]
HidP_SetButtonArray ENDP

HidP_SetData PROC
    jmp qword ptr [g_pfn_HidP_SetData]
HidP_SetData ENDP

HidP_SetScaledUsageValue PROC
    jmp qword ptr [g_pfn_HidP_SetScaledUsageValue]
HidP_SetScaledUsageValue ENDP

HidP_SetUsageValue PROC
    jmp qword ptr [g_pfn_HidP_SetUsageValue]
HidP_SetUsageValue ENDP

HidP_SetUsageValueArray PROC
    jmp qword ptr [g_pfn_HidP_SetUsageValueArray]
HidP_SetUsageValueArray ENDP

HidP_SetUsages PROC
    jmp qword ptr [g_pfn_HidP_SetUsages]
HidP_SetUsages ENDP

HidP_TranslateUsagesToI8042ScanCodes PROC
    jmp qword ptr [g_pfn_HidP_TranslateUsagesToI8042ScanCodes]
HidP_TranslateUsagesToI8042ScanCodes ENDP

HidP_UnsetUsages PROC
    jmp qword ptr [g_pfn_HidP_UnsetUsages]
HidP_UnsetUsages ENDP

HidP_UsageListDifference PROC
    jmp qword ptr [g_pfn_HidP_UsageListDifference]
HidP_UsageListDifference ENDP

END
