#include "TimeConverter.hpp"

_Success_(return) BOOL TimeConvertFileTime(
    _In_ CONST FILETIME *inputFileTime,
    _Out_ LPFILETIME outputFileTime,
    _In_ TIME_CONVERT_FILE_TIME_DIRECTION direction,
    _In_opt_ PDYNAMIC_TIME_ZONE_INFORMATION dynamicTimeZone) {
    SYSTEMTIME inputTime, outputTime;

    if (!FileTimeToSystemTime(inputFileTime, &inputTime))
        return FALSE;

    switch (direction) {
    case TimeConvertUniversalToLocal:
        if (!SystemTimeToTzSpecificLocalTimeEx(dynamicTimeZone, &inputTime, &outputTime))
            return FALSE;
        break;
    case TimeConvertLocalToUniversal:
        if (!TzSpecificLocalTimeToSystemTimeEx(dynamicTimeZone, &inputTime, &outputTime))
            return FALSE;
        break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!SystemTimeToFileTime(&outputTime, outputFileTime))
        return FALSE;

    return TRUE;
}
