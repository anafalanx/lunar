# tools/uishot.tcl -- render a deterministic (stubbed-engine) Lunar UI state
# for screenshot-based UI review, so the layout can be evaluated without a live
# network sync. Driven by shot.tcl, which launches wish on this script:
#
#   z uishot <out.png> <state>
#   tools/shot.tcl <wish> tools/uishot.tcl <out.png> <state>
#
# <state>: collapsed (default) | expanded
#
# It sources lunar.tcl (which builds the real UI via lunar::main), then replaces
# the engine commands with fixed sample data so every capture is identical.
# Modelled on els's tools/readme_shots.tcl staged-scene approach.

set ::LUNAR_ROOT [file dirname [file dirname [file normalize [info script]]]]
set ::UISHOT_STATE [expr {[llength $::argv] ? [lindex $::argv 0] : "collapsed"}]
set ::argv {} ; set ::argc 0

source [file join $::LUNAR_ROOT lunar.tcl]   ;# runs lunar::main -> builds the UI

# --- deterministic engine stubs (no network) --------------------------------
proc ::lunar::status {} {
    return [dict create state ok synced 1 hasTime 1 \
        utcMs 1751731872000 boundMs 207 sysDeltaValid 1 sysDeltaMs 420]
}
proc ::lunar::sources {} {
    return [list \
        [dict create ok 1 label "INRIM-1"        offsetMs 4  rttMs 71] \
        [dict create ok 1 label "METAS"          offsetMs -1 rttMs 50] \
        [dict create ok 1 label "RISE"           offsetMs 2  rttMs 54] \
        [dict create ok 1 label "NPL-1"          offsetMs 0  rttMs 38] \
        [dict create ok 1 label "NTS:netnod"     offsetMs -1 rttMs 53] \
        [dict create ok 1 label "NTS:cloudflare" offsetMs 1  rttMs 37]]
}
proc ::lunar::localtime {ms zone} {
    return [dict create hour 17 minute 51 second 12 \
        wday 0 day 5 month 7 year 2026 offSec 7200 abbr CEST]
}
proc ::lunar::update_status {} { return [dict create available 0 version ""] }

# render immediately with the stubs, then stage the requested panel state
after 150 {
    set ::lunar::tz "Europe/Brussels"
    catch { lunar::render [::lunar::status] }
    catch { lunar::render_sources [::lunar::sources] }
    if {$::UISHOT_STATE eq "expanded" && !$::lunar::sources_open} {
        catch { lunar::toggle_sources }
    }
    update idletasks ; update
}
