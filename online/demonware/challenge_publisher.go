// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"errors"
	"fmt"
	"math"
	"strconv"
	"time"

	"google.golang.org/protobuf/encoding/protowire"
)

// ChallengeRifts.proto is embedded in CUSA00242 01.46. The file callback
// requires a present, nonzero challenge_number before the Challenges menu
// can issue its leaderboard tasks. This describes calendar metadata only;
// it does not create downloadable challenge heroes or dungeon content.
func encodeChallengePublisher(data []byte) ([]byte, error) {
	lines, err := publisherLines(data)
	if err != nil {
		return nil, err
	}
	fields := make(map[string]string)
	for _, line := range lines {
		m := publisherField.FindStringSubmatch(line)
		if m == nil {
			return nil, errors.New("expected Key \"value\"")
		}
		if _, exists := fields[m[1]]; exists {
			return nil, fmt.Errorf("duplicate challenge key %s", m[1])
		}
		switch m[1] {
		case "ChallengeNumber", "Start", "LastBroadcast", "End", "Hash", "OrbMultiplier":
			fields[m[1]] = m[2]
		default:
			return nil, fmt.Errorf("unknown challenge key %s", m[1])
		}
	}
	number, err := strconv.ParseUint(fields["ChallengeNumber"], 10, 31)
	if err != nil || number < 2 || strconv.FormatUint(number, 10) != fields["ChallengeNumber"] {
		return nil, errors.New("ChallengeNumber must be an integer from 2 to 2147483647 (current and previous board IDs)")
	}
	dates := make(map[string]time.Time)
	for _, key := range []string{"Start", "LastBroadcast", "End"} {
		date, err := publisherDate(fields[key])
		if err != nil {
			return nil, fmt.Errorf("%s: %w", key, err)
		}
		dates[key] = date
	}
	if !dates["Start"].Before(dates["End"]) || dates["LastBroadcast"].Before(dates["Start"]) || dates["LastBroadcast"].After(dates["End"]) {
		return nil, errors.New("challenge dates need Start <= LastBroadcast <= End and Start < End")
	}
	var hash uint64
	if value, present := fields["Hash"]; present {
		hash, err = strconv.ParseUint(value, 10, 64)
		if err != nil || strconv.FormatUint(hash, 10) != value {
			return nil, errors.New("Hash must be an unsigned decimal uint64")
		}
	}
	multiplier := float32(1)
	if value, present := fields["OrbMultiplier"]; present {
		n, err := strconv.ParseFloat(value, 32)
		if err != nil || math.IsNaN(n) || math.IsInf(n, 0) || n <= 0 || n > 100 {
			return nil, errors.New("OrbMultiplier must be finite, positive and at most 100")
		}
		multiplier = float32(n)
	}
	var out []byte
	for _, field := range []struct {
		number protowire.Number
		value  uint64
	}{{1, number}, {2, uint64(dates["Start"].Unix())}, {3, uint64(dates["LastBroadcast"].Unix())}, {4, uint64(dates["End"].Unix())}, {5, hash}} {
		out = protowire.AppendTag(out, field.number, protowire.VarintType)
		out = protowire.AppendVarint(out, field.value)
	}
	out = protowire.AppendTag(out, 6, protowire.Fixed32Type)
	out = protowire.AppendFixed32(out, math.Float32bits(multiplier))
	return out, nil
}
