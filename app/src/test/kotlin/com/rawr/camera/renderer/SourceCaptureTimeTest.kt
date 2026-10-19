package com.rawr.camera.renderer
import java.time.ZoneId
import org.junit.Test
import org.junit.Assert.*
class SourceCaptureTimeTest {
    private val jakarta = ZoneId.of("Asia/Jakarta")
    @Test fun offsetAndSubsecPinTheInstant() {
        // 2026-05-01 04:34:56.042 UTC.
        assertEquals(1777610096042L, SourceCaptureTime.takenMillis("2026:05:01 12:34:56", "+08:00", "042", jakarta))
        assertEquals(1777610096500L, SourceCaptureTime.takenMillis("2026:05:01 12:34:56", "+08:00", "5", jakarta))
    }
    @Test fun offsetLessDateReadsInTheDeviceZone() {
        assertEquals(1777613696000L, SourceCaptureTime.takenMillis("2026:05:01 12:34:56", "", "", jakarta))
    }
    @Test fun unknownDatesAreAbsent() {
        assertNull(SourceCaptureTime.takenMillis("", "+08:00", "", jakarta))
        assertNull(SourceCaptureTime.takenMillis("0000:00:00 00:00:00", "", "", jakarta))
        assertEquals(1777613696000L, SourceCaptureTime.takenMillis("2026:05:01 12:34:56", "bogus", "x1", jakarta))
    }
}
