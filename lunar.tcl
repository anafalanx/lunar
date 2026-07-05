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
font create lunarUIb   -family {Segoe UI} -size 9 -weight bold
font create lunarTitle -family {Segoe UI Light} -size 32
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

# ---- settings (same file + format + semantics as the Win32 shell) -----------
# %APPDATA%\Lunar\settings.dat, one key=value per line. Keys this shell does
# not own (chimes/unmin/confirm/tray/startup) are preserved verbatim so the
# two shells can be swapped without losing anything. The PRESENCE of the tz=
# key -- even empty, meaning explicit UTC -- counts as a deliberate choice;
# only then does first-run OS-zone suggestion stop.
set ::lunar::cfg [dict create fmt24 1 tray 0 startup 0]
set ::lunar::cfg_extra {}       ;# unowned lines (chimes/unmin/confirm), kept in file order
set ::lunar::tz "UTC"           ;# the active display zone
set ::lunar::tz_chosen 0
set ::lunar::tray_tip_last ""

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
            tz      { set ::lunar::tz_chosen 1 ; dict set ::lunar::cfg tz $v }
            default { lappend ::lunar::cfg_extra $line }
        }
    }
}

proc lunar::settings_save {} {
    set dir [lunar::datadir]
    if {[catch {file mkdir $dir}]} { return }
    set lines $::lunar::cfg_extra
    lappend lines "fmt24=[dict get $::lunar::cfg fmt24]"
    lappend lines "tray=[dict get $::lunar::cfg tray]"
    lappend lines "startup=[dict get $::lunar::cfg startup]"
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

# ---- the dashboard ----------------------------------------------------------
proc lunar::build {} {
    wm title . "Lunar $::lunar::version"
    wm geometry . 540x640
    wm minsize . 460 520
    lunar::init_style
    . configure -background $::lunar::PAGE
    catch { wm iconphoto . -default [image create photo -file [file join [file dirname [info script]] resources icon.png]] }

    # menubar
    menu .menu -tearoff 0
    . configure -menu .menu
    menu .menu.lunar -tearoff 0
    .menu add cascade -label "Lunar" -menu .menu.lunar
    .menu.lunar add command -label "Sync now"     -accelerator "Ctrl+R" -command { catch { ::lunar::syncnow } }
    .menu.lunar add command -label "Copy time"    -accelerator "Ctrl+C" -command lunar::copy_time
    .menu.lunar add separator
    .menu.lunar add command -label "Settings…"    -accelerator "Ctrl+," -command lunar::settings_dlg
    .menu.lunar add separator
    .menu.lunar add command -label "About Lunar"  -command lunar::about_dlg
    .menu.lunar add separator
    .menu.lunar add command -label "Exit"         -command lunar::quit
    menu .menu.view -tearoff 0
    .menu add cascade -label "View" -menu .menu.view
    .menu.view add command     -label "Event log…"     -command lunar::log_dlg
    .menu.view add checkbutton -label "Always on top" -variable ::lunar::ontop -command lunar::apply_ontop
    bind . <Control-r>     { catch { ::lunar::syncnow } ; break }
    bind . <Control-c>     { lunar::copy_time ; break }
    bind . <Control-comma> { lunar::settings_dlg ; break }

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

    # status bar: hairline + muted labels, clickable zone indicator (els idiom)
    frame .sb -bg $::lunar::CHROME
    frame .sb.hair -height 1 -bg $::lunar::HAIR
    label .sb.sys    -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUI -anchor w -text ""
    label .sb.zone   -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUI -anchor e -text "" -cursor hand2
    frame .sb.sep1 -width 1 -bg $::lunar::HAIR
    label .sb.update -bg $::lunar::CHROME -fg $::lunar::ACCENT -font lunarUI -anchor e -text "" -cursor hand2
    pack .sb.hair   -side top -fill x
    pack .sb.sys    -side left  -padx {12 8} -pady 4
    pack .sb.zone   -side right -padx {8 12} -pady 4
    pack .sb.sep1   -side right -padx {2 2}  -pady {7 6} -fill y
    pack .sb.update -side right -padx {8 2}  -pady 4
    bind .sb.zone <Button-1> lunar::settings_dlg
    bind .sb.zone <Enter> { .sb.zone configure -fg $::lunar::INK }
    bind .sb.zone <Leave> { .sb.zone configure -fg $::lunar::MUTED }

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
    lunar::tray_setup
}

# ---- menu actions: copy time, always-on-top, About, log viewer --------------
proc lunar::apply_ontop {} { catch { wm attributes . -topmost $::lunar::ontop } }

proc lunar::copy_time {} {
    if {![llength [info commands ::lunar::status]]} return
    set st [::lunar::status]
    if {![dict get $st hasTime]} { lunar::status_note "no trusted time yet" ; return }
    if {[catch { ::lunar::localtime [dict get $st utcMs] $::lunar::tz } lt]} return
    set off [dict get $lt offSec]
    set sign [expr {$off < 0 ? "-" : "+"}] ; set a [expr {abs($off)}]
    set iso [format "%04d-%02d-%02dT%02d:%02d:%02d%s%02d:%02d" \
        [dict get $lt year] [dict get $lt month] [dict get $lt day] \
        [dict get $lt hour] [dict get $lt minute] [dict get $lt second] \
        $sign [expr {$a / 3600}] [expr {($a % 3600) / 60}]]
    clipboard clear ; clipboard append $iso
    lunar::status_note "copied $iso"
}

proc lunar::about_dlg {} {
    catch {destroy .about}
    set P $::lunar::PAGE
    toplevel .about -bg $P
    wm withdraw .about
    wm title .about "About Lunar" ; wm transient .about . ; wm resizable .about 0 0
    set a [dict create version $::lunar::version tzdata "?"]
    if {[llength [info commands ::lunar::about]]} { set a [::lunar::about] }
    frame .about.c -bg $P ; pack .about.c -padx 40 -pady 30
    label .about.c.name -bg $P -fg $::lunar::INK   -font lunarTitle -text "Lunar"
    label .about.c.ver  -bg $P -fg $::lunar::MUTED -font lunarUI    -text "version [dict get $a version]"
    label .about.c.tag  -bg $P -fg $::lunar::MUTED -font lunarUI    -text "Trustworthy network time for Windows."
    label .about.c.tz   -bg $P -fg $::lunar::MUTED -font lunarSmall -text "embedded tzdata [dict get $a tzdata]  ·  MIT licensed"
    pack .about.c.name -pady {0 4}
    pack .about.c.ver  -pady {0 0}
    pack .about.c.tag  -pady {10 0}
    pack .about.c.tz   -pady {12 0}
    bind .about <Escape> {destroy .about}
    lunar::bindtree .about <Button-1> {destroy .about}
    update idletasks
    set x [expr {[winfo rootx .] + ([winfo width .]  - [winfo reqwidth .about])  / 2}]
    set y [expr {[winfo rooty .] + ([winfo height .] - [winfo reqheight .about]) / 3}]
    wm geometry .about +$x+$y ; wm deiconify .about ; focus .about
}
# bind an event to a widget and all its descendants (els helper)
proc lunar::bindtree {w seq script} {
    bind $w $seq $script
    foreach c [winfo children $w] { lunar::bindtree $c $seq $script }
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
# els dialog idiom: a plain toplevel over PAGE, built off-screen, centered over
# the main window, Escape closes. Zone picker = filter entry + listbox with a
# live "current time in selected zone" preview off the disciplined clock.
proc lunar::settings_dlg {} {
    catch {destroy .set}
    set P $::lunar::PAGE
    toplevel .set -bg $P
    wm withdraw .set
    wm title .set "Lunar Settings"
    wm transient .set .
    wm resizable .set 0 0

    frame .set.card -bg $P
    pack  .set.card -padx 26 -pady 20
    set c .set.card

    label $c.zhdr -bg $P -fg $::lunar::INK -font lunarUIb -anchor w -text "Display time zone"
    frame $c.zf -bg $P
    label $c.zf.l -bg $P -fg $::lunar::MUTED -font lunarUI -text "Filter:"
    ttk::entry $c.zf.e -font lunarUI -width 32 -textvariable ::lunar::set_filter
    pack $c.zf.l -side left -padx {0 6}
    pack $c.zf.e -side left -fill x -expand 1
    frame $c.zl -bg $P
    listbox $c.zl.list -height 9 -font lunarUI -bg $P -fg $::lunar::INK \
        -selectbackground "#D6E2F2" -selectforeground $::lunar::INK \
        -borderwidth 1 -relief solid -highlightthickness 0 -exportselection 0 \
        -yscrollcommand [list $c.zl.vs set]
    ttk::scrollbar $c.zl.vs -orient vertical -command [list $c.zl.list yview]
    pack $c.zl.vs   -side right -fill y
    pack $c.zl.list -side left -fill both -expand 1
    label $c.zprev -bg $P -fg $::lunar::MUTED -font lunarUI -anchor w -text ""

    checkbutton $c.fmt24 -bg $P -fg $::lunar::INK -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Use 24-hour clock" -variable ::lunar::set_fmt24
    checkbutton $c.tray -bg $P -fg $::lunar::INK -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Minimize to the notification area (tray)" -variable ::lunar::set_tray
    checkbutton $c.startup -bg $P -fg $::lunar::INK -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Start Lunar when you sign in" -variable ::lunar::set_startup
    frame $c.btns -bg $P
    ttk::button $c.btns.ok     -style Dialog.TButton -text "OK"     -command lunar::settings_ok
    ttk::button $c.btns.cancel -style Dialog.TButton -text "Cancel" -command {destroy .set}
    pack $c.btns.cancel -side right
    pack $c.btns.ok     -side right -padx {0 8}

    pack $c.zhdr  -fill x -pady {0 6}
    pack $c.zf    -fill x -pady {0 6}
    pack $c.zl    -fill x
    pack $c.zprev   -fill x -pady {4 0}
    pack $c.fmt24   -fill x -pady {14 0}
    pack $c.tray    -fill x -pady {4 0}
    pack $c.startup -fill x -pady {4 0}
    pack $c.btns    -fill x -pady {18 0}

    set ::lunar::set_filter ""
    set ::lunar::set_fmt24   [dict get $::lunar::cfg fmt24]
    set ::lunar::set_tray    [dict get $::lunar::cfg tray]
    set ::lunar::set_startup [dict get $::lunar::cfg startup]
    trace add variable ::lunar::set_filter write {apply {{args} {lunar::settings_fill}}}
    bind $c.zl.list <<ListboxSelect>> lunar::settings_preview
    bind .set <Escape> {destroy .set}
    lunar::settings_fill

    update idletasks
    set x [expr {[winfo rootx .] + ([winfo width .]  - [winfo reqwidth .set]) / 2}]
    set y [expr {[winfo rooty .] + ([winfo height .] - [winfo reqheight .set]) / 3}]
    wm geometry .set +$x+$y
    wm deiconify .set
    focus $c.zf.e
    lunar::settings_preview_loop
}

# repopulate the zone list from the filter; keep the active zone selected
proc lunar::settings_fill {} {
    set lb .set.card.zl.list
    if {![winfo exists $lb]} return
    set pat [string tolower [string trim $::lunar::set_filter]]
    $lb delete 0 end
    set sel -1 ; set i 0
    set zones [expr {[llength [info commands ::lunar::tz_list]] ? [::lunar::tz_list] : {UTC}}]
    foreach z $zones {
        if {$pat ne "" && ![string match "*$pat*" [string tolower $z]]} continue
        $lb insert end $z
        if {$z eq $::lunar::tz} { set sel $i }
        incr i
    }
    if {$sel >= 0} { $lb selection set $sel ; $lb see $sel }
    lunar::settings_preview
}

proc lunar::settings_sel {} {
    set lb .set.card.zl.list
    if {![winfo exists $lb]} { return "" }
    set s [$lb curselection]
    if {$s eq ""} { return "" }
    return [$lb get [lindex $s 0]]
}

proc lunar::settings_preview {} {
    set pv .set.card.zprev
    if {![winfo exists $pv]} return
    set z [lunar::settings_sel]
    if {$z eq ""} { $pv configure -text "" ; return }
    set txt "Current time: —"
    if {[llength [info commands ::lunar::status]]} {
        set st [::lunar::status]
        if {[dict get $st hasTime] &&
            ![catch { ::lunar::localtime [dict get $st utcMs] $z } lt]} {
            set txt "Current time: [lunar::fmt_time $lt]  [dict get $lt abbr] ([lunar::fmt_utcoff [dict get $lt offSec]])"
        }
    }
    $pv configure -text $txt
}

proc lunar::settings_preview_loop {} {
    if {![winfo exists .set]} return
    lunar::settings_preview
    after 500 lunar::settings_preview_loop
}

proc lunar::settings_ok {} {
    set z [lunar::settings_sel]
    if {$z ne ""} { set ::lunar::tz $z }
    set ::lunar::tz_chosen 1
    dict set ::lunar::cfg tz [expr {$::lunar::tz eq "UTC" ? "" : $::lunar::tz}]
    dict set ::lunar::cfg fmt24 [expr {$::lunar::set_fmt24 ? 1 : 0}]
    dict set ::lunar::cfg tray  [expr {$::lunar::set_tray ? 1 : 0}]
    # the registry Run key is the source of truth for run-at-startup
    if {[llength [info commands ::lunar::run_at_startup]]} {
        catch { ::lunar::run_at_startup [expr {$::lunar::set_startup ? 1 : 0}] }
        dict set ::lunar::cfg startup [::lunar::run_at_startup]
    } else {
        dict set ::lunar::cfg startup [expr {$::lunar::set_startup ? 1 : 0}]
    }
    lunar::settings_save
    destroy .set
}

# nudge the engine to re-sync periodically (defined at top level, not inside
# main: a `proc lunar::x` created from within a ::lunar-namespace proc would
# resolve to ::lunar::lunar::x and fail).
proc lunar::repoll {} {
    catch { ::lunar::syncnow }
    after $::lunar::poll_ms lunar::repoll
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
        # break the disciplined UTC down in the DISPLAY zone via the embedded
        # tzdata (never Tcl's [clock], which trusts the OS zone database)
        set utcMs [dict get $st utcMs]
        if {![catch { ::lunar::localtime $utcMs $::lunar::tz } lt]} {
            .face.time configure -text [lunar::fmt_time $lt]
            .face.date configure -text [lunar::fmt_date $lt]
            .sb.zone configure -text \
                "$::lunar::tz · [dict get $lt abbr] ([lunar::fmt_utcoff [dict get $lt offSec]])"
        }
        .face.bound configure -text [lunar::fmt_bound [dict get $st boundMs]]
    } else {
        .face.time  configure -text "--:--:--"
        .face.date  configure -text ""
        .face.bound configure -text ""
        .sb.zone    configure -text $::lunar::tz
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

    # live tray tooltip (HH:MM so it only changes once a minute; throttled)
    set tip "Lunar · $txt"
    if {$hasTime && [info exists lt]} {
        append tip " · [format %02d:%02d [dict get $lt hour] [dict get $lt minute]] [dict get $lt abbr]"
    }
    lunar::tray_tip_update $tip
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
        after $::lunar::poll_ms lunar::repoll
    }
    after 100 lunar::tick
    # dev hooks: open a dialog on launch, for screenshots/testing
    if {[info exists ::env(LUNAR_OPEN_SETTINGS)] && $::env(LUNAR_OPEN_SETTINGS) ne ""} { after 400 lunar::settings_dlg }
    if {[info exists ::env(LUNAR_OPEN_ABOUT)]    && $::env(LUNAR_OPEN_ABOUT)    ne ""} { after 400 lunar::about_dlg }
    if {[info exists ::env(LUNAR_OPEN_LOG)]      && $::env(LUNAR_OPEN_LOG)      ne ""} { after 400 lunar::log_dlg }
}

lunar::main
