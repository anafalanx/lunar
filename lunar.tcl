# lunar.tcl -- Lunar's Tcl/Tk shell (startup script, packaged as main.tcl).
#
# Analog clock shell for Lunar's disciplined C timescale. Time and trust come
# from the engine via ::lunar::* (see src/lunarx.c); the networking stays on
# its worker threads while Tk owns the window chrome and interaction.

package require Tk

namespace eval lunar {
    variable version "0.54"
    variable poll_ms   60000 ;# BASE cadence: re-sync about this often when OK
    variable poll_min   8000 ;# FAST floor while acquiring / re-anchoring
    variable poll_max 300000 ;# RELAXED ceiling once well disciplined (5 min)
    variable poll_good     0 ;# consecutive converged cycles (drives the relax ramp)
    variable poll_bad      0 ;# consecutive unhealthy cycles (drives the fast backoff)
    variable poll_cur  60000 ;# current interval actually scheduled (for the log/About)
    variable repoll_after {} ;# pending [after] token, so poll_now can cancel it
    variable poll_forced_at 0 ;# [clock milliseconds] of the last forced poll
    variable stopped_prev 0  ;# last rendered stop flag (recover-fast edge)
    variable hastime_prev 1  ;# last rendered hasTime (recover-fast edge)
    variable log_active 0    ;# reentry latch so logging can't recurse into bgerror
    variable square_extra_w 0
    variable square_extra_h 0
    variable square_work_x 0
    variable square_work_y 0
    variable square_work_w 0
    variable square_work_h 0
    variable square_last_w 0
    variable square_last_h 0
    variable square_fixing 0
    variable square_pending 0
    variable square_dragging 0
    variable settings_preview_after ""
}

# ---- diagnostics: log + non-modal bgerror (els's approach) ------------------
# A GUI-subsystem exe has no console, so an uncaught async error otherwise pops
# Tk's modal "raining dialogs". Route them to %APPDATA%\Lunar\lunar-ui.log and a
# quiet status note instead.
proc lunar::datadir {} {
    if {[info exists ::env(LUNAR_DATA_DIR)] && $::env(LUNAR_DATA_DIR) ne ""} { return $::env(LUNAR_DATA_DIR) }
    if {[info exists ::env(APPDATA)] && $::env(APPDATA) ne ""} { return [file join $::env(APPDATA) Lunar] }
    return [pwd]
}
proc lunar::log {msg} {
    if {$::lunar::log_active} return
    set ::lunar::log_active 1
    catch {
        set dir [lunar::datadir] ; file mkdir $dir
        set fh [open [file join $dir lunar-ui.log] a] ; fconfigure $fh -encoding utf-8
        puts $fh "[clock format [clock seconds] -format {%Y-%m-%d %H:%M:%S}] $msg" ; close $fh
    }
    set ::lunar::log_active 0
}
# Keep the status bar's trust field stable. Short-lived interaction feedback
# uses the system-clock witness field and is replaced by the next render.
proc lunar::status_note {m} { catch { .sb.sys configure -text $m -fg $::lunar::ACCENT } }
proc lunar::bgerror {msg args} {
    if {$::lunar::log_active} return
    set trace $msg
    if {[llength $args]} { catch { set trace [dict get [lindex $args 0] -errorinfo] } }
    catch { lunar::log "\[bgerror\] $trace" }
    catch { lunar::status_note "internal error (logged to lunar-ui.log)" }
}

# ---- look: els's visual identity --------------------------------------------
set ::lunar::PAGE   "#F2F2F2"   ;# calm grey page
set ::lunar::INK    "#1A1A1A"   ;# near-black ink
set ::lunar::ACCENT "#DC322F"   ;# signature red
set ::lunar::MUTED  "#6B7177"   ;# muted slate (chrome text)
set ::lunar::CHROME "#E9E9E9"   ;# flat chrome (status bar)
set ::lunar::HAIR   "#D4D4D4"   ;# hairline separators
set ::lunar::OK     "#2E7D32"   ;# trusted (green)
set ::lunar::WARN   "#B8860B"   ;# holdover/degraded (amber)
set ::lunar::CLOCK_SZ 440       ;# analog face canvas size (square, px)

option add *tearOff 0
font create lunarUI    -family {Segoe UI} -size 9
font create lunarBig   -family Consolas   -size 44
font create lunarDate  -family {Segoe UI} -size 12
font create lunarSmall -family {Segoe UI} -size 9
font create lunarState -family {Segoe UI Semibold} -size 11
font create lunarMono  -family Consolas   -size 9
font create lunarHdr   -family {Segoe UI Semibold} -size 8
font create lunarUIb   -family {Segoe UI} -size 9 -weight bold
font create lunarGear  -family {Segoe UI Symbol} -size 12
set ::lunar::ontop 0

proc lunar::init_style {} {
    set s ttk::style
    catch {$s theme use clam}
    set bg $::lunar::CHROME ; set ink $::lunar::INK ; set hair $::lunar::HAIR
    $s configure . -background $bg -foreground $ink -font lunarUI \
        -borderwidth 0 -focuscolor $bg -troughcolor $::lunar::PAGE \
        -bordercolor $hair -darkcolor $bg -lightcolor $bg
    # entries: flat, page-coloured field, hairline border; focus = a firmer
    # grey, never red (els: red is reserved for the accent). Dialogs sit on
    # PAGE, so give dialog widgets a Page background variant too.
    $s configure TEntry -relief flat -borderwidth 1 -padding {6 4} \
        -fieldbackground $::lunar::PAGE -foreground $ink -insertcolor $ink \
        -bordercolor $hair -lightcolor $hair -darkcolor $hair
    $s map TEntry -bordercolor [list focus "#A6ACB4"] \
        -lightcolor [list focus "#A6ACB4"] -darkcolor [list focus "#A6ACB4"]
    # dialog buttons read as buttons even before hover (els Dialog.TButton)
    $s configure Dialog.TButton -background $::lunar::PAGE -foreground $ink \
        -borderwidth 1 -relief solid -padding {10 5} -anchor center \
        -bordercolor $hair -lightcolor $hair -darkcolor $hair
    $s map Dialog.TButton -background [list pressed $hair active "#DEDEDE"] \
        -foreground [list disabled $::lunar::MUTED]
    # The clock's only persistent control: a quiet, borderless settings icon
    # embedded in the status bar.
    $s configure Status.TButton -background $bg -foreground $::lunar::MUTED \
        -font lunarGear -borderwidth 0 -relief flat -padding {7 1} -anchor center
    $s map Status.TButton -background [list pressed "#CCCCCC" active $hair] \
        -foreground [list active $ink]
    $s configure Settings.TNotebook -background $::lunar::PAGE -borderwidth 0 \
        -tabmargins {0 0 0 0}
    $s configure Settings.TNotebook.Tab -background $bg -foreground $::lunar::MUTED \
        -padding {14 7} -borderwidth 0 -font lunarUI
    $s map Settings.TNotebook.Tab \
        -background [list selected $::lunar::PAGE active "#DEDEDE"] \
        -foreground [list selected $ink]
    # a traditional scrollbar, arrow size in POINTS so it scales per-DPI
    $s configure Vertical.TScrollbar -troughcolor $::lunar::PAGE \
        -background #BCBCBC -arrowcolor #4A4A4A -bordercolor #9A9A9A \
        -relief raised -borderwidth 1 -arrowsize 12p
    $s map Vertical.TScrollbar -background [list active #A4A4A4 disabled $::lunar::PAGE]
}

# ---- state -> (label, colour) ----------------------------------------------
proc lunar::state_display {state synced} {
    switch $state {
        ok          { return [list "TRUSTED"     $::lunar::OK] }
        holdover    { return [list "ESTIMATED"   $::lunar::WARN] }
        reacquiring { return [list "REACQUIRING" $::lunar::WARN] }
        default {
            if {$synced} { return [list "NO SIGNAL" $::lunar::ACCENT] }
            return [list "ACQUIRING…" $::lunar::MUTED]
        }
    }
}

proc lunar::fmt_bound {ms} {
    if {$ms <= 0} { return "" }
    if {$ms < 1000} { return "±${ms} ms" }
    return [format "±%.1f s" [expr {$ms / 1000.0}]]
}

# System-clock deviation, compact: seconds carry no unit (they are the
# obvious default); minutes/hours/days get m/h/d appended directly, no space.
proc lunar::fmt_delta {ms} {
    set s [expr {$ms / 1000.0}]
    set a [expr {abs($s)}]
    if {$a < 60.0}      { return [format "%+.2f" $s] }
    if {$a < 3600.0}    { return [format "%+.1fm" [expr {$s / 60.0}]] }
    if {$a < 86400.0}   { return [format "%+.1fh" [expr {$s / 3600.0}]] }
    return [format "%+.1fd" [expr {$s / 86400.0}]]
}

# ---- settings (same file + format + semantics as the Win32 shell) -----------
# %APPDATA%\Lunar\settings.dat, one key=value per line. Keys this shell does
# not own (chimes/unmin/confirm/tray/startup) are preserved verbatim so the
# two shells can be swapped without losing anything. The PRESENCE of the tz=
# key -- even empty, meaning explicit UTC -- counts as a deliberate choice;
# only then does first-run OS-zone suggestion stop.
set ::lunar::cfg [dict create fmt24 1 tray 0 startup 0 chimes 1 unmin 0 stopms 5000]
set ::lunar::cfg_extra {}       ;# unowned lines (confirm), kept in file order
set ::lunar::tz "UTC"           ;# the active display zone
set ::lunar::tz_chosen 0
set ::lunar::tray_tip_last ""
set ::lunar::armed [lrepeat 12 0]   ;# the 12 five-minute marks (:00..:55)
set ::lunar::prev_min -1            ;# last displayed integer minute (chime edge)
set ::lunar::prewarm_min -1         ;# armed mark we've already pre-warmed for
set ::lunar::stop_hits 0            ;# consecutive reads with bound above the ceiling

# The stop ceiling: boundMs above which the clock stops showing time.
# Clamped to [1 s, 30 s]: below 1 s a healthy clock could trip on a
# transient fault inflate; above 30 s the fan exceeds what the seconds
# dial can encode (±30 s wraps the full circle). Bad input -> default.
proc lunar::clamp_stopms {v} {
    if {![string is integer -strict $v]} { return 5000 }
    if {$v < 1000}  { return 1000 }
    if {$v > 30000} { return 30000 }
    return $v
}

# Stop rule: stop only after two consecutive reads above the ceiling (a
# 500 ms debounce so a single fault-inflated cycle cannot flap the face);
# resume the instant the bound is back under it -- a re-anchor collapses the
# bound to ~200 ms, so recovery is one accepted cycle.
proc lunar::stop_eval {hasTime boundMs} {
    if {!$hasTime || $boundMs <= [dict get $::lunar::cfg stopms]} {
        set ::lunar::stop_hits 0
        return 0
    }
    incr ::lunar::stop_hits
    return [expr {$::lunar::stop_hits >= 2}]
}

proc lunar::settings_load {} {
    set path [file join [lunar::datadir] settings.dat]
    if {[catch {open $path r} fh]} { return }
    set raw [read $fh] ; close $fh
    foreach line [split $raw \n] {
        set line [string trimright $line \r]
        if {$line eq ""} continue
        if {![regexp {^([a-z0-9]+)=(.*)$} $line -> k v]} continue
        switch $k {
            fmt24   { dict set ::lunar::cfg fmt24   [expr {$v ? 1 : 0}] }
            tray    { dict set ::lunar::cfg tray    [expr {$v ? 1 : 0}] }
            startup { dict set ::lunar::cfg startup [expr {$v ? 1 : 0}] }
            chimes  { dict set ::lunar::cfg chimes  [expr {$v ? 1 : 0}] }
            unmin   { dict set ::lunar::cfg unmin   [expr {$v ? 1 : 0}] }
            stopms  { dict set ::lunar::cfg stopms  [lunar::clamp_stopms $v] }
            tz      { set ::lunar::tz_chosen 1 ; dict set ::lunar::cfg tz $v }
            default { lappend ::lunar::cfg_extra $line }
        }
    }
}

proc lunar::settings_save {} {
    set dir [lunar::datadir]
    if {[catch {file mkdir $dir}]} { return }
    set lines $::lunar::cfg_extra
    lappend lines "chimes=[dict get $::lunar::cfg chimes]"
    lappend lines "unmin=[dict get $::lunar::cfg unmin]"
    lappend lines "fmt24=[dict get $::lunar::cfg fmt24]"
    lappend lines "tray=[dict get $::lunar::cfg tray]"
    lappend lines "startup=[dict get $::lunar::cfg startup]"
    lappend lines "stopms=[dict get $::lunar::cfg stopms]"
    lappend lines "tz=[expr {$::lunar::tz eq "UTC" ? "" : $::lunar::tz}]"
    set path [file join $dir settings.dat]
    set tmp  "$path.new"
    if {[catch {
        set fh [open $tmp w] ; fconfigure $fh -translation lf
        puts $fh [join $lines \n] ; close $fh
        file rename -force $tmp $path
    } err]} {
        catch { file delete -force $tmp }
        lunar::log "\[settings\] save failed: $err"
    }
}

# armed.dat: 12 chars '0'/'1', one per five-minute mark (:00..:55). Same file
# and format as the Win32 shell, so the two are interchangeable.
proc lunar::armed_load {} {
    if {[catch {open [file join [lunar::datadir] armed.dat] r} fh]} return
    set raw [read $fh] ; close $fh
    set a {}
    for {set i 0} {$i < 12} {incr i} {
        lappend a [expr {[string index $raw $i] eq "1" ? 1 : 0}]
    }
    set ::lunar::armed $a
}
proc lunar::armed_save {} {
    set dir [lunar::datadir]
    if {[catch {file mkdir $dir}]} return
    set s "" ; foreach v $::lunar::armed { append s [expr {$v ? "1" : "0"}] }
    set path [file join $dir armed.dat] ; set tmp "$path.new"
    if {[catch {
        set fh [open $tmp w] ; fconfigure $fh -translation binary
        puts -nonewline $fh $s ; close $fh
        file rename -force $tmp $path
    }]} { catch {file delete -force $tmp} }
}

# Just-in-time audio pre-warm: ~3 s before an armed 5-minute mark, wake the
# render endpoint so the chime renders into an awake device. Windows parks an
# idle endpoint in D3; the D3->D0 wake would otherwise clip the tone's onset
# (and the amp's power-on pop lands on this silent prime, not on the tone).
# Fires once per approaching mark, and only when a chime will actually fire
# (chimes enabled + bound chime-worthy) -- so the endpoint stays idle the rest
# of the time, unlike a continuous keep-alive stream.
proc lunar::prewarm_check {curMin curSec boundMs} {
    if {$::lunar::prewarm_min == $curMin} { set ::lunar::prewarm_min -1 }  ;# mark reached; re-arm
    # No prime once the bound is at/above the stop ceiling: a stopped clock
    # must not assert time audibly either.
    if {$curSec < 57 || $boundMs >= [dict get $::lunar::cfg stopms]} return
    if {![dict get $::lunar::cfg chimes]} return
    set nextMin [expr {($curMin + 1) % 60}]
    if {$nextMin % 5 != 0} return
    if {![lindex $::lunar::armed [expr {$nextMin / 5}]]} return
    if {$::lunar::prewarm_min == $nextMin} return                          ;# already primed
    if {![llength [info commands ::lunar::prewarm]]} return
    set ::lunar::prewarm_min $nextMin
    catch { ::lunar::prewarm }
}

# Chime edge detector (parity with the Win32 shell): fire once when the
# displayed minute crosses an armed :MM mark, only while the bound is
# chime-worthy (<30 s) and the minute-step is 1-2 (no cascade after a jump).
proc lunar::chime_check {curMin boundMs} {
    set prev $::lunar::prev_min
    set ::lunar::prev_min $curMin
    if {$prev < 0 || $curMin == $prev} return
    set delta [expr {($curMin - $prev + 60) % 60}]
    # Chime-worthy only while the bound is under the stop ceiling: a stopped
    # clock (or one about to stop) must not assert time audibly.
    if {$delta < 1 || $delta > 2 || $boundMs >= [dict get $::lunar::cfg stopms]} return
    set chimes [dict get $::lunar::cfg chimes]
    set unmin  [dict get $::lunar::cfg unmin]
    for {set k 1} {$k <= $delta} {incr k} {
        set mm [expr {($prev + $k) % 60}]
        if {$mm % 5 != 0} continue
        if {![lindex $::lunar::armed [expr {$mm / 5}]]} continue
        if {$chimes && [llength [info commands ::lunar::beep]]} { catch { ::lunar::beep } }
        if {$unmin && [wm state .] ne "normal"} { lunar::restore }
    }
}

# Resolve the display zone at startup: an explicit (valid) choice wins, else
# suggest the OS zone by NAME (not trusting the OS clock), else UTC.
proc lunar::tz_startup {} {
    set have_engine [llength [info commands ::lunar::localtime]]
    if {$::lunar::tz_chosen} {
        set want [dict get $::lunar::cfg tz]
        if {$want eq ""} { set want "UTC" }
        if {!$have_engine || ![catch { ::lunar::localtime 0 $want }]} {
            set ::lunar::tz $want
            return
        }
        lunar::log "\[tz\] configured zone '$want' not in embedded index; using UTC"
        set ::lunar::tz "UTC"
        return
    }
    if {$have_engine && [llength [info commands ::lunar::tz_suggest]]} {
        set sug [::lunar::tz_suggest]
        if {$sug ne "" && ![catch { ::lunar::localtime 0 $sug }]} {
            set ::lunar::tz $sug
            lunar::log "\[tz\] first run: suggesting OS zone $sug"
        }
    }
}

# ---- wall-clock formatting off the embedded tzdata --------------------------
set ::lunar::DAYS   {Sunday Monday Tuesday Wednesday Thursday Friday Saturday}
set ::lunar::MONTHS {January February March April May June July \
                     August September October November December}

proc lunar::fmt_time {lt} {
    set h [dict get $lt hour]
    if {[dict get $::lunar::cfg fmt24]} {
        return [format "%02d:%02d:%02d" $h [dict get $lt minute] [dict get $lt second]]
    }
    set ap [expr {$h >= 12 ? "PM" : "AM"}]
    set h [expr {$h % 12}] ; if {$h == 0} { set h 12 }
    return [format "%d:%02d:%02d %s" $h [dict get $lt minute] [dict get $lt second] $ap]
}
proc lunar::fmt_date {lt} {
    set wd [lindex $::lunar::DAYS   [dict get $lt wday]]
    set mo [lindex $::lunar::MONTHS [expr {[dict get $lt month] - 1}]]
    return "$wd, [format %02d [dict get $lt day]] $mo [dict get $lt year]"
}
proc lunar::fmt_utcoff {offSec} {
    set sign [expr {$offSec < 0 ? "-" : "+"}]
    set a [expr {abs($offSec)}]
    set h [expr {$a / 3600}] ; set m [expr {($a % 3600) / 60}]
    if {$m} { return "UTC$sign$h:[format %02d $m]" }
    return "UTC$sign$h"
}

# ---- analog face ------------------------------------------------------------
# A plain Tk canvas: the ring + tick marks are drawn once (tag "face"); the
# three hands + hub are redrawn each tick (tag "hand") from the disciplined
# wall-clock breakdown. No anti-aliasing library needed -- thick round-capped
# lines read clean on Tk 9.

proc lunar::_hand {c cx cy len ang col w} {
    set a [expr {$ang * 3.14159265358979 / 180.0}]
    $c create line $cx $cy [expr {$cx + $len*sin($a)}] [expr {$cy - $len*cos($a)}] \
        -fill $col -width $w -capstyle round -tags hand
}

proc lunar::armed_mask {} {
    set mask ""
    foreach armed $::lunar::armed { append mask [expr {$armed ? "1" : "0"}] }
    return $mask
}

# The hit band is the same annulus and 24-degree wedge used by the old native
# clock: a click near any five-minute marker toggles its corresponding :MM
# chime.  It deliberately follows the resized face's actual geometry.
proc lunar::clock_marker_hit {c x y} {
    set w [winfo width $c] ; set h [winfo height $c]
    if {$w <= 1 || $h <= 1} { return -1 }
    set size [expr {min($w, $h)}]
    if {$size <= 40} { return -1 }
    set cx [expr {$w / 2.0}] ; set cy [expr {$h / 2.0}]
    set radius [expr {$size * .46}]
    set dx [expr {$x - $cx}] ; set dy [expr {$y - $cy}]
    set r [expr {hypot($dx, $dy)}]
    if {$r < $radius - .075 * $size || $r > $radius + .015 * $size} { return -1 }
    set angle [expr {atan2($dx, -$dy) * 180.0 / acos(-1)}]
    if {$angle < 0} { set angle [expr {$angle + 360.0}] }
    set nearest [expr {round($angle / 30.0)}]
    if {[expr {abs($angle - $nearest * 30.0)}] > 12.0} { return -1 }
    return [expr {int($nearest) % 12}]
}

proc lunar::clock_marker_click {c x y} {
    set index [lunar::clock_marker_hit $c $x $y]
    if {$index < 0} return
    lset ::lunar::armed $index [expr {![lindex $::lunar::armed $index]}]
    # If Settings is open, keep its staged mark in sync so Save cannot
    # overwrite a click made directly on the dial.
    if {[winfo exists .set] && [info exists ::lunar::set_armed($index)]} {
        set ::lunar::set_armed($index) [lindex $::lunar::armed $index]
    }
    lunar::armed_save
    # Native Direct2D and the source-only canvas both update immediately;
    # neither has to wait for the next 200 ms clock tick.
    if {[catch {$c armed [lunar::armed_mask]}]} { lunar::clock_face_static $c }
    set mark [format ":%02d" [expr {$index * 5}]]
    lunar::status_note "[expr {[lindex $::lunar::armed $index] ? "armed" : "disarmed"}] $mark"
}

proc lunar::clock_face_static {c} {
    # A native Direct2D clock receives its actual HWND size directly. This
    # canvas fallback mirrors that geometry for source-only Tk runs.
    set w [winfo width $c] ; set h [winfo height $c]
    if {$w <= 1 || $h <= 1} { set w $::lunar::CLOCK_SZ ; set h $::lunar::CLOCK_SZ }
    set sz [expr {min($w, $h)}]
    set cx [expr {$w/2.0}] ; set cy [expr {$h/2.0}] ; set R [expr {$sz*.46}]
    set PI 3.14159265358979
    $c delete face
    $c create oval [expr {$cx-$R}] [expr {$cy-$R}] [expr {$cx+$R}] [expr {$cy+$R}] \
        -outline $::lunar::HAIR -width 2 -tags face
    set minWidth [expr {max(1, round($sz * .008))}]
    for {set i 0} {$i < 60} {incr i} {
        if {$i % 5 == 0} continue
        set a [expr {$i * 6 * $PI / 180.0}]
        set r1 [expr {$R - .020 * $sz}]
        $c create line [expr {$cx+$r1*sin($a)}] [expr {$cy-$r1*cos($a)}] \
                       [expr {$cx+$R*sin($a)}]  [expr {$cy-$R*cos($a)}] \
                       -fill $::lunar::MUTED -width $minWidth -capstyle round -tags face
    }
    set hourInner [expr {$R - .050 * $sz}]
    set hourWidth [expr {max(3, round($sz * .014))}]
    for {set hour 1} {$hour < 12} {incr hour} {
        set a [expr {$hour * 30 * $PI / 180.0}]
        set col [expr {[lindex $::lunar::armed $hour] ? $::lunar::ACCENT : $::lunar::INK}]
        $c create line [expr {$cx+$hourInner*sin($a)}] [expr {$cy-$hourInner*cos($a)}] \
                       [expr {$cx+$R*sin($a)}]         [expr {$cy-$R*cos($a)}] \
                       -fill $col -width $hourWidth -capstyle round -tags face
    }
    set col [expr {[lindex $::lunar::armed 0] ? $::lunar::ACCENT : $::lunar::INK}]
    set offset [expr {$sz * .020}]
    $c create line [expr {$cx-$offset}] [expr {$cy-$hourInner}] \
                   [expr {$cx-$offset}] [expr {$cy-$R}] \
                   -fill $col -width $hourWidth -capstyle round -tags face
    $c create line [expr {$cx+$offset}] [expr {$cy-$hourInner}] \
                   [expr {$cx+$offset}] [expr {$cy-$R}] \
                   -fill $col -width $hourWidth -capstyle round -tags face
    # A resize redraws the static face but must not cover the live hands.
    $c lower face
}

# Draw the hands from a localtime dict, mirroring the native widget's policy.
# The display has exactly TWO states: the time -- with the uncertainty carried
# entirely by the second hand, an UNCERTAINTY FAN (a pie sector as wide as the
# error bound, ±boundMs -> half-angle boundMs/1000 × 6°, around a hairline
# best-estimate centerline, always the signature red) -- or no time at all
# (the caller clears the hands). Tk canvas has no alpha, so the fan is a solid
# tint lowered BENEATH the face ticks; the ticks stay legible across it.
proc lunar::clock_hands {c lt milliseconds boundMs} {
    set w [winfo width $c] ; set h [winfo height $c]
    if {$w <= 1 || $h <= 1} { set w $::lunar::CLOCK_SZ ; set h $::lunar::CLOCK_SZ }
    set sz [expr {min($w, $h)}]
    set cx [expr {$w/2.0}] ; set cy [expr {$h/2.0}]
    $c delete hand
    set h [dict get $lt hour] ; set m [dict get $lt minute]
    set s [expr {[dict get $lt second] + $milliseconds / 1000.0}]
    if {$boundMs > 0} {
        set r [expr {$sz*.44}]
        set half [expr {double($boundMs) / 1000.0 * 6.0}]
        if {$half >= 180.0} {
            set id [$c create oval [expr {$cx-$r}] [expr {$cy-$r}] \
                        [expr {$cx+$r}] [expr {$cy+$r}] \
                        -fill "#EECCCB" -outline "" -tags hand]
        } else {
            set secAng [expr {$s/60.0*360}]
            set id [$c create arc [expr {$cx-$r}] [expr {$cy-$r}] \
                        [expr {$cx+$r}] [expr {$cy+$r}] \
                        -start [expr {90.0 - $secAng - $half}] \
                        -extent [expr {2.0*$half}] \
                        -style pieslice -fill "#EECCCB" -outline "" -tags hand]
        }
        $c lower $id     ;# beneath the face ring/ticks
    }
    lunar::_hand $c $cx $cy [expr {$sz*.28}] [expr {(($h%12)+$m/60.0)/12.0*360}] $::lunar::INK 9
    lunar::_hand $c $cx $cy [expr {$sz*.40}] [expr {($m+$s/60.0)/60.0*360}]      $::lunar::INK 5
    lunar::_hand $c $cx $cy [expr {$sz*.44}] [expr {$s/60.0*360}] $::lunar::ACCENT 2
    $c create oval [expr {$cx-6}] [expr {$cy-6}] [expr {$cx+6}] [expr {$cy+6}] \
        -fill $::lunar::INK -outline "" -tags hand
}

# Prefer the native Direct2D clock widget when packaged with Lunar.  The
# source-only Tk iteration path has no C extension, so it deliberately falls
# back to the canvas implementation above.
proc lunar::clock_display {lt milliseconds state hasTime synced boundMs stopped} {
    if {[llength [info commands .face.clock]]} {
        if {$hasTime && $lt ne ""} {
            if {![catch {
                .face.clock show [dict get $lt hour] [dict get $lt minute] \
                    [dict get $lt second] $milliseconds 1 $state $synced \
                    $boundMs $stopped [lunar::armed_mask]
            } err]} { return }
            lunar::log "Direct2D clock update failed: $err"
        } elseif {![catch {
            .face.clock show 0 0 0 0 0 $state $synced 0 0 [lunar::armed_mask]
        } err]} {
            return
        } else {
            lunar::log "Direct2D clock update failed: $err"
        }
    }
    if {$hasTime && $lt ne "" && !$stopped} {
        lunar::clock_hands .face.clock $lt $milliseconds $boundMs
    } else {
        # The other of the two display states: no time shown at all.
        catch { .face.clock delete hand }
    }
}

# Windows ignores Tk's `wm aspect` request.  Keep the resize in Tk's own
# geometry manager, then compensate for the measured title-frame insets so
# the visible outer window remains square without disturbing native children.
proc lunar::square_finish {} {
    set ::lunar::square_fixing 0
    set ::lunar::square_last_w [winfo width .]
    set ::lunar::square_last_h [winfo height .]
}

proc lunar::square_apply {width height} {
    lassign [wm minsize .] minW minH
    if {$width < $minW} { set width $minW }
    if {$height < $minH} { set height $minH }
    set ::lunar::square_fixing 1
    wm geometry . "${width}x${height}"
    after idle lunar::square_finish
}

proc lunar::square_correct {} {
    set ::lunar::square_pending 0
    if {$::lunar::square_fixing || $::lunar::square_dragging} return
    if {[wm state .] eq "zoomed" && $::lunar::square_work_w > 0 && $::lunar::square_work_h > 0} {
        set side [expr {min($::lunar::square_work_w, $::lunar::square_work_h)}]
        set width [expr {$side - $::lunar::square_extra_w}]
        set height [expr {$side - $::lunar::square_extra_h}]
        set x [expr {$::lunar::square_work_x + ($::lunar::square_work_w - $side) / 2}]
        set y [expr {$::lunar::square_work_y + ($::lunar::square_work_h - $side) / 2}]
        set ::lunar::square_fixing 1
        wm state . normal
        wm geometry . "${width}x${height}+$x+$y"
        after idle lunar::square_finish
        return
    }
    set w [winfo width .] ; set h [winfo height .]
    if {$::lunar::square_last_w <= 0 || $::lunar::square_last_h <= 0} {
        set ::lunar::square_last_w $w ; set ::lunar::square_last_h $h
        return
    }
    set outerW [expr {$w + $::lunar::square_extra_w}]
    set outerH [expr {$h + $::lunar::square_extra_h}]
    if {[expr {abs($outerW - $outerH)}] <= 1} {
        set ::lunar::square_last_w $w ; set ::lunar::square_last_h $h
        return
    }
    set dw [expr {abs($w - $::lunar::square_last_w)}]
    set dh [expr {abs($h - $::lunar::square_last_h)}]
    if {$dw >= $dh} {
        set targetW $w
        set targetH [expr {$outerW - $::lunar::square_extra_h}]
    } else {
        set targetH $h
        set targetW [expr {$outerH - $::lunar::square_extra_w}]
    }
    lassign [wm minsize .] minW minH
    if {$targetH < $minH} {
        set targetH $minH
        set targetW [expr {$targetH + $::lunar::square_extra_h - $::lunar::square_extra_w}]
    }
    if {$targetW < $minW} {
        set targetW $minW
        set targetH [expr {$targetW + $::lunar::square_extra_w - $::lunar::square_extra_h}]
    }
    lunar::square_apply $targetW $targetH
}

proc lunar::square_configure {} {
    if {$::lunar::square_fixing || $::lunar::square_dragging || $::lunar::square_pending} return
    set ::lunar::square_pending 1
    after idle lunar::square_correct
}

proc lunar::square_resize_start {} { set ::lunar::square_dragging 1 }
proc lunar::square_resize_end {} {
    set ::lunar::square_dragging 0
    after idle lunar::square_correct
}

proc lunar::force_square_window {} {
    if {[llength [info commands ::lunar::frame_metrics]]} {
        if {[catch {
            set metrics [::lunar::frame_metrics [lunar::hwnd]]
            set ::lunar::square_extra_w [dict get $metrics extraWidth]
            set ::lunar::square_extra_h [dict get $metrics extraHeight]
            set ::lunar::square_work_x [dict get $metrics workX]
            set ::lunar::square_work_y [dict get $metrics workY]
            set ::lunar::square_work_w [dict get $metrics workWidth]
            set ::lunar::square_work_h [dict get $metrics workHeight]
            ::lunar::watch_resize [lunar::hwnd]
            set w [winfo width .] ; set h [winfo height .]
            set side [expr {max($w + $::lunar::square_extra_w,
                                $h + $::lunar::square_extra_h)}]
            set ::lunar::square_last_w $w
            set ::lunar::square_last_h $h
            bind . <Configure> { lunar::square_configure }
            lunar::square_apply [expr {$side - $::lunar::square_extra_w}] \
                                [expr {$side - $::lunar::square_extra_h}]
        } err]} {
            lunar::log "square-window setup failed: $err"
        }
    } else {
        catch { wm aspect . 1 1 1 1 }
    }
}

# ---- the dashboard ----------------------------------------------------------
proc lunar::build {} {
    wm title . "Lunar $::lunar::version"
    # force_square_window below adjusts Tk's own client geometry so that the
    # actual Windows frame is square, including status/title chrome.
    wm geometry . 510x510
    wm minsize . 396 396
    wm resizable . 1 1
    lunar::init_style
    . configure -background $::lunar::PAGE
    catch { wm iconphoto . -default [image create photo -file [file join [file dirname [info script]] resources icon.png]] }

    # Keep the power-user shortcuts, but leave the clock itself free of a
    # desktop-style menu bar. All discoverable controls live behind the
    # settings icon in the status bar.
    bind . <Control-r>     { catch { ::lunar::syncnow } ; break }
    bind . <Control-c>     { lunar::copy_time ; break }
    bind . <Control-comma> { lunar::settings_dlg ; break }

    set P $::lunar::PAGE
    frame .face -bg $P
    if {[llength [info commands ::lunar::clock]]} {
        ::lunar::clock .face.clock -width $::lunar::CLOCK_SZ -height $::lunar::CLOCK_SZ
    } else {
        canvas .face.clock -width $::lunar::CLOCK_SZ -height $::lunar::CLOCK_SZ \
            -bg $P -highlightthickness 0
        lunar::clock_face_static .face.clock
        bind .face.clock <Configure> { after idle [list lunar::clock_face_static %W] }
    }
    pack .face.clock -fill both -expand 1 -padx 16 -pady 16
    bind .face.clock <ButtonRelease-1> { lunar::clock_marker_click %W %x %y }

    # Stable left-to-right status summary: trust, uncertainty, SYS witness,
    # then the selected display zone. The face can be read at a glance; this
    # row answers the audit questions without repeating decorative UI.
    frame .sb -bg $::lunar::CHROME
    frame .sb.hair -height 1 -bg $::lunar::HAIR
    label .sb.trust -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUIb \
        -anchor w -text ""
    label .sb.bound -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUI \
        -anchor w -text ""
    label .sb.sys   -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUI \
        -anchor w -text ""
    label .sb.zone  -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUI \
        -anchor e -text ""
    ttk::button .sb.settings -style Status.TButton -text "⚙" -width 2 \
        -takefocus 1 -command lunar::settings_dlg
    foreach n {1 2 3} {
        label .sb.sep$n -bg $::lunar::CHROME -fg $::lunar::HAIR -font lunarUI \
            -text "·"
    }
    # The stretch lives in an EMPTY spacer column (6), never in a text cell:
    # every text cell keeps its natural width, so a width shortfall can never
    # clip the zone mid-glyph into a fake-looking code ("JTC+2"). Render hides
    # redundant cells (and their dots) so the row fits with room to spare.
    grid .sb.hair  -row 0 -column 0 -columnspan 9 -sticky ew
    grid .sb.trust -row 1 -column 0 -sticky w  -padx {12 6} -pady {6 7}
    grid .sb.sep1  -row 1 -column 1 -sticky w  -padx {0 6}  -pady {6 7}
    grid .sb.bound -row 1 -column 2 -sticky w  -padx {0 6}  -pady {6 7}
    grid .sb.sep2  -row 1 -column 3 -sticky w  -padx {0 6}  -pady {6 7}
    grid .sb.sys   -row 1 -column 4 -sticky w  -padx {0 6}  -pady {6 7}
    grid .sb.sep3  -row 1 -column 6 -sticky w  -padx {0 6}  -pady {6 7}
    grid .sb.zone  -row 1 -column 7 -sticky e  -padx {0 6} -pady {6 7}
    grid .sb.settings -row 1 -column 8 -sticky e -padx {0 6} -pady {3 4}
    # The stretch (col 5) sits BEFORE the zone's separator dot, so the dot
    # travels with the zone instead of dangling after the left-hand cells.
    grid columnconfigure .sb 5 -weight 1

    pack .sb   -side bottom -fill x
    pack .face -fill both -expand 1

    wm protocol . WM_DELETE_WINDOW lunar::quit
    lunar::tray_setup
    # Let Tk map and decorate the top-level before measuring its frame insets.
    after 50 lunar::force_square_window
}

# ---- settings actions: copy time, always-on-top, log viewer -----------------
proc lunar::apply_ontop {} { catch { wm attributes . -topmost $::lunar::ontop } }

proc lunar::copy_time {} {
    if {![llength [info commands ::lunar::status]]} { return 0 }
    set st [::lunar::status]
    if {![dict get $st hasTime]} { lunar::status_note "no trusted time yet" ; return 0 }
    if {[catch { ::lunar::localtime [dict get $st utcMs] $::lunar::tz } lt]} { return 0 }
    set off [dict get $lt offSec]
    set sign [expr {$off < 0 ? "-" : "+"}] ; set a [expr {abs($off)}]
    set iso [format "%04d-%02d-%02dT%02d:%02d:%02d%s%02d:%02d" \
        [dict get $lt year] [dict get $lt month] [dict get $lt day] \
        [dict get $lt hour] [dict get $lt minute] [dict get $lt second] \
        $sign [expr {$a / 3600}] [expr {($a % 3600) / 60}]]
    clipboard clear ; clipboard append $iso
    lunar::status_note "copied $iso"
    return 1
}

proc lunar::log_dlg {} {
    if {[winfo exists .log]} { raise .log ; focus .log ; lunar::log_refresh ; return }
    set P $::lunar::PAGE
    toplevel .log -bg $P
    wm title .log "Lunar — Event Log" ; wm geometry .log 760x480 ; wm transient .log .
    frame .log.f -bg $P ; pack .log.f -fill both -expand 1 -padx 10 -pady 10
    text .log.f.t -bg $P -fg $::lunar::INK -font lunarMono -wrap none \
        -borderwidth 1 -relief solid -highlightthickness 0 -padx 8 -pady 6 \
        -yscrollcommand {.log.f.vs set} -xscrollcommand {.log.f.hs set}
    ttk::scrollbar .log.f.vs -orient vertical   -command {.log.f.t yview}
    ttk::scrollbar .log.f.hs -orient horizontal -command {.log.f.t xview}
    grid .log.f.t  -row 0 -column 0 -sticky nsew
    grid .log.f.vs -row 0 -column 1 -sticky ns
    grid .log.f.hs -row 1 -column 0 -sticky ew
    grid rowconfigure    .log.f 0 -weight 1
    grid columnconfigure .log.f 0 -weight 1
    frame .log.b -bg $P ; pack .log.b -fill x -padx 10 -pady {0 10}
    ttk::button .log.b.copy  -style Dialog.TButton -text "Copy all" -command lunar::log_copy
    ttk::button .log.b.close -style Dialog.TButton -text "Close"    -command {destroy .log}
    pack .log.b.close -side right
    pack .log.b.copy  -side right -padx {0 8}
    bind .log <Escape> {destroy .log}
    lunar::log_refresh
    lunar::log_refresh_loop
}
proc lunar::log_refresh {} {
    if {![winfo exists .log.f.t] || ![llength [info commands ::lunar::log_text]]} return
    set atbottom [expr {[lindex [.log.f.t yview] 1] > 0.999}]
    .log.f.t configure -state normal
    .log.f.t delete 1.0 end
    .log.f.t insert end [::lunar::log_text]
    .log.f.t configure -state disabled
    if {$atbottom} { .log.f.t see end }
}
proc lunar::log_refresh_loop {} {
    if {![winfo exists .log]} return
    lunar::log_refresh
    after 1000 lunar::log_refresh_loop
}
proc lunar::log_copy {} {
    if {![llength [info commands ::lunar::log_text]]} return
    clipboard clear ; clipboard append [::lunar::log_text]
    lunar::status_note "log copied to clipboard"
}

# ---- Settings dialog ---------------------------------------------------------
# The status-bar gear is the clock's one persistent control. Everything that
# used to live in the desktop-style menu is gathered here. Preferences are
# staged until Save; utility actions act immediately and report inline.
proc lunar::settings_dlg {{tab ""}} {
    if {[wm state .] in {withdrawn iconic}} { lunar::restore }
    if {[winfo exists .set]} {
        if {$tab ne ""} { catch { .set.shell.tabs select .set.shell.tabs.$tab } }
        wm deiconify .set
        raise .set
        focus .set
        return
    }

    # A settings window stages every preference until Save. Remove a trace left
    # by an interrupted development run before initializing the staged values.
    catch { trace remove variable ::lunar::set_filter write ::lunar::settings_filter_changed }
    set ::lunar::set_filter  ""
    set ::lunar::set_tz      $::lunar::tz
    set ::lunar::set_fmt24   [dict get $::lunar::cfg fmt24]
    set ::lunar::set_tray    [dict get $::lunar::cfg tray]
    set ::lunar::set_startup [dict get $::lunar::cfg startup]
    set ::lunar::set_chimes  [dict get $::lunar::cfg chimes]
    set ::lunar::set_unmin   [dict get $::lunar::cfg unmin]
    set ::lunar::set_ontop   $::lunar::ontop
    set ::lunar::set_stopsec [expr {[dict get $::lunar::cfg stopms] / 1000}]
    for {set i 0} {$i < 12} {incr i} {
        set ::lunar::set_armed($i) [lindex $::lunar::armed $i]
    }

    set P $::lunar::PAGE
    set I $::lunar::INK
    set M $::lunar::MUTED
    toplevel .set -bg $P
    wm withdraw .set
    wm title .set "Lunar Settings"
    wm transient .set .
    wm resizable .set 0 0

    frame .set.shell -bg $P
    pack .set.shell -fill both -expand 1

    ttk::notebook .set.shell.tabs -style Settings.TNotebook
    foreach {name title} {clock Clock chimes Chimes app Application} {
        frame .set.shell.tabs.$name -bg $P
        frame .set.shell.tabs.$name.inner -bg $P
        pack .set.shell.tabs.$name.inner -fill both -expand 1 -padx 24 -pady 18
        .set.shell.tabs add .set.shell.tabs.$name -text $title
    }
    pack .set.shell.tabs -fill both -expand 1 -pady {10 0}

    # Clock -------------------------------------------------------------------
    set c .set.shell.tabs.clock.inner
    label $c.zhdr -bg $P -fg $I -font lunarUIb -anchor w -text "Display time zone"
    label $c.zhelp -bg $P -fg $M -font lunarUI -anchor w \
        -text "Search the embedded IANA time-zone database."
    ttk::entry $c.filter -font lunarUI -width 55 -textvariable ::lunar::set_filter
    frame $c.zl -bg $P
    listbox $c.zl.list -height 8 -width 55 -font lunarUI -bg $P -fg $I \
        -selectbackground "#D6E2F2" -selectforeground $I \
        -borderwidth 1 -relief solid -highlightthickness 0 -exportselection 0 \
        -yscrollcommand [list $c.zl.vs set]
    ttk::scrollbar $c.zl.vs -orient vertical -command [list $c.zl.list yview]
    pack $c.zl.vs -side right -fill y
    pack $c.zl.list -side left -fill both -expand 1
    label $c.zprev -bg $P -fg $M -font lunarUI -anchor w -text ""
    checkbutton $c.fmt24 -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P -command lunar::settings_preview \
        -text "Use 24-hour time" -variable ::lunar::set_fmt24
    frame $c.stop -bg $P
    label $c.stop.l -bg $P -fg $I -font lunarUI -anchor w \
        -text "Stop the clock when uncertainty exceeds"
    ttk::spinbox $c.stop.n -from 1 -to 30 -increment 1 -width 4 -font lunarUI \
        -textvariable ::lunar::set_stopsec
    label $c.stop.u -bg $P -fg $M -font lunarUI -text "seconds"
    pack $c.stop.l -side left
    pack $c.stop.n -side left -padx {8 4}
    pack $c.stop.u -side left
    frame $c.rule -bg $::lunar::HAIR -height 1
    label $c.ahdr -bg $P -fg $I -font lunarUIb -anchor w -text "Clock actions"
    frame $c.actions -bg $P
    ttk::button $c.actions.sync -style Dialog.TButton -text "Sync now" \
        -command lunar::settings_sync_now
    ttk::button $c.actions.copy -style Dialog.TButton -text "Copy current time" \
        -command lunar::settings_copy_time
    pack $c.actions.sync -side left
    pack $c.actions.copy -side left -padx {8 0}
    label $c.actionnote -bg $P -fg $M -font lunarSmall -anchor w -text ""

    pack $c.zhdr -fill x
    pack $c.zhelp -fill x -pady {2 9}
    pack $c.filter -fill x -pady {0 7}
    pack $c.zl -fill x
    pack $c.zprev -fill x -pady {6 0}
    pack $c.fmt24 -fill x -pady {12 0}
    pack $c.stop -fill x -pady {8 0}
    pack $c.rule -fill x -pady {16 14}
    pack $c.ahdr -fill x -pady {0 8}
    pack $c.actions -fill x
    pack $c.actionnote -fill x -pady {6 0}

    # Chimes ------------------------------------------------------------------
    set c .set.shell.tabs.chimes.inner
    label $c.hdr -bg $P -fg $I -font lunarUIb -anchor w -text "Chimes"
    label $c.help -bg $P -fg $M -font lunarUI -anchor w -justify left \
        -text "Choose which five-minute marks should sound. You can also toggle\na chime by clicking its marker directly on the clock dial."
    checkbutton $c.enabled -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Play a chime on armed marks" -variable ::lunar::set_chimes
    checkbutton $c.unmin -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Restore the clock window on armed marks" -variable ::lunar::set_unmin
    frame $c.rule1 -bg $::lunar::HAIR -height 1
    label $c.mhdr -bg $P -fg $I -font lunarUIb -anchor w -text "Armed five-minute marks"
    frame $c.marks -bg $P
    for {set i 0} {$i < 12} {incr i} {
        checkbutton $c.marks.m$i -bg $P -fg $I -font lunarMono \
            -activebackground $P -selectcolor $P -padx 3 \
            -text [format ":%02d" [expr {$i * 5}]] -variable ::lunar::set_armed($i)
        grid $c.marks.m$i -row [expr {$i / 6}] -column [expr {$i % 6}] \
            -sticky w -padx {0 15} -pady 2
    }
    frame $c.rule2 -bg $::lunar::HAIR -height 1
    label $c.thdr -bg $P -fg $I -font lunarUIb -anchor w -text "Sound check"
    frame $c.actions -bg $P
    ttk::button $c.actions.test -style Dialog.TButton -text "Test chime" \
        -command lunar::settings_test_chime
    pack $c.actions.test -side left
    label $c.actionnote -bg $P -fg $M -font lunarSmall -anchor w -text ""

    pack $c.hdr -fill x
    pack $c.help -fill x -pady {2 12}
    pack $c.enabled -fill x
    pack $c.unmin -fill x -pady {3 0}
    pack $c.rule1 -fill x -pady {16 14}
    pack $c.mhdr -fill x -pady {0 7}
    pack $c.marks -fill x
    pack $c.rule2 -fill x -pady {16 14}
    pack $c.thdr -fill x -pady {0 8}
    pack $c.actions -fill x
    pack $c.actionnote -fill x -pady {6 0}

    # Application -------------------------------------------------------------
    set c .set.shell.tabs.app.inner
    label $c.bhdr -bg $P -fg $I -font lunarUIb -anchor w -text "Window and startup"
    checkbutton $c.ontop -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Keep Lunar above other windows" -variable ::lunar::set_ontop
    checkbutton $c.tray -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Minimize to the notification area" -variable ::lunar::set_tray
    checkbutton $c.startup -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Start Lunar when I sign in" -variable ::lunar::set_startup
    frame $c.rule1 -bg $::lunar::HAIR -height 1
    label $c.uhdr -bg $P -fg $I -font lunarUIb -anchor w -text "Diagnostics"
    frame $c.actions -bg $P
    ttk::button $c.actions.log -style Dialog.TButton -text "Open event log" \
        -command lunar::settings_open_log
    pack $c.actions.log -side left
    label $c.actionnote -bg $P -fg $M -font lunarSmall -anchor w -text ""
    frame $c.rule2 -bg $::lunar::HAIR -height 1

    set about [dict create version $::lunar::version tzdata unknown]
    if {[llength [info commands ::lunar::about]]} { catch { set about [::lunar::about] } }
    label $c.abouttitle -bg $P -fg $I -font lunarUIb -anchor w \
        -text "Lunar [dict get $about version]"
    label $c.aboutbody -bg $P -fg $M -font lunarUI -anchor w -justify left \
        -text "An analog clock disciplined by authenticated network time.\nEmbedded IANA time-zone data: [dict get $about tzdata]  ·  MIT License"
    ttk::button $c.quit -style Dialog.TButton -text "Quit Lunar" -command lunar::quit

    pack $c.bhdr -fill x -pady {0 7}
    pack $c.ontop -fill x
    pack $c.tray -fill x -pady {3 0}
    pack $c.startup -fill x -pady {3 0}
    pack $c.rule1 -fill x -pady {16 14}
    pack $c.uhdr -fill x -pady {0 8}
    pack $c.actions -fill x
    pack $c.actionnote -fill x -pady {6 0}
    pack $c.rule2 -fill x -pady {16 14}
    pack $c.abouttitle -fill x
    pack $c.aboutbody -fill x -pady {3 12}
    pack $c.quit -anchor w

    # Shared footer -----------------------------------------------------------
    frame .set.shell.footrule -bg $::lunar::HAIR -height 1
    frame .set.shell.foot -bg $P
    ttk::button .set.shell.foot.save -style Dialog.TButton -text "Save" \
        -command lunar::settings_ok
    ttk::button .set.shell.foot.cancel -style Dialog.TButton -text "Cancel" \
        -command lunar::settings_close
    pack .set.shell.foot.cancel -side right
    pack .set.shell.foot.save -side right -padx {0 8}
    pack .set.shell.footrule -fill x
    pack .set.shell.foot -fill x -padx 24 -pady 14

    trace add variable ::lunar::set_filter write ::lunar::settings_filter_changed
    bind .set.shell.tabs.clock.inner.zl.list <<ListboxSelect>> lunar::settings_zone_selected
    bind .set <Escape> { lunar::settings_close }
    bind .set <Destroy> { if {%W eq ".set"} { lunar::settings_cleanup } }
    wm protocol .set WM_DELETE_WINDOW lunar::settings_close
    lunar::settings_fill
    if {$tab ne ""} { catch { .set.shell.tabs select .set.shell.tabs.$tab } }

    update idletasks
    set x [expr {[winfo rootx .] + ([winfo width .]  - [winfo reqwidth .set]) / 2}]
    set y [expr {[winfo rooty .] + ([winfo height .] - [winfo reqheight .set]) / 3}]
    wm geometry .set +$x+$y
    wm deiconify .set
    raise .set
    switch $tab {
        chimes { focus .set.shell.tabs.chimes.inner.enabled }
        app    { focus .set.shell.tabs.app.inner.ontop }
        default { focus .set.shell.tabs.clock.inner.filter }
    }
    lunar::settings_preview_loop
}

proc lunar::settings_cleanup {} {
    if {$::lunar::settings_preview_after ne ""} {
        catch { after cancel $::lunar::settings_preview_after }
        set ::lunar::settings_preview_after ""
    }
    catch { trace remove variable ::lunar::set_filter write ::lunar::settings_filter_changed }
}

proc lunar::settings_close {} {
    lunar::settings_cleanup
    catch { destroy .set }
}

proc lunar::settings_filter_changed {args} { lunar::settings_fill }

# Repopulate the zone list from the filter while retaining the staged zone.
proc lunar::settings_fill {} {
    set lb .set.shell.tabs.clock.inner.zl.list
    if {![winfo exists $lb]} return
    set pat [string tolower [string trim $::lunar::set_filter]]
    $lb delete 0 end
    set sel -1
    set i 0
    set zones [expr {[llength [info commands ::lunar::tz_list]] ? [::lunar::tz_list] : {UTC}}]
    foreach z $zones {
        if {$pat ne "" && ![string match "*$pat*" [string tolower $z]]} continue
        $lb insert end $z
        if {$z eq $::lunar::set_tz} { set sel $i }
        incr i
    }
    if {$sel >= 0} { $lb selection set $sel ; $lb see $sel }
    lunar::settings_preview
}

proc lunar::settings_zone_selected {} {
    set lb .set.shell.tabs.clock.inner.zl.list
    if {![winfo exists $lb]} return
    set s [$lb curselection]
    if {$s ne ""} { set ::lunar::set_tz [$lb get [lindex $s 0]] }
    lunar::settings_preview
}

proc lunar::settings_sel {} {
    if {[info exists ::lunar::set_tz]} { return $::lunar::set_tz }
    return ""
}

proc lunar::settings_preview {} {
    set pv .set.shell.tabs.clock.inner.zprev
    if {![winfo exists $pv]} return
    set z [lunar::settings_sel]
    if {$z eq ""} { $pv configure -text "" ; return }
    set txt "Selected: $z  ·  Current time unavailable"
    if {[llength [info commands ::lunar::status]]} {
        set st [::lunar::status]
        if {[dict get $st hasTime] &&
            ![catch { ::lunar::localtime [dict get $st utcMs] $z } lt]} {
            set h [dict get $lt hour]
            if {$::lunar::set_fmt24} {
                set tm [format "%02d:%02d:%02d" $h [dict get $lt minute] [dict get $lt second]]
            } else {
                set ap [expr {$h >= 12 ? "PM" : "AM"}]
                set h [expr {$h % 12}] ; if {$h == 0} { set h 12 }
                set tm [format "%d:%02d:%02d %s" $h [dict get $lt minute] [dict get $lt second] $ap]
            }
            set txt "Selected: $z  ·  $tm [dict get $lt abbr] ([lunar::fmt_utcoff [dict get $lt offSec]])"
        }
    }
    $pv configure -text $txt
}

proc lunar::settings_preview_loop {} {
    set ::lunar::settings_preview_after ""
    if {![winfo exists .set]} return
    lunar::settings_preview
    set ::lunar::settings_preview_after [after 500 lunar::settings_preview_loop]
}

proc lunar::settings_note {page text} {
    set w .set.shell.tabs.$page.inner.actionnote
    if {[winfo exists $w]} { $w configure -text $text }
}

proc lunar::settings_sync_now {} {
    if {![llength [info commands ::lunar::syncnow]]} {
        lunar::settings_note clock "Sync is unavailable in this build."
        return
    }
    if {[catch { ::lunar::syncnow } err]} {
        lunar::settings_note clock "Sync could not be started."
        lunar::log "\[settings sync\] $err"
    } else {
        lunar::settings_note clock "Sync requested."
    }
}

proc lunar::settings_copy_time {} {
    if {[lunar::copy_time]} {
        lunar::settings_note clock "Current time copied to the clipboard."
    } else {
        lunar::settings_note clock "Current time is not available yet."
    }
}

proc lunar::settings_test_chime {} {
    if {![llength [info commands ::lunar::beep]] || [catch { ::lunar::beep } err]} {
        lunar::settings_note chimes "The chime could not be played."
        if {[info exists err] && $err ne ""} { lunar::log "\[settings chime\] $err" }
    } else {
        lunar::settings_note chimes "Chime played."
    }
}

proc lunar::settings_open_log {} {
    lunar::log_dlg
    lunar::settings_note app "Event log opened."
}

proc lunar::settings_ok {} {
    # Validate the one external setting first. A registry failure leaves every
    # staged preference untouched and keeps the relevant page visible.
    set startup [expr {$::lunar::set_startup ? 1 : 0}]
    if {[llength [info commands ::lunar::run_at_startup]]} {
        if {[catch { ::lunar::run_at_startup } current] ||
            ($current != $startup && [catch { ::lunar::run_at_startup $startup } regerr]) ||
            [catch { ::lunar::run_at_startup } startup] || $startup != $::lunar::set_startup} {
            .set.shell.tabs select .set.shell.tabs.app
            focus .set.shell.tabs.app.inner.startup
            lunar::settings_note app "Could not update the sign-in startup setting."
            if {[info exists regerr] && $regerr ne ""} { lunar::log "\[settings startup\] $regerr" }
            return
        }
    }

    set z [lunar::settings_sel]
    if {$z ne ""} { set ::lunar::tz $z }
    set ::lunar::tz_chosen 1
    dict set ::lunar::cfg tz [expr {$::lunar::tz eq "UTC" ? "" : $::lunar::tz}]
    dict set ::lunar::cfg fmt24  [expr {$::lunar::set_fmt24 ? 1 : 0}]
    dict set ::lunar::cfg tray   [expr {$::lunar::set_tray ? 1 : 0}]
    dict set ::lunar::cfg chimes [expr {$::lunar::set_chimes ? 1 : 0}]
    dict set ::lunar::cfg unmin  [expr {$::lunar::set_unmin ? 1 : 0}]
    if {[string is integer -strict $::lunar::set_stopsec]} {
        dict set ::lunar::cfg stopms [lunar::clamp_stopms [expr {$::lunar::set_stopsec * 1000}]]
    }
    set ::lunar::ontop [expr {$::lunar::set_ontop ? 1 : 0}]
    lunar::apply_ontop
    set a {}
    for {set i 0} {$i < 12} {incr i} { lappend a [expr {$::lunar::set_armed($i) ? 1 : 0}] }
    set ::lunar::armed $a
    lunar::armed_save
    dict set ::lunar::cfg startup $startup
    lunar::settings_save
    lunar::settings_close
}

# nudge the engine to re-sync periodically (defined at top level, not inside
# main: a `proc lunar::x` created from within a ::lunar-namespace proc would
# resolve to ::lunar::lunar::x and fail).
proc lunar::repoll {} {
    set err [catch { ::lunar::syncnow }]
    set next [lunar::next_poll_ms $err]
    set ::lunar::poll_cur $next
    set ::lunar::repoll_after [after $next lunar::repoll]
}

# Recover as fast as possible: cancel the pending scheduled poll and fire
# one now. Called on the stop edge (interval crossed the ceiling) and on
# the time-vanishing edge (hasTime 1 -> 0), so recovery never waits out a
# relaxed interval. Rate-limited to one forced poll per FAST floor; the
# engine's g_running CAS already dedups an in-flight cycle.
proc lunar::poll_now {} {
    set now [clock milliseconds]
    if {$now - $::lunar::poll_forced_at < $::lunar::poll_min} return
    set ::lunar::poll_forced_at $now
    catch { after cancel $::lunar::repoll_after }
    set ::lunar::repoll_after [after 0 lunar::repoll]
}

# Adaptive poll cadence (this is the ENTIRE scheduler; the C engine polls only
# when syncnow calls it). Sync FAST while acquiring or re-anchoring, at the BASE
# rate while merely OK, and RELAX toward the ceiling only once the clock is well
# self-regulated -- consecutive cycles that are TRUST_OK, carry a converged drift
# rate, and where BOTH the core sources and the two NTS anchors agree tightly.
# Any state drop, sync error, or continuity break (state != ok/degraded) snaps
# straight back to FAST, then backs off toward BASE so a sustained outage does
# not hammer the servers. Returns the ms delay until the next poll.
proc lunar::next_poll_ms {syncErr} {
    set fast $::lunar::poll_min
    set base $::lunar::poll_ms
    set max  $::lunar::poll_max
    set unhealthy $syncErr
    set converged 0
    if {!$syncErr && [llength [info commands ::lunar::status]]} {
        if {[catch { ::lunar::status } st]} {
            set unhealthy 1
        } else {
            set state [dict get $st state]
            # Unhealthy = not fully trusted, or the interval has grown past
            # half the stop ceiling (recover BEFORE the face would blank).
            set b [expr {[dict exists $st boundMs] ? [dict get $st boundMs] : 0}]
            if {$state ne "ok" || $b > [dict get $::lunar::cfg stopms] / 2} {
                set unhealthy 1
            }
            if {$state eq "ok" && [dict get $st synced] && [dict get $st ratePpm] != 0} {
                set cs [dict get $st spreadMs]
                set ns [dict get $st ntsSpreadMs]
                if {$cs >= 0 && $cs <= 80 && $ns >= 0 && $ns <= 80} { set converged 1 }
            }
        }
    }
    if {$unhealthy} {
        set ::lunar::poll_good 0
        incr ::lunar::poll_bad
        set iv [expr {$fast * (1 << min($::lunar::poll_bad - 1, 4))}]  ;# 8,16,32,60,60s
        return [expr {$iv > $base ? $base : $iv}]
    }
    set ::lunar::poll_bad 0
    if {!$converged} { set ::lunar::poll_good 0 ; return $base }
    incr ::lunar::poll_good
    if {$::lunar::poll_good < 3} { return $base }                      ;# confirm before relaxing
    set iv [expr {$base * (1 << min($::lunar::poll_good - 3, 8))}]     ;# 60,120,240,480...
    return [expr {$iv > $max ? $max : $iv}]                           ;# capped at 5 min
}

# ---- system tray + window state --------------------------------------------
proc lunar::hwnd {} { return [winfo id .] }

proc lunar::restore {} {
    wm deiconify .
    raise .
    catch { focus -force . }
}

# Called from the C tray subclass (via ::lunar::tray_event) at a safe point.
proc lunar::tray_event {kind} {
    switch $kind {
        activate { lunar::restore }
        menu     { lunar::tray_menu }
        resize-start { lunar::square_resize_start }
        resize-end   { lunar::square_resize_end }
    }
}

proc lunar::tray_menu {} {
    catch {destroy .traymenu}
    menu .traymenu -tearoff 0
    .traymenu add command -label "Restore"    -command lunar::restore
    .traymenu add command -label "Sync now"   -command { catch { ::lunar::syncnow } }
    .traymenu add command -label "Settings…"  -command lunar::settings_dlg
    .traymenu add separator
    .traymenu add command -label "Exit Lunar" -command lunar::quit
    # a tray popup needs the owning window foregrounded or it won't dismiss
    catch { focus -force . }
    tk_popup .traymenu [winfo pointerx .] [winfo pointery .]
}

# minimize-to-tray: when enabled, a minimize withdraws the window (leaving only
# the tray icon). Guarded on iconic state so withdrawing can't re-enter.
proc lunar::on_unmap {} {
    if {[dict get $::lunar::cfg tray] && [wm state .] eq "iconic"} {
        wm withdraw .
    }
}

proc lunar::tray_setup {} {
    if {![llength [info commands ::lunar::tray_add]]} return
    catch { ::lunar::tray_add [lunar::hwnd] "Lunar" }
    bind . <Unmap> { after idle lunar::on_unmap }
}

proc lunar::tray_tip_update {text} {
    if {$text eq $::lunar::tray_tip_last} return
    set ::lunar::tray_tip_last $text
    if {[llength [info commands ::lunar::tray_tip]]} {
        catch { ::lunar::tray_tip [lunar::hwnd] $text }
    }
}

proc lunar::quit {} {
    catch { if {[llength [info commands ::lunar::tray_remove]]} { ::lunar::tray_remove [lunar::hwnd] } }
    catch { if {[llength [info commands ::lunar::shutdown]]} { ::lunar::shutdown } }
    destroy .
}

# ---- poll loop --------------------------------------------------------------
# The Tcl side feeds the face authoritative disciplined time at five frames
# per second, phase locked to the disciplined clock. The native widget sweeps
# the second hand at ~30 fps on its own timer by extrapolating (at most a few
# hundred ms of QPC) between feeds -- so the Tk event loop never becomes an
# animation loop, and a stalled feed pauses the hand instead of letting the
# display free-run.
proc lunar::schedule_tick {utcMs} {
    set delay 200
    if {$utcMs ne ""} {
        set phase [expr {$utcMs % 200}]
        if {$phase < 0} { set phase [expr {$phase + 200}] }
        set delay [expr {200 - $phase}]
        # Match the old native timer's small guard against a pathological
        # immediate re-entry when a frame lands just before its boundary.
        if {$delay < 10} { set delay [expr {$delay + 200}] }
    }
    after $delay lunar::tick
}

proc lunar::tick {} {
    set nextUtcMs ""
    if {[catch {
        if {[llength [info commands ::lunar::status]]} {
            set st [::lunar::status]
            lunar::render $st
            if {[dict get $st hasTime]} { set nextUtcMs [dict get $st utcMs] }
        } else {
            # no engine (dev/spike): fall back to the local clock
            set nowMs [clock milliseconds]
            set now [expr {$nowMs / 1000}]
            set milliseconds [expr {$nowMs % 1000}]
            if {$milliseconds < 0} { set milliseconds [expr {$milliseconds + 1000}] }
            set lt [dict create \
                hour   [scan [clock format $now -format %H] %d] \
                minute [scan [clock format $now -format %M] %d] \
                second [scan [clock format $now -format %S] %d]]
            # No engine, no honest bound: claim none (no fan), never stopped.
            lunar::clock_display $lt $milliseconds ok 1 1 0 0
            set nextUtcMs $nowMs
        }
    } err opts]} {
        catch { lunar::log "\[tick\] [dict get $opts -errorinfo]" }
    }
    # Always leave a timer behind, including after a transient engine error.
    lunar::schedule_tick $nextUtcMs
}

proc lunar::render {st} {
    set state   [dict get $st state]
    set synced  [dict get $st synced]
    set hasTime [dict get $st hasTime]

    set lt ""
    set milliseconds 0
    if {$hasTime} {
        # break the disciplined UTC down in the DISPLAY zone via the embedded
        # tzdata (never Tcl's [clock], which trusts the OS zone database), then
        # draw the hands; the second hand's fan width carries the uncertainty.
        set utcMs [dict get $st utcMs]
        set milliseconds [expr {$utcMs % 1000}]
        if {$milliseconds < 0} { set milliseconds [expr {$milliseconds + 1000}] }
        if {![catch { ::lunar::localtime $utcMs $::lunar::tz } lt]} {
            # The native widget owns its pixels and exposes its trust state on
            # the face; the canvas fallback preserves the same hand policy.
        }
    }
    # The uncertainty fan carries the bound; the stop rule withdraws the
    # seconds claim entirely once the bound exceeds the user's ceiling.
    set boundMs [dict get $st boundMs]
    # LUNAR_FAKE_BOUND=<ms>: dev hook for screenshot/UI review of the fan and
    # the stop rule at an arbitrary bound (the engine's real bound is hard to
    # stage). Display-only; never affects the engine or the chime edge.
    if {[info exists ::env(LUNAR_FAKE_BOUND)] &&
        [string is integer -strict $::env(LUNAR_FAKE_BOUND)]} {
        set boundMs $::env(LUNAR_FAKE_BOUND)
    }
    set running [expr {$hasTime && $lt ne ""}]
    set stopped [lunar::stop_eval $running $boundMs]
    lunar::clock_display $lt $milliseconds $state $running $synced \
        $boundMs $stopped

    # Recover as fast as possible: on the edge where the face stops showing
    # time -- the stop ceiling was crossed, or the time vanished outright --
    # fire an immediate poll instead of waiting out the scheduled interval.
    if {($stopped && !$::lunar::stopped_prev) ||
        (!$running && $::lunar::hastime_prev)} {
        lunar::poll_now
    }
    set ::lunar::stopped_prev $stopped
    set ::lunar::hastime_prev $running

    # The face has exactly two states -- time (uncertainty carried entirely
    # by the second hand's fan width) or no time -- so the bar words a state
    # ONLY when the face shows no time: STOPPED, or ACQUIRING/NO SIGNAL/
    # REACQUIRING.
    # Cells hide entirely (with their separator dots) when redundant, so the
    # zone witness is never starved into a clipped, fake-looking code: the
    # full bar's natural width exceeded the window (measured 633 vs 510 px),
    # which left-clipped "UTC+2" into "JTC+2".
    lassign [lunar::state_display $state $synced] txt col
    if {$stopped} {
        set txt "STOPPED" ; set col $::lunar::ACCENT
    }
    set word [expr {($stopped || !$running) ? $txt : ""}]
    .sb.trust configure -text $word -fg $col
    .sb.sep1  configure -text [expr {$word eq "" ? "" : "·"}]
    set bound [lunar::fmt_bound $boundMs]
    .sb.bound configure -text [expr {$running ? ($bound eq "" ? "±—" : $bound) : ""}] \
        -fg $::lunar::MUTED
    set showSys [expr {$running && !$stopped && [dict get $st sysDeltaValid]}]
    # A separator dot renders only when BOTH its neighbors are visible:
    # sep2 pairs bound|sys; sep3 is the zone's dot and pairs it with ANY
    # visible cell on the left cluster (bound when running -- with SYS hidden
    # the bound|zone pairing still needs it); in the no-time states sep1
    # (word) already provides the single dot before the zone.
    .sb.sep2  configure -text [expr {$showSys ? "·" : ""}]
    .sb.sys  configure -text [expr {$showSys ? \
        "SYS[lunar::fmt_delta [dict get $st sysDeltaMs]]" : ""}] -fg $::lunar::MUTED
    .sb.sep3 configure -text [expr {$running ? "·" : ""}]
    if {$running} {
        .sb.zone configure -text "[dict get $lt abbr] [lunar::fmt_utcoff [dict get $lt offSec]]"
    } else {
        .sb.zone configure -text $::lunar::tz
    }

    set tip "Lunar · $txt"
    if {$stopped} {
        append tip " · [lunar::fmt_bound $boundMs] exceeds\
            [expr {[dict get $::lunar::cfg stopms] / 1000.0}] s · recovering"
    } elseif {$hasTime && $lt ne ""} {
        append tip " · [format %02d:%02d [dict get $lt hour] [dict get $lt minute]] [dict get $lt abbr]"
    }
    lunar::tray_tip_update $tip

    # chimes on armed marks, with a just-in-time audio pre-warm ahead of them
    if {$hasTime && [info exists lt]} {
        lunar::prewarm_check [dict get $lt minute] [dict get $lt second] [dict get $st boundMs]
        lunar::chime_check [dict get $lt minute] [dict get $st boundMs]
    } else {
        set ::lunar::prev_min -1
        set ::lunar::prewarm_min -1
    }
}

# ---- selftest (headless) ----------------------------------------------------
proc lunar::selftest {reportPath} {
    set ok 1 ; set msg ""
    if {[catch { lunar::build ; update idletasks ; update } err]} { set ok 0 ; set msg $err }
    set txt "lunar-selftest\nversion=$::lunar::version\ntk=[package present Tk]\n"
    append txt "engine=[expr {[llength [info commands ::lunar::status]] ? {yes} : {no}}]\n"
    append txt "toplevel=[winfo exists .face.clock]\n"
    set hasSettings [winfo exists .sb.settings]
    append txt "settingsbutton=[expr {$hasSettings ? {yes} : {no}}]\n"
    if {!$hasSettings} { set ok 0 ; set msg "status-bar settings button is missing" }
    set hasMenu [expr {[. cget -menu] ne ""}]
    append txt "appmenu=[expr {$hasMenu ? {yes} : {no}}]\n"
    if {$hasMenu} { set ok 0 ; set msg "application menu should not be attached" }
    if {[llength [info commands ::lunar::tz_list]]} {
        append txt "tzcount=[llength [::lunar::tz_list]]\n"
        append txt "tzversion=[::lunar::tz_version]\n"
        # a known zone must resolve and disagree with UTC in summer
        if {![catch { ::lunar::localtime 1751500000000 Europe/Brussels } lt]} {
            append txt "tzresolve=[dict get $lt abbr]/[dict get $lt offSec]\n"
        } else {
            set ok 0 ; set msg "tz resolve failed: $lt"
        }
    }
    if {[llength [info commands ::lunar::run_at_startup]]} {
        append txt "startup=[::lunar::run_at_startup]\n"
    }
    append txt "beep=[expr {[llength [info commands ::lunar::beep]] ? {yes} : {no}}]\n"
    append txt "prewarm=[expr {[llength [info commands ::lunar::prewarm]] ? {yes} : {no}}]\n"
    append txt "armed=[join $::lunar::armed {}]\n"
    # The face's ButtonRelease binding must reach the native Direct2D child
    # too, not only the canvas fallback. Stub persistence so self-test never
    # changes the user's armed.dat while exercising the real interaction.
    set savedArmed $::lunar::armed
    if {[catch {
        set ::lunar::armed [lrepeat 12 0]
        rename ::lunar::armed_save ::lunar::_armed_save
        proc ::lunar::armed_save {} {}
        set cw [winfo width .face.clock] ; set ch [winfo height .face.clock]
        set cs [expr {min($cw, $ch)}]
        event generate .face.clock <ButtonRelease-1> \
            -x [expr {$cw / 2}] -y [expr {round($ch / 2.0 - $cs * .46)}]
        update
        set clickok [expr {[lindex $::lunar::armed 0] == 1}]
        append txt "markerclick=[expr {$clickok ? {ok} : {FAIL}}]\n"
        if {!$clickok} { set ok 0 ; set msg "hour-marker click did not arm :00" }
    } markerErr]} {
        append txt "markerclick=FAIL\n"
        set ok 0 ; set msg "hour-marker click test failed: $markerErr"
    }
    catch { rename ::lunar::armed_save {} }
    catch { rename ::lunar::_armed_save ::lunar::armed_save }
    set ::lunar::armed $savedArmed
    # verify the chime edge-detector fires on an armed crossing, silently
    catch {
        set ::lunar::armed [lreplace $::lunar::armed 1 1 1]   ;# arm :05
        dict set ::lunar::cfg chimes 1
        set ::lunar::_chimes 0
        catch { rename ::lunar::beep ::lunar::_realbeep }
        proc ::lunar::beep {} { incr ::lunar::_chimes }
        set ::lunar::prev_min 4 ; lunar::chime_check 5 1000       ;# 4->5, bound ok -> fire
        set ::lunar::prev_min 5 ; lunar::chime_check 6 1000       ;# 5->6, :06 not a mark
        set ::lunar::prev_min 4 ; lunar::chime_check 5 60000      ;# bound too big -> no fire
        append txt "chimefire=$::lunar::_chimes\n"                ;# want 1
    }
    # verify the just-in-time pre-warm fires once, ~3 s before an armed mark
    catch {
        set ::lunar::armed [lreplace $::lunar::armed 1 1 1]   ;# arm :05
        dict set ::lunar::cfg chimes 1
        set ::lunar::_prewarms 0
        catch { rename ::lunar::prewarm ::lunar::_realprewarm }
        proc ::lunar::prewarm {} { incr ::lunar::_prewarms }
        set ::lunar::prewarm_min -1
        lunar::prewarm_check 4 57 1000    ;# 3 s before armed :05 -> prime
        lunar::prewarm_check 4 58 1000    ;# same mark, already primed -> no
        lunar::prewarm_check 4 50 1000    ;# too early in the minute -> no
        lunar::prewarm_check 3 57 1000    ;# next minute :04 not a mark -> no
        append txt "prewarmfire=$::lunar::_prewarms\n"           ;# want 1
    }
    # verify the adaptive poll cadence: FAST (8s) when unhealthy, BASE (60s)
    # when merely OK, RELAXED ramp (120/240/300s, capped) once converged.
    catch {
        catch { rename ::lunar::status ::lunar::_realstatus }
        proc ::lunar::status {} { return $::lunar::_ststub }
        set ::lunar::poll_good 0 ; set ::lunar::poll_bad 0
        set ::lunar::_ststub {state holdover synced 0 ratePpm 0 spreadMs 0 ntsSpreadMs 0 boundMs 6000}
        set fst [lunar::next_poll_ms 0]                          ;# unhealthy -> 8000
        set ::lunar::poll_good 0 ; set ::lunar::poll_bad 0
        set ::lunar::_ststub {state ok synced 1 ratePpm 0 spreadMs 20 ntsSpreadMs 20 boundMs 50}
        set bse [lunar::next_poll_ms 0]                          ;# OK, no rate -> 60000
        set ::lunar::poll_good 0 ; set ::lunar::poll_bad 0
        set ::lunar::_ststub {state ok synced 1 ratePpm 13 spreadMs 20 ntsSpreadMs 20 boundMs 50}
        for {set i 1} {$i <= 3} {incr i} { set r3 [lunar::next_poll_ms 0] } ;# confirm x3 -> 60000
        set r4 [lunar::next_poll_ms 0]                           ;# -> 120000
        set r5 [lunar::next_poll_ms 0]                           ;# -> 240000
        set r6 [lunar::next_poll_ms 0]                           ;# -> capped 300000
        set good [expr {$fst==8000 && $bse==60000 && $r3==60000 && $r4==120000 \
                        && $r5==240000 && $r6==300000}]
        if {$good} { set cad ok } else { set cad "BAD $fst $bse $r3 $r4 $r5 $r6" }
        append txt "pollcadence=$cad\n"
    }
    # verify the stop rule: setting clamp, 2-read debounce, immediate resume
    catch {
        set oldStop [dict get $::lunar::cfg stopms]
        dict set ::lunar::cfg stopms 5000
        set ::lunar::stop_hits 0
        set r1 [lunar::stop_eval 1 5100]   ;# 1st read above ceiling -> not yet
        set r2 [lunar::stop_eval 1 5100]   ;# 2nd consecutive -> stopped
        set r3 [lunar::stop_eval 1 5100]   ;# stays stopped
        set r4 [lunar::stop_eval 1 300]    ;# back under -> resumes immediately
        set r5 [lunar::stop_eval 0 9999]   ;# no time -> stop path not taken
        set c1 [lunar::clamp_stopms 400]   ;# below floor  -> 1000
        set c2 [lunar::clamp_stopms 99999] ;# above ceil   -> 30000
        set c3 [lunar::clamp_stopms abc]   ;# garbage      -> default 5000
        set good [expr {!$r1 && $r2 && $r3 && !$r4 && !$r5 &&
                        $c1 == 1000 && $c2 == 30000 && $c3 == 5000}]
        dict set ::lunar::cfg stopms $oldStop
        if {$good} { set sg ok } else { set sg "BAD $r1$r2$r3$r4$r5 $c1 $c2 $c3" }
        append txt "stopgate=$sg\n"
        catch { rename ::lunar::status {} }
        catch { rename ::lunar::_realstatus ::lunar::status }
    }
    # build added a real tray icon; remove it so headless checks leave nothing
    catch { if {[llength [info commands ::lunar::tray_remove]]} { ::lunar::tray_remove [winfo id .] } }
    append txt "status=[expr {$ok ? {ok} : {FAIL}}]\n"
    if {!$ok} { append txt "error=$msg\n" }
    if {$reportPath ne ""} {
        catch { set fh [open $reportPath w] ; puts -nonewline $fh $txt ; close $fh }
    }
    exit [expr {$ok ? 0 : 1}]
}

proc lunar::main {} {
    set i [lsearch -exact $::argv "--selftest"]
    if {$i >= 0} {
        lunar::selftest [lindex $::argv [expr {$i + 1}]]
        return
    }
    lunar::settings_load
    lunar::armed_load
    # single-source the version from the engine (VERSION -> version.h) so the
    # title bar and About agree
    if {[llength [info commands ::lunar::about]]} {
        set ::lunar::version [dict get [::lunar::about] version]
    }
    # the registry Run key is authoritative for run-at-startup (the user may
    # have toggled it via Task Manager/msconfig outside Lunar)
    if {[llength [info commands ::lunar::run_at_startup]]} {
        dict set ::lunar::cfg startup [::lunar::run_at_startup]
    }
    lunar::tz_startup
    lunar::build
    # Route uncaught async errors to the log + a status note, never a modal
    # dialog (els's production bgerror pattern). Installed after build.
    proc ::bgerror {msg args} { lunar::bgerror $msg {*}$args }
    catch { interp bgerror {} lunar::bgerror }
    catch { proc ::tk::dialog::error::bgerror {msg args} { lunar::bgerror $msg {*}$args } }

    if {[llength [info commands ::lunar::engine_start]]} {
        if {[catch { ::lunar::engine_start } e opts]} {
            catch { lunar::log "\[engine_start\] [dict get $opts -errorinfo]" }
        }
        # engine_start fired cycle #1 now; arm the adaptive scheduler at the
        # FAST floor so a fresh boot re-polls quickly until it has anchored,
        # rather than waiting a full base interval.
        set ::lunar::repoll_after [after $::lunar::poll_min lunar::repoll]
    }
    after 100 lunar::tick
    # dev hooks: open a dialog on launch, for screenshots/testing
    if {[info exists ::env(LUNAR_OPEN_SETTINGS)] && $::env(LUNAR_OPEN_SETTINGS) ne ""} { after 400 lunar::settings_dlg }
    if {[info exists ::env(LUNAR_OPEN_ABOUT)]    && $::env(LUNAR_OPEN_ABOUT)    ne ""} { after 400 {lunar::settings_dlg app} }
    if {[info exists ::env(LUNAR_OPEN_LOG)]      && $::env(LUNAR_OPEN_LOG)      ne ""} { after 400 lunar::log_dlg }
    # LUNAR_BEEP=<n>: on launch, run the real prewarm->chime sequence n times
    # (prewarm ~300ms before each chime, ~1s apart), for audio testing
    # (cold-device reliability, overlap-drop). Default 1.
    if {[info exists ::env(LUNAR_BEEP)] && $::env(LUNAR_BEEP) ne ""} {
        set n [expr {[string is integer -strict $::env(LUNAR_BEEP)] ? $::env(LUNAR_BEEP) : 1}]
        for {set b 0} {$b < $n} {incr b} {
            after [expr {300 + $b * 1000}] { catch { ::lunar::prewarm } }
            after [expr {600 + $b * 1000}] { catch { ::lunar::beep } }
        }
    }
}

lunar::main
