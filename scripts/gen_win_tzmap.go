// gen_win_tzmap generates src/tz_winmap_gen.c from the vendored CLDR
// windowsZones.xml, filtered to the IANA zones Lunar actually embeds.
//
// Run from the repository root:
//
//	go run scripts/gen_win_tzmap.go
//
// Windows reports a time-zone key such as "W. Europe Standard Time", while
// Lunar renders IANA zones such as "Europe/Berlin". CLDR supplies the mapping,
// and tzdata backward links reconcile CLDR's legacy aliases with Lunar's
// canonical embedded zone names.
package main

import (
	"flag"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strings"
)

const (
	defaultCLDR   = "third_party/cldr/windowsZones.xml"
	defaultEmbed  = "src/tz_embed.c"
	defaultTZData = "third_party/tzdata/zoneinfo/tzdata.zi"
	defaultOutput = "src/tz_winmap_gen.c"
)

var (
	embeddedNamePattern = regexp.MustCompile(`"([^"]+)"`)
	tzLinkPattern       = regexp.MustCompile(`^L\s+(\S+)\s+(\S+)`)
	mapZonePattern      = regexp.MustCompile(`<mapZone other="([^"]+)" territory="([^"]+)" type="([^"]+)"`)
	typeVersionPattern  = regexp.MustCompile(`typeVersion="([^"]+)"`)
)

type options struct {
	cldr   string
	embed  string
	tzdata string
	output string
}

type zoneAliases struct {
	canonicalOf map[string]string
	aliasesOf   map[string][]string
}

type winZonePair struct {
	windows string
	iana    string
}

func main() {
	opts := parseFlags()
	if err := generate(opts); err != nil {
		fmt.Fprintln(os.Stderr, "gen_win_tzmap:", err)
		os.Exit(1)
	}
}

func parseFlags() options {
	var opts options
	flag.StringVar(&opts.cldr, "cldr", defaultCLDR, "path to CLDR windowsZones.xml")
	flag.StringVar(&opts.embed, "embed", defaultEmbed, "path to Lunar's generated tz_embed.c")
	flag.StringVar(&opts.tzdata, "tzdata", defaultTZData, "path to tzdata.zi")
	flag.StringVar(&opts.output, "out", defaultOutput, "output C source path")
	flag.Parse()
	if flag.NArg() != 0 {
		fmt.Fprintf(os.Stderr, "gen_win_tzmap: unexpected argument %q\n", flag.Arg(0))
		flag.Usage()
		os.Exit(2)
	}
	return opts
}

func generate(opts options) error {
	embedText, err := readText(opts.embed)
	if err != nil {
		return err
	}
	embedded := parseEmbeddedZones(embedText)
	fmt.Printf("embedded zones: %d\n", len(embedded))

	tzdataText, err := readText(opts.tzdata)
	if err != nil {
		return err
	}
	aliases := parseTZDataLinks(tzdataText)

	cldrText, err := readText(opts.cldr)
	if err != nil {
		return err
	}
	preferred, alternatives := parseWindowsZones(cldrText)
	pairs, dropped := resolveWindowsZones(preferred, alternatives, embedded, aliases)
	fmt.Printf("mapped: %d   dropped: %d\n", len(pairs), len(dropped))
	for _, name := range dropped {
		fmt.Println("  drop " + name)
	}

	cldrVersion := "unknown"
	if match := typeVersionPattern.FindStringSubmatch(cldrText); match != nil {
		cldrVersion = match[1]
	}
	output := renderC(pairs, cldrVersion)
	if err := os.WriteFile(opts.output, []byte(output), 0o644); err != nil {
		return fmt.Errorf("write %s: %w", opts.output, err)
	}
	fmt.Printf("wrote %s (%d entries, CLDR %s)\n", opts.output, len(pairs), cldrVersion)
	return nil
}

func readText(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", fmt.Errorf("read %s: %w", path, err)
	}
	return string(data), nil
}

func sourceLines(text string) []string {
	text = strings.ReplaceAll(text, "\r\n", "\n")
	text = strings.ReplaceAll(text, "\r", "\n")
	return strings.Split(text, "\n")
}

func parseEmbeddedZones(text string) map[string]bool {
	embedded := map[string]bool{}
	inNames := false
	for _, line := range sourceLines(text) {
		if strings.Contains(line, "g_tz_names[]") {
			inNames = true
			continue
		}
		if inNames && strings.Contains(line, "};") {
			inNames = false
			continue
		}
		if !inNames {
			continue
		}
		if match := embeddedNamePattern.FindStringSubmatch(line); match != nil {
			embedded[match[1]] = true
		}
	}
	return embedded
}

func parseTZDataLinks(text string) zoneAliases {
	aliases := zoneAliases{
		canonicalOf: map[string]string{},
		aliasesOf:   map[string][]string{},
	}
	for _, line := range sourceLines(text) {
		match := tzLinkPattern.FindStringSubmatch(line)
		if match == nil {
			continue
		}
		canonical, alias := match[1], match[2]
		aliases.canonicalOf[alias] = canonical
		aliases.aliasesOf[canonical] = append(aliases.aliasesOf[canonical], alias)
	}
	return aliases
}

func parseWindowsZones(text string) (map[string]string, map[string][]string) {
	preferred := map[string]string{}
	alternatives := map[string][]string{}
	for _, line := range sourceLines(text) {
		match := mapZonePattern.FindStringSubmatch(line)
		if match == nil {
			continue
		}
		windows, territory := match[1], match[2]
		iana := strings.Fields(match[3])[0]
		if territory == "001" {
			preferred[windows] = iana
		} else {
			alternatives[windows] = append(alternatives[windows], iana)
		}
	}
	return preferred, alternatives
}

func resolveEmbedded(iana string, embedded map[string]bool, aliases zoneAliases) string {
	canonical := iana
	if target, ok := aliases.canonicalOf[iana]; ok {
		canonical = target
	}
	candidates := []string{iana, canonical}
	candidates = append(candidates, aliases.aliasesOf[canonical]...)
	for _, candidate := range candidates {
		if embedded[candidate] {
			return candidate
		}
	}
	return ""
}

func resolveWindowsZones(
	preferred map[string]string,
	alternatives map[string][]string,
	embedded map[string]bool,
	aliases zoneAliases,
) ([]winZonePair, []string) {
	pairs := make([]winZonePair, 0, len(preferred))
	dropped := []string{}
	for windows, preferredIANA := range preferred {
		picked := resolveEmbedded(preferredIANA, embedded, aliases)
		if picked == "" {
			for _, iana := range alternatives[windows] {
				if picked = resolveEmbedded(iana, embedded, aliases); picked != "" {
					break
				}
			}
		}
		if picked == "" {
			dropped = append(dropped, windows)
			continue
		}
		pairs = append(pairs, winZonePair{windows: windows, iana: picked})
	}
	sort.Slice(pairs, func(i, j int) bool { return pairs[i].windows < pairs[j].windows })
	sort.Strings(dropped)
	return pairs, dropped
}

func renderC(pairs []winZonePair, cldrVersion string) string {
	var output strings.Builder
	output.WriteString("// tz_winmap_gen.c -- Windows time-zone key -> IANA name table.\n")
	output.WriteString("//\n")
	output.WriteString("// GENERATED by scripts/gen_win_tzmap.go from CLDR windowsZones\n")
	fmt.Fprintf(&output, "// (typeVersion %s), filtered to Lunar's embedded IANA\n", cldrVersion)
	output.WriteString("// index and canonicalized through tzdata backward links. DO NOT EDIT.\n")
	output.WriteString("//\n")
	output.WriteString("// Sorted by the Windows key (ASCII, so wcscmp order == this byte\n")
	output.WriteString("// order) for binary search in TzWinmap_IanaFromWindows.\n")
	output.WriteString("\n")
	output.WriteString("#include \"tz_winmap.h\"\n")
	output.WriteString("\n")
	output.WriteString("const TzWinMapEntry g_tz_winmap[] = {\n")
	for _, pair := range pairs {
		fmt.Fprintf(&output, "    { L\"%s\", \"%s\" },\n", pair.windows, pair.iana)
	}
	output.WriteString("};\n")
	output.WriteString("\n")
	output.WriteString("const int g_tz_winmap_count =\n")
	output.WriteString("    (int)(sizeof g_tz_winmap / sizeof g_tz_winmap[0]);\n")
	return output.String()
}
