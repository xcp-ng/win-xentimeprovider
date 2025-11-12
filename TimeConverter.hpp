#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef enum _TIME_CONVERT_FILE_TIME_DIRECTION {
    TimeConvertUniversalToLocal,
    TimeConvertLocalToUniversal,
} TIME_CONVERT_FILE_TIME_DIRECTION;

_Success_(return) BOOL TimeConvertFileTime(
    _In_ CONST FILETIME *inputFileTime,
    _Out_ LPFILETIME outputFileTime,
    _In_ TIME_CONVERT_FILE_TIME_DIRECTION direction,
    _In_opt_ PDYNAMIC_TIME_ZONE_INFORMATION dynamicTimeZone);
