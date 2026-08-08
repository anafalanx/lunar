# tools/uishot.tcl -- render a deterministic (stubbed-engine) Lunar UI state
# for screenshot-based UI review, so the layout can be evaluated without a live
# network sync. Driven by shot.tcl, which launches wish on this script:
#
#   z uishot <out.png> ?state?
#   tools/shot.tcl <wish> tools/uishot.tcl <out.png> ?state?
#
# ?state?: trusted (default) | degraded | wide | stopped -- stages the
# uncertainty-fan second hand at a representative bound:
#   trusted   ok        ±207 ms  (red hairline fan)
#   degraded  degraded  ±300 ms  (amber: unauthenticated corroboration)
#   wide      holdover  ±2.5 s   (amber fan, visibly open)
#   stopped   holdover  ±6.5 s   (> default 5 s ceiling: seconds withdrawn)
#
# It sources lunar.tcl (which builds the real UI via lunar::main), then replaces
# the engine commands with fixed sample data so every capture is identical.
# Modelled on els's tools/readme_shots.tcl staged-scene approach.

set ::LUNAR_ROOT [file dirname [file dirname [file normalize [info script]]]]
set ::UISHOT_STATE [expr {[llength $::argv] ? [lindex $::argv 0] : "trusted"}]
set ::argv {} ; set ::argc 0

source [file join $::LUNAR_ROOT lunar.tcl]   ;# runs lunar::main -> builds the UI

# --- deterministic engine stubs (no network) --------------------------------
switch -- $::UISHOT_STATE {
    degraded { set ::UISHOT_TRUST degraded ; set ::UISHOT_BOUND 300 }
    wide     { set ::UISHOT_TRUST holdover ; set ::UISHOT_BOUND 2500 }
    stopped  { set ::UISHOT_TRUST holdover ; set ::UISHOT_BOUND 6500 }
    default  { set ::UISHOT_TRUST ok       ; set ::UISHOT_BOUND 207 }
}
proc ::lunar::status {} {
    return [dict create state $::UISHOT_TRUST synced 1 hasTime 1 \
        utcMs 1751731872000 boundMs $::UISHOT_BOUND sysDeltaValid 1 sysDeltaMs 420]
}
proc ::lunar::localtime {ms zone} {
    return [dict create hour 17 minute 51 second 12 \
        wday 0 day 5 month 7 year 2026 offSec 7200 abbr CEST]
}
# Render immediately with the stubs; twice, so the stop rule's two-read
# debounce settles when staging the stopped state.
after 150 {
    set ::lunar::tz "Europe/Brussels"
    catch { lunar::render [::lunar::status] }
    catch { lunar::render [::lunar::status] }
    update idletasks ; update
}
