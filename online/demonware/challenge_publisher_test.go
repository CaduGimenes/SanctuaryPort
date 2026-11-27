// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"
)

func TestChallengePublisherMatchesEmbeddedNativeDescriptor(t *testing.T) {
	// Exact FileDescriptorProto bytes from the identified native executable,
	// not a schema restating the Go encoder under test. See the evidence report.
	data, err := os.ReadFile("testdata/challenge_descriptor.pb")
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(data)
	if hex.EncodeToString(digest[:]) != "6fce2f6af12b77b57dc0bc7d4afd9e9d967499751859e2e9d18f9a44f4ca995f" {
		t.Fatal("native descriptor identity differs")
	}
	descriptor := &descriptorpb.FileDescriptorProto{}
	if err := proto.Unmarshal(data, descriptor); err != nil {
		t.Fatal(err)
	}
	// HeroCommon.proto is an unused import for this scalar-only message. Keep
	// that recorded dependency without inventing a replacement descriptor.
	file, err := (protodesc.FileOptions{AllowUnresolvable: true}).New(descriptor, nil)
	if err != nil {
		t.Fatal(err)
	}
	message := dynamicpb.NewMessage(file.Messages().ByName("ChallengeData"))
	seed, _ := defaultPublisher.ReadFile("publisher/ChallengeRifts.txt")
	payload, err := encodeChallengePublisher(seed)
	if err != nil {
		t.Fatal(err)
	}
	if err := proto.Unmarshal(payload, message); err != nil || len(message.GetUnknown()) != 0 {
		t.Fatal("native schema rejected generated publisher data", err)
	}
	fields := message.Descriptor().Fields()
	checks := map[protoreflect.Name]any{
		"challenge_number": uint64(2), "challenge_start_unix_time": int64(1767225600),
		"challenge_last_broadcast_unix_time": int64(1767830400), "challenge_end_unix_time_console": int64(1767830400),
		"challenge_hash": uint64(0), "challenge_nephalem_orb_multiplier": float64(1),
	}
	for name, want := range checks {
		field := fields.ByName(name)
		if field == nil || !message.Has(field) {
			t.Fatal("missing native field", name)
		}
		value := message.Get(field)
		var got any
		switch field.Kind() {
		case protoreflect.Uint32Kind, protoreflect.Uint64Kind:
			got = value.Uint()
		case protoreflect.Int64Kind:
			got = value.Int()
		case protoreflect.FloatKind:
			got = value.Float()
		default:
			t.Fatal("unexpected native field kind", name, field.Kind())
		}
		if got != want {
			t.Fatal("native publisher value mismatch", name, got, want)
		}
	}
	if !time.Unix(message.Get(fields.ByName("challenge_end_unix_time_console")).Int(), 0).Before(time.Date(2026, 11, 10, 0, 0, 0, 0, time.UTC)) {
		t.Fatal("default private metadata must describe a historical challenge")
	}
}

func TestChallengePublisherFileAndPreservedSettings(t *testing.T) {
	dir := t.TempDir()
	files, err := loadPublisherFiles(dir)
	if err != nil {
		t.Fatal(err)
	}
	before := append([]byte(nil), files["challengerift_config.dat"]...)
	config := strings.ReplaceAll(string(files["ChallengeRifts.txt"]), `ChallengeNumber "2"`, `ChallengeNumber "3"`)
	if err := os.WriteFile(filepath.Join(dir, "ChallengeRifts.txt"), []byte(config), 0644); err != nil {
		t.Fatal(err)
	}
	updated, err := loadPublisherFiles(dir)
	if err != nil || !bytes.Equal(updated["ChallengeRifts.txt"], []byte(config)) || bytes.Equal(before, updated["challengerift_config.dat"]) {
		t.Fatal("operator challenge metadata not preserved/encoded", err)
	}
	if !bytes.Equal(files["challengerift_config.dat"], before) {
		t.Fatal("startup snapshot changed after an edit")
	}
	title := titleFixture(t)
	w := NewTaggedWriter(256)
	w.CString("", 15)
	w.CString("challengerift_config.dat", 127)
	request, _ := w.Bytes()
	result := title.HandleTask(matchIdentity(1, 1), 10, 21, request)
	if result.ErrorCode != 0 || len(result.Records) != 1 {
		t.Fatal("native challenge file request rejected", result)
	}
	r := NewTaggedReader(result.Records[0])
	if !bytes.Equal(r.Blob(MaxPublisherBytes), before) || r.Done() != nil {
		t.Fatal("native binary publisher blob corrupted")
	}
}

func TestChallengePublisherRejectsInvalidMetadata(t *testing.T) {
	seed, _ := defaultPublisher.ReadFile("publisher/ChallengeRifts.txt")
	for _, invalid := range []string{
		"", string(seed) + "Mystery \"1\"\n", string(seed) + "ChallengeNumber \"3\"\n",
		strings.ReplaceAll(string(seed), `ChallengeNumber "2"`, `ChallengeNumber "0"`),
		strings.ReplaceAll(string(seed), `ChallengeNumber "2"`, `ChallengeNumber "-1"`),
		strings.ReplaceAll(string(seed), `ChallengeNumber "2"`, `ChallengeNumber "2147483648"`),
		strings.ReplaceAll(string(seed), `End "Thu, 08 Jan 2026 00:00:00 GMT"`, `End "Thu, 01 Jan 2026 00:00:00 GMT"`),
		strings.ReplaceAll(string(seed), `LastBroadcast "Thu, 08 Jan 2026 00:00:00 GMT"`, `LastBroadcast "Thu, 15 Jan 2026 00:00:00 GMT"`),
		strings.ReplaceAll(string(seed), `Hash "0"`, `Hash "-1"`),
		strings.ReplaceAll(string(seed), `OrbMultiplier "1"`, `OrbMultiplier "NaN"`),
		strings.ReplaceAll(string(seed), `OrbMultiplier "1"`, `OrbMultiplier "+Inf"`),
		strings.ReplaceAll(string(seed), `OrbMultiplier "1"`, `OrbMultiplier "0"`),
		strings.TrimSuffix(string(seed), "\n"),
	} {
		if _, err := encodeChallengePublisher([]byte(invalid)); err == nil {
			t.Fatal("invalid challenge metadata accepted", invalid)
		}
	}
}
