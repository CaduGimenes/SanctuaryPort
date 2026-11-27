// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"encoding/hex"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func titleFixture(t *testing.T) *TitleServices {
	t.Helper()
	root := t.TempDir()
	s, err := NewTitleServices(TitleConfig{PublisherDir: filepath.Join(root, "publisher"), StateDir: filepath.Join(root, "state"), MaxRooms: 4})
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func TestTitlePublisherAndClockNativeRequests(t *testing.T) {
	s := titleFixture(t)
	session := matchIdentity(1, 1)
	// The 15-byte service-specific request recorded at startup: empty
	// context, then Seasons.txt; the lobby already consumed its zero marker.
	request, _ := hex.DecodeString("100010536561736f6e732e74787400")
	result := s.HandleTask(session, 10, 21, request)
	if result.ErrorCode != 0 || len(result.Records) != 1 || result.Total != 1 {
		t.Fatalf("publisher failed: %+v", result)
	}
	r := NewTaggedReader(result.Records[0])
	file := r.Blob(MaxPublisherBytes)
	if r.Done() != nil || !bytes.Equal(file, s.publisher["Seasons.txt"]) || !bytes.Contains(file, []byte("[Season 29]")) {
		t.Fatal("native publisher blob invalid")
	}
	s.now = func() time.Time { return time.Unix(0x12345678, 999999999) }
	result = s.HandleTask(session, 12, 6, nil)
	if result.ErrorCode != 0 || len(result.Records) != 1 || !bytes.Equal(result.Records[0], []byte{8, 0x78, 0x56, 0x34, 0x12}) {
		t.Fatalf("clock failed: %+v", result)
	}
	for _, data := range [][]byte{append(append([]byte{}, request...), 0), request[:len(request)-1], []byte{16, 0, 16, '.', '.', '/', 0}} {
		if got := s.HandleTask(session, 10, 21, data); got.ErrorCode == 0 {
			t.Fatal("invalid file request accepted")
		}
	}
	w := NewTaggedWriter(256)
	w.CString("foreign", 15)
	w.CString("Config.txt", 127)
	data, _ := w.Bytes()
	if s.HandleTask(session, 10, 21, data).ErrorCode == 0 {
		t.Fatal("foreign publisher context accepted")
	}
	if s.HandleTask(session, 12, 6, []byte{0}).ErrorCode == 0 {
		t.Fatal("clock trailing field accepted")
	}
	for _, timestamp := range []int64{-1, 1 << 32} {
		s.now = func() time.Time { return time.Unix(timestamp, 0) }
		if s.HandleTask(session, 12, 6, nil).ErrorCode == 0 {
			t.Fatal("out-of-range clock accepted")
		}
	}
	if s.HandleTask(Session{}, 12, 6, nil).ErrorCode == 0 || s.HandleTask(session, 12, 99, nil).ErrorCode == 0 {
		t.Fatal("invalid identity or unsupported task accepted")
	}
}

func TestPublisherEditsPreservedAndInvalidStartupRejected(t *testing.T) {
	root := t.TempDir()
	_, err := loadPublisherFiles(root)
	if err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(root, "Config.txt")
	custom := []byte("# Operator settings\nEnableDiablo4Advertisement \"0\"\n")
	if err := os.WriteFile(p, custom, 0644); err != nil {
		t.Fatal(err)
	}
	files, err := loadPublisherFiles(root)
	if err != nil || !bytes.Equal(files["Config.txt"], custom) {
		t.Fatal("operator edit overwritten", err)
	}
	for _, bad := range [][]byte{[]byte("UpdateVersion=0\n"), []byte("UpdateVersion \"-1\"\n"), []byte("UpdateVersion \"0\"\x00\n"), bytes.Repeat([]byte("x"), MaxPublisherBytes+1)} {
		if err := os.WriteFile(p, bad, 0644); err != nil {
			t.Fatal(err)
		}
		if _, err := loadPublisherFiles(root); err == nil {
			t.Fatal("invalid publisher file accepted")
		}
		got, _ := os.ReadFile(p)
		if !bytes.Equal(got, bad) {
			t.Fatal("bad operator file overwritten")
		}
	}
}

func TestPrivateSeasonWindows(t *testing.T) {
	b, _ := defaultPublisher.ReadFile("publisher/Seasons.txt")
	lines, err := publisherLines(b)
	if err != nil {
		t.Fatal(err)
	}
	windows, err := parseSeasonWindows(lines)
	if err != nil || len(windows) != 1 || windows[0].id != 29 {
		t.Fatal("invalid private schedule", err)
	}
	now := time.Date(2026, 11, 10, 9, 0, 0, 0, time.UTC)
	if !windows[0].start.Before(now) || !windows[0].end.After(now) {
		t.Fatal("default private season inactive at implementation date")
	}
	for _, bad := range []string{
		strings.ReplaceAll(string(b), "[Season 29]", "[Season 0]"),
		strings.ReplaceAll(string(b), "End \"Tue, 01 Jan 2030 00:00:00 GMT\"", "End \"Thu, 01 Jan 2026 00:00:00 GMT\""),
		strings.ReplaceAll(string(b), "Pre \"Thu, 01 Jan 2026 00:00:00 GMT\"", "Pre \"2026-01-01\""),
		string(b) + "Start \"Fri, 02 Jan 2026 00:00:00 GMT\"\n",
		string(b) + "[Season 29]\n",
		strings.TrimSuffix(string(b), "\n"),
	} {
		if validatePublisher("Seasons.txt", []byte(bad)) == nil {
			t.Fatal("invalid season schedule accepted")
		}
	}
}

func TestEncryptedTitleServicesAndMatchmaking(t *testing.T) {
	title := titleFixture(t)
	auth, _, _ := authFixture(t)
	ticket := authTicket(t, authCall(auth, authBody(t, 46, D3TitleID, testToken)))
	session, _ := auth.Lookup(ticket)
	lobby := NewLobbyServer(auth, title.HandleTask, 2)
	lobby.OnDisconnect = title.OnDisconnect
	lobby.Logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	t.Cleanup(lobby.Close)
	client := connectLobby(t, lobby, session, false)
	for _, name := range append(append([]string(nil), publisherNames...), "challengerift_config.dat") {
		w := NewTaggedWriter(256)
		w.CString("", 15)
		w.CString(name, 127)
		request, _ := w.Bytes()
		r, n, code := client.task(t, 10, 21, request)
		if code != 0 || n != 1 || !bytes.Equal(r.Blob(MaxPublisherBytes), title.publisher[name]) || r.Done() != nil {
			t.Fatal("encrypted publisher request failed", name, code, n)
		}
	}
	r, n, code := client.task(t, 12, 6, nil)
	seconds := r.U32()
	if code != 0 || n != 1 || r.Done() != nil || int64(seconds) < time.Now().Unix()-10 || int64(seconds) > time.Now().Unix()+10 {
		t.Fatal("encrypted clock request failed", code, n)
	}
	counter, _ := hex.DecodeString("0801000000090100000000000000")
	if _, _, code := client.task(t, 23, 1, counter); code != 0 {
		t.Fatal("encrypted counter request failed", code)
	}
	// Native empty-context mailbox request, offset0/max50/two categories
	// 0/1, false query flag; no optional identity.
	mail, _ := hex.DecodeString("1000080000000008320000006a080400000002000000000001000100")
	if r, n, code := client.task(t, 29, 11, mail); code != 0 || n != 0 || r.Done() != nil {
		t.Fatal("encrypted mailbox request failed", code, n)
	}
	if _, n, code := client.task(t, 21, 1, nativeCreate(t)); code != 0 || n != 1 {
		t.Fatal("matchmaking regression", code, n)
	}
}
