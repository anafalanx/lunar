# lunar.tcl -- Lunar's Tcl/Tk shell (startup script, packaged as main.tcl).
#
# Digital "time dashboard": a large disciplined-time readout plus the trust
# state, error bound, and system-clock delta. Time and trust come from the C
# engine via ::lunar::* (see src/lunarx.c); the networking runs on the
# engine's own threads and the UI just polls. Styling is inherited from els.

package require Tk

namespace eval lunar {
    variable version "0.21.0-dev"
    variable poll_ms 60000   ;# ask the engine to re-sync at most this often
    variable log_active 0    ;# reentry latch so logging can't recurse into bgerror
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
proc lunar::status_note {m} { catch { .sb.sys configure -text $m } }
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

option add *tearOff 0
font create lunarUI    -family {Segoe UI} -size 9
font create lunarBig   -family Consolas   -size 44
font create lunarDate  -family {Segoe UI} -size 12
font create lunarSmall -family {Segoe UI} -size 9
font create lunarState -family {Segoe UI Semibold} -size 11
font create lunarMono  -family Consolas   -size 9
font create lunarHdr   -family {Segoe UI Semibold} -size 8

proc lunar::init_style {} {
    set s ttk::style
    catch {$s theme use clam}
    set bg $::lunar::CHROME ; set ink $::lunar::INK ; set hair $::lunar::HAIR
    $s configure . -background $bg -foreground $ink -font lunarUI \
        -borderwidth 0 -focuscolor $bg -troughcolor $::lunar::PAGE \
        -bordercolor $hair -darkcolor $bg -lightcolor $bg
}

# ---- state -> (label, colour) ----------------------------------------------
proc lunar::state_display {state synced} {
    switch $state {
        ok          { return [list "TRUSTED"     $::lunar::OK] }
        degraded    { return [list "DEGRADED"    $::lunar::WARN] }
        holdover    { return [list "HOLDOVER"    $::lunar::WARN] }
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

proc lunar::fmt_delta {ms} {
    set s [expr {$ms / 1000.0}]
    return [format "%+.2f s" $s]
}

# ---- the dashboard ----------------------------------------------------------
proc lunar::build {} {
    wm title . "Lunar $::lunar::version"
    wm geometry . 540x640
    wm minsize . 460 520
    lunar::init_style
    . configure -background $::lunar::PAGE
    catch { wm iconphoto . -default [image create photo -file [file join [file dirname [info script]] resources icon.png]] }

    set P $::lunar::PAGE
    frame .face -bg $P
    label .face.time  -bg $P -fg $::lunar::INK   -font lunarBig   -text "--:--:--"
    label .face.date  -bg $P -fg $::lunar::MUTED -font lunarDate  -text ""
    label .face.state -bg $P -fg $::lunar::MUTED -font lunarState -text "starting…"
    label .face.bound -bg $P -fg $::lunar::MUTED -font lunarSmall -text ""
    pack .face.time  -pady {26 0}
    pack .face.date  -pady {6 0}
    pack .face.state -pady {18 0}
    pack .face.bound -pady {3 0}

    # sources section: hairline + muted header + one fixed row per slot,
    # updated in place each tick (no rebuilding, so no flicker).
    frame .src -bg $P
    frame .src.hair -height 1 -bg $::lunar::HAIR
    label .src.hdr  -bg $P -fg $::lunar::MUTED -font lunarHdr -anchor w -text "SOURCES"
    pack .src.hair -fill x -pady {0 8}
    pack .src.hdr  -fill x -padx 24 -pady {0 2}
    for {set i 0} {$i < 6} {incr i} {
        set r [frame .src.r$i -bg $P]
        label $r.dot  -bg $P -fg $::lunar::MUTED -font lunarMono  -text "●" -width 2
        label $r.name -bg $P -fg $::lunar::INK   -font lunarSmall -anchor w -text "—"
        label $r.off  -bg $P -fg $::lunar::MUTED -font lunarMono  -anchor e -width 9 -text ""
        label $r.rtt  -bg $P -fg $::lunar::MUTED -font lunarMono  -anchor e -width 8 -text ""
        grid $r.dot $r.name $r.off $r.rtt -sticky ew -pady 1
        grid columnconfigure $r 1 -weight 1
        pack $r -fill x -padx 24
    }

    # status bar: hairline + muted labels (els idiom)
    frame .sb -bg $::lunar::CHROME
    frame .sb.hair -height 1 -bg $::lunar::HAIR
    label .sb.sys    -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUI -anchor w -text ""
    label .sb.update -bg $::lunar::CHROME -fg $::lunar::ACCENT -font lunarUI -anchor e -text "" -cursor hand2
    pack .sb.hair   -side top -fill x
    pack .sb.sys    -side left  -padx {12 8} -pady 4
    pack .sb.update -side right -padx {8 12} -pady 4

    # face + sources take their natural height; a page-coloured spacer absorbs
    # the slack so the status bar stays pinned to the bottom and nothing in the
    # face gets squeezed out.
    frame .gap -bg $P
    grid .face -row 0 -column 0 -sticky ew
    grid .src  -row 1 -column 0 -sticky ew -pady {0 12}
    grid .gap  -row 2 -column 0 -sticky nsew
    grid .sb   -row 3 -column 0 -sticky ew
    grid rowconfigure    . 2 -weight 1
    grid columnconfigure . 0 -weight 1

    bind .sb.update <Button-1> { catch { exec {*}[auto_execok start] "" \
        "https://github.com/anafalanx/lunar/releases/latest" & } }
    wm protocol . WM_DELETE_WINDOW lunar::quit
}

# nudge the engine to re-sync periodically (defined at top level, not inside
# main: a `proc lunar::x` created from within a ::lunar-namespace proc would
# resolve to ::lunar::lunar::x and fail).
proc lunar::repoll {} {
    catch { ::lunar::syncnow }
    after $::lunar::poll_ms lunar::repoll
}

proc lunar::quit {} {
    catch { if {[llength [info commands ::lunar::shutdown]]} { ::lunar::shutdown } }
    destroy .
}

# ---- poll loop --------------------------------------------------------------
proc lunar::tick {} {
    after 250 lunar::tick    ;# reschedule FIRST so a transient error can't kill the loop
    if {[catch {
        if {[llength [info commands ::lunar::status]]} {
            lunar::render [::lunar::status]
        } else {
            # no engine (dev/spike): fall back to the local clock
            set now [clock seconds]
            .face.time  configure -text [clock format $now -format %H:%M:%S]
            .face.date  configure -text [clock format $now -format "%A, %d %B %Y"]
            .face.state configure -text "no engine" -fg $::lunar::MUTED
        }
    } err opts]} {
        catch { lunar::log "\[tick\] [dict get $opts -errorinfo]" }
    }
}

proc lunar::render {st} {
    set state   [dict get $st state]
    set synced  [dict get $st synced]
    set hasTime [dict get $st hasTime]

    if {$hasTime} {
        set secs [expr {[dict get $st utcMs] / 1000}]
        .face.time configure -text [clock format $secs -format %H:%M:%S]
        .face.date configure -text [clock format $secs -format "%A, %d %B %Y"]
        .face.bound configure -text [lunar::fmt_bound [dict get $st boundMs]]
    } else {
        .face.time  configure -text "--:--:--"
        .face.date  configure -text ""
        .face.bound configure -text ""
    }

    lassign [lunar::state_display $state $synced] txt col
    .face.state configure -text $txt -fg $col

    if {[dict get $st sysDeltaValid]} {
        .sb.sys configure -text "PC clock: [lunar::fmt_delta [dict get $st sysDeltaMs]] vs Lunar"
    } else {
        .sb.sys configure -text ""
    }

    if {[llength [info commands ::lunar::update_status]]} {
        set u [::lunar::update_status]
        if {[dict get $u available]} {
            .sb.update configure -text "⬇ update v[dict get $u version]"
        } else {
            .sb.update configure -text ""
        }
    }

    if {[llength [info commands ::lunar::sources]]} {
        lunar::render_sources [::lunar::sources]
    }
}

proc lunar::render_sources {srcs} {
    for {set i 0} {$i < 6} {incr i} {
        set r .src.r$i
        if {![winfo exists $r]} continue
        set s [lindex $srcs $i]
        if {$s eq "" || ![dict get $s ok]} {
            set label [expr {$s eq "" ? "—" : [dict get $s label]}]
            if {$label eq ""} { set label "—" }
            $r.dot  configure -fg $::lunar::HAIR
            $r.name configure -text $label -fg $::lunar::MUTED
            $r.off  configure -text "" ; $r.rtt configure -text ""
            continue
        }
        $r.dot  configure -fg $::lunar::OK
        $r.name configure -text [dict get $s label] -fg $::lunar::INK
        $r.off  configure -text [format "%+d ms" [dict get $s offsetMs]]
        $r.rtt  configure -text "[dict get $s rttMs] ms"
    }
}

# ---- selftest (headless) ----------------------------------------------------
proc lunar::selftest {reportPath} {
    set ok 1 ; set msg ""
    if {[catch { lunar::build ; update idletasks ; update } err]} { set ok 0 ; set msg $err }
    set txt "lunar-selftest\nversion=$::lunar::version\ntk=[package present Tk]\n"
    append txt "engine=[expr {[llength [info commands ::lunar::status]] ? {yes} : {no}}]\n"
    append txt "toplevel=[winfo exists .face.time]\n"
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
        after $::lunar::poll_ms lunar::repoll
    }
    after 100 lunar::tick
}

lunar::main
