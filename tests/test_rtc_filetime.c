/* sceRtcSetWin32FileTime / sceRtcGetWin32FileTime (src/runtime_rtc.c): they convert between a
 * SceRtcDateTime and a Win32 FILETIME (100 ns since 1601-01-01). Diablo III reads its season dates
 * through them; a conversion that fails makes every season "end in 1 minute". */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include "../src/runtime_rtc.c"

/* The export table is not used here; the file only needs the symbol. */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid;
    return 0;
}

int main(void) {
    DateTime t;
    uint64_t filetime = 0;
    /* 1970-01-01 00:00:00 UTC is FILETIME 116444736000000000. */
    assert(rtc_set_win32_filetime(&t, UINT64_C(116444736000000000)) == 0);
    assert(t.year == 1970 && t.month == 1 && t.day == 1 && t.hour == 0 && t.minute == 0 && t.second == 0);
    assert(rtc_get_win32_filetime(&t, &filetime) == 0 && filetime == UINT64_C(116444736000000000));
    /* A date with a time of day and a microsecond round-trips: 2025-02-09 12:34:56.000001. */
    DateTime season = {2025, 2, 9, 12, 34, 56, 1};
    assert(rtc_get_win32_filetime(&season, &filetime) == 0);
    DateTime back;
    assert(rtc_set_win32_filetime(&back, filetime) == 0);
    assert(back.year == 2025 && back.month == 2 && back.day == 9 && back.hour == 12 &&
           back.minute == 34 && back.second == 56 && back.microsecond == 1);
    /* The Seasons.txt dates of the offline seasons: 2025 comes before 2036. */
    DateTime end = {2036, 2, 9, 1, 0, 0, 0};
    uint64_t later = 0;
    assert(rtc_get_win32_filetime(&end, &later) == 0 && later > filetime);
    /* Before 1601 there is no FILETIME; a bad date or a missing pointer is an error. */
    DateTime early = {1600, 12, 31, 0, 0, 0, 0}, bad = {2025, 13, 1, 0, 0, 0, 0};
    assert(rtc_get_win32_filetime(&early, &filetime) == ERR_INVALID_YEAR);
    assert(rtc_get_win32_filetime(&bad, &filetime) == ERR_INVALID_MONTH);
    assert(rtc_get_win32_filetime(NULL, &filetime) == ERR_INVALID_POINTER);
    assert(rtc_set_win32_filetime(NULL, 0) == ERR_INVALID_POINTER);
    puts("rtc filetime ok");
    return 0;
}
