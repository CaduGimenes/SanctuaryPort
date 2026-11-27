// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"math"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"time"
)

var publisherField = regexp.MustCompile(`^([A-Za-z][A-Za-z0-9_]*)[ \t]+"([^"\r\n]{0,127})"[ \t]*$`)
var seasonHeader = regexp.MustCompile(`^\[Season ([0-9]{1,10})\]$`)

type seasonWindow struct {
	id              uint32
	pre, start, end time.Time
}

func publisherLines(b []byte) ([]string, error) {
	if len(b) > MaxPublisherBytes {
		return nil, ErrLimit
	}
	for _, c := range b {
		if c != '\n' && c != '\r' && c != '\t' && (c < 32 || c > 126) {
			return nil, errors.New("text must be ASCII without embedded NULs")
		}
	}
	if len(b) > 0 && b[len(b)-1] != '\n' {
		return nil, errors.New("text must end in a newline")
	}
	var lines []string
	scan := bufio.NewScanner(bytes.NewReader(b))
	for scan.Scan() {
		line := strings.TrimSuffix(scan.Text(), "\r")
		if strings.TrimSpace(line) == "" || strings.HasPrefix(line, "#") {
			continue
		}
		// The native season reader does not trim a comment's leading spaces.
		lines = append(lines, line)
	}
	return lines, scan.Err()
}

func validatePublisher(name string, b []byte) error {
	lines, err := publisherLines(b)
	if err != nil {
		return err
	}
	switch name {
	case "ChallengeRifts.txt":
		_, err := encodeChallengePublisher(b)
		return err
	case "Seasons.txt":
		_, err := parseSeasonWindows(lines)
		return err
	case "Config.txt":
		seen := make(map[string]bool)
		for _, line := range lines {
			m := publisherField.FindStringSubmatch(line)
			if m == nil {
				return errors.New("expected Key \"value\"")
			}
			if seen[m[1]] {
				return fmt.Errorf("duplicate configuration key %s", m[1])
			}
			seen[m[1]] = true
			switch m[1] {
			case "SeasonalGlobalLeaderboardsEnabled", "EnableCrossPlatformSaveMigration", "EnableDiablo4Advertisement", "CommunityBuffSeasonOnly":
				if m[2] != "0" && m[2] != "1" {
					return fmt.Errorf("%s must be 0 or 1", m[1])
				}
			case "HeroPublishFrequencyMinutes", "MailTimeoutDaysMail", "MailTimeoutDaysAvengers", "UpdateVersion":
				n, e := strconv.ParseUint(m[2], 10, 31)
				if e != nil || strconv.FormatUint(n, 10) != m[2] {
					return fmt.Errorf("%s must be a nonnegative integer", m[1])
				}
			case "CommunityBuffStart", "CommunityBuffEnd":
				if _, err := publisherDate(m[2]); err != nil {
					return fmt.Errorf("%s: %w", m[1], err)
				}
			default:
				return fmt.Errorf("unsupported configuration key %s", m[1])
			}
		}
		return nil
	case "Blacklist.txt":
		// A comment-only blacklist explicitly clears both native maps. Entry
		// syntax must be established before accepting operator rules here.
		if len(lines) != 0 {
			return errors.New("this build supports an empty blacklist (comments only)")
		}
		return nil
	default:
		return errors.New("unknown publisher file")
	}
}

func publisherDate(value string) (time.Time, error) {
	t, err := time.Parse(http.TimeFormat, value)
	if err != nil || t.Unix() < 0 || t.Unix() > math.MaxUint32 {
		return time.Time{}, errors.New("expected a GMT HTTP date within the native Unix timestamp range")
	}
	return t, nil
}

func parseSeasonWindows(lines []string) ([]seasonWindow, error) {
	var seasons []seasonWindow
	seen := make(map[uint32]bool)
	fields := make(map[string]bool)
	for _, line := range lines {
		if m := seasonHeader.FindStringSubmatch(line); m != nil {
			n, err := strconv.ParseUint(m[1], 10, 32)
			if err != nil || n == 0 || seen[uint32(n)] {
				return nil, errors.New("season IDs must be unique nonzero uint32 values")
			}
			seen[uint32(n)] = true
			seasons = append(seasons, seasonWindow{id: uint32(n)})
			fields = make(map[string]bool)
			continue
		}
		m := publisherField.FindStringSubmatch(line)
		if m == nil || len(seasons) == 0 || fields[m[1]] {
			return nil, errors.New("invalid or duplicate season field")
		}
		fields[m[1]] = true
		t, err := publisherDate(m[2])
		if err != nil {
			return nil, err
		}
		s := &seasons[len(seasons)-1]
		switch m[1] {
		case "Pre":
			s.pre = t
		case "Start":
			s.start = t
		case "End":
			s.end = t
		default:
			return nil, fmt.Errorf("unknown season key %s", m[1])
		}
	}
	if len(seasons) == 0 {
		return nil, errors.New("at least one season with Pre, Start and End is required")
	}
	for i, s := range seasons {
		if s.pre.IsZero() || s.start.IsZero() || s.end.IsZero() || s.pre.After(s.start) || !s.start.Before(s.end) {
			return nil, fmt.Errorf("season %d needs ordered Pre <= Start < End", s.id)
		}
		if i > 0 && (s.id <= seasons[i-1].id || s.pre.Before(seasons[i-1].end)) {
			return nil, errors.New("seasons must increase in ID and have non-overlapping windows")
		}
	}
	return seasons, nil
}
