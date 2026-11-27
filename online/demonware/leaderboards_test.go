// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

func leaderboardWriteFields(rows ...LeaderboardEntry) []byte {
	w := NewTaggedWriter(DefaultMaxFrame)
	for _, row := range rows {
		w.U32(row.BoardID)
		w.U64(row.EntityID)
		writeType := row.WriteType
		if writeType == 0 {
			writeType = 1
		}
		w.U8(writeType)
		w.I64(row.Score)
		w.U64(row.Field80)
		w.I64(row.Field88)
		w.Blob(row.Blob, maxLeaderboardBlob)
	}
	b, err := w.Bytes()
	if err != nil {
		panic(err)
	}
	return b
}

func leaderboardReadFields(task uint8, entities []uint64, boards []uint32, columns ...string) []byte {
	w := NewTaggedWriter(DefaultMaxFrame)
	if task == 13 {
		w.U32(boards[0])
	}
	w.U32(uint32(len(entities)))
	for _, id := range entities {
		w.U64(id)
	}
	if task == 11 {
		w.U32(uint32(len(boards)))
		for _, id := range boards {
			w.U32(id)
		}
	} else {
		w.U32(uint32(len(columns)))
		for _, name := range columns {
			w.CString(name, 64)
		}
	}
	b, err := w.Bytes()
	if err != nil {
		panic(err)
	}
	return b
}

func TestLeaderboardNativeWriteReadAndPersistence(t *testing.T) {
	path := filepath.Join(t.TempDir(), "leaderboards.json")
	s, err := NewLeaderboardStore(path)
	if err != nil {
		t.Fatal(err)
	}
	session := matchIdentity(42, 1)
	session.Identity.Name = "Alice"
	// Native writer order: board U32, entity U64, write-type U8, score I64,
	// opaque extension U64/I64/blob. This literal is independent of the writer.
	write, _ := hex.DecodeString("08640000000a2a000000000000000301090a000000000000000a070000000000000009feffffffffffffff130802000000aabb")
	if got := s.HandleTask(session, 1, write); got.ErrorCode != 0 {
		t.Fatal(got.Reason)
	}
	reopened, err := NewLeaderboardStore(path)
	if err != nil {
		t.Fatal(err)
	}
	read, _ := hex.DecodeString("086400000008010000000a2a000000000000000800000000")
	got := reopened.HandleTask(matchIdentity(43, 1), 13, read)
	if got.ErrorCode != 0 || len(got.Records) != 1 || got.Total != 1 {
		t.Fatal(got)
	}
	want, _ := hex.DecodeString("0a2a00000000000000090a000000000000000a010000000000000010416c6963650008000000000a070000000000000009feffffffffffffff130802000000aabb")
	if !bytes.Equal(got.Records[0], want) {
		t.Fatalf("native full row\n got %x\nwant %x", got.Records[0], want)
	}
	matrix, _ := hex.DecodeString("08010000000a2a0000000000000008010000000864000000")
	got = reopened.HandleTask(session, 11, matrix)
	if got.ErrorCode != 0 || len(got.Records) != 1 {
		t.Fatal(got)
	}
	prefix, _ := hex.DecodeString("08640000000800000000")
	if !bytes.Equal(got.Records[0], append(prefix, want...)) {
		t.Fatalf("native matrix row %x", got.Records[0])
	}
}

func TestLeaderboardMatrixMissingAndRanks(t *testing.T) {
	s, err := NewLeaderboardStore("")
	if err != nil {
		t.Fatal(err)
	}
	for _, row := range []struct {
		owner, entity uint64
		score         int64
	}{{100, 100, 10}, {200, 200, 30}, {300, 300, 30}} {
		if got := s.HandleTask(matchIdentity(row.owner, 1), 1, leaderboardWriteFields(LeaderboardEntry{BoardID: 7, EntityID: row.entity, Score: row.score})); got.ErrorCode != 0 {
			t.Fatal(got.Reason)
		}
	}
	got := s.HandleTask(matchIdentity(4, 1), 11, leaderboardReadFields(11, []uint64{100, 200, 300, 999}, []uint32{7, 8}))
	if got.ErrorCode != 0 || len(got.Records) != 8 || got.Total != 8 {
		t.Fatal(got)
	}
	for i, encoded := range got.Records {
		r := NewTaggedReader(encoded)
		board, status := r.U32(), r.U32()
		entity := r.U64()
		if board == 8 || entity == 999 {
			if status == 0 || r.Done() != nil {
				t.Fatal("missing matrix cell malformed", i, status, r.Err())
			}
			continue
		}
		if status != 0 {
			t.Fatal("existing row missing", i, status)
		}
		r.I64()
		rank := r.U64()
		want := map[uint64]uint64{100: 3, 200: 1, 300: 2}[entity]
		if rank != want {
			t.Fatal("private score ranking mismatch", entity, rank, want)
		}
	}
	if got := s.HandleTask(matchIdentity(4, 1), 13, leaderboardReadFields(13, []uint64{999}, []uint32{7})); got.ErrorCode != 0 || len(got.Records) != 0 || got.Total != 0 {
		t.Fatal("nonexistent score fabricated", got)
	}
}

func TestLeaderboardOwnershipMalformedAndPersistenceFailureAtomic(t *testing.T) {
	s, err := NewLeaderboardStore(filepath.Join(t.TempDir(), "leaderboards.json"))
	if err != nil {
		t.Fatal(err)
	}
	original := LeaderboardEntry{BoardID: 7, EntityID: 42, Score: 10, Blob: []byte{1, 2, 3}}
	if got := s.HandleTask(matchIdentity(42, 1), 1, leaderboardWriteFields(original)); got.ErrorCode != 0 {
		t.Fatal(got.Reason)
	}
	before, err := os.ReadFile(s.path)
	if err != nil {
		t.Fatal(err)
	}
	for _, fields := range [][]byte{
		leaderboardWriteFields(LeaderboardEntry{BoardID: 7, EntityID: 43, Score: 20}, original),
		leaderboardWriteFields(LeaderboardEntry{BoardID: 8, EntityID: 42, Score: 20}),
	} {
		if got := s.HandleTask(matchIdentity(43, 1), 1, fields); got.ErrorCode == 0 {
			t.Fatal("foreign owned entity write accepted")
		}
		if len(s.data.Entries) != 1 || s.data.Entries[0].Score != 10 {
			t.Fatal("failed batch changed rows")
		}
	}
	valid := leaderboardWriteFields(original)
	for n := 0; n < len(valid); n++ {
		if got := s.HandleTask(matchIdentity(42, 1), 1, valid[:n]); got.ErrorCode == 0 {
			t.Fatal("truncated write accepted", n)
		}
	}
	if got := s.HandleTask(matchIdentity(42, 1), 1, append(append([]byte(nil), valid...), 0)); got.ErrorCode == 0 {
		t.Fatal("trailing write accepted")
	}
	current, err := os.ReadFile(s.path)
	if err != nil || !bytes.Equal(before, current) {
		t.Fatal("invalid writes changed disk", err)
	}
	s.path = t.TempDir()
	changed := original
	changed.Score = 20
	if got := s.HandleTask(matchIdentity(42, 1), 1, leaderboardWriteFields(changed)); got.ErrorCode == 0 {
		t.Fatal("failed disk write reported success")
	}
	if s.data.Entries[0].Score != 10 {
		t.Fatal("failed disk write changed live score")
	}
}

func TestLeaderboardReadLimitsAndAuthentication(t *testing.T) {
	s, err := NewLeaderboardStore("")
	if err != nil {
		t.Fatal(err)
	}
	for _, task := range []uint8{11, 13} {
		valid := leaderboardReadFields(task, []uint64{100}, []uint32{7})
		for n := 0; n < len(valid); n++ {
			if got := s.HandleTask(matchIdentity(42, 1), task, valid[:n]); got.ErrorCode == 0 {
				t.Fatal("truncated read accepted", task, n)
			}
		}
		if got := s.HandleTask(matchIdentity(42, 1), task, append(append([]byte(nil), valid...), 0)); got.ErrorCode == 0 {
			t.Fatal("trailing read accepted", task)
		}
		if got := s.HandleTask(Session{}, task, valid); got.ErrorCode == 0 {
			t.Fatal("unauthenticated read accepted")
		}
	}
	for _, fields := range [][]byte{
		leaderboardReadFields(11, nil, []uint32{7}),
		leaderboardReadFields(11, []uint64{100}, nil),
		leaderboardReadFields(11, make([]uint64, maxLeaderboardEntities+1), []uint32{7}),
		leaderboardReadFields(11, []uint64{100, 200}, make([]uint32, maxLeaderboardQueries)),
	} {
		if got := s.HandleTask(matchIdentity(42, 1), 11, fields); got.ErrorCode == 0 {
			t.Fatal("unbounded read accepted")
		}
	}
	if got := s.HandleTask(matchIdentity(42, 1), 255, nil); got.ErrorCode == 0 {
		t.Fatal("unsupported task accepted")
	}
}

func TestLeaderboardReplySizeBoundAndConcurrentWrites(t *testing.T) {
	s, err := NewLeaderboardStore("")
	if err != nil {
		t.Fatal(err)
	}
	entities := make([]uint64, 64)
	for i := range entities {
		entities[i] = uint64(i + 1)
	}
	for _, board := range []uint32{7, 8} {
		for i := 0; i < 64; i++ {
			row := LeaderboardEntry{BoardID: board, EntityID: uint64(i + 1), Blob: bytes.Repeat([]byte{1}, maxLeaderboardBlob)}
			if got := s.HandleTask(matchIdentity(uint64(i+1), 1), 1, leaderboardWriteFields(row)); got.ErrorCode != 0 {
				t.Fatal(got.Reason)
			}
		}
	}
	if got := s.HandleTask(matchIdentity(42, 1), 11, leaderboardReadFields(11, entities, []uint32{7, 8})); got.ErrorCode == 0 || len(got.Records) != 0 {
		t.Fatal("oversize response reached lobby encoder", got.ErrorCode, len(got.Records))
	}
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			for j := 0; j < 10; j++ {
				row := LeaderboardEntry{BoardID: 9, EntityID: uint64(i + 1), Score: int64(j)}
				if got := s.HandleTask(matchIdentity(uint64(i+1), 1), 1, leaderboardWriteFields(row)); got.ErrorCode != 0 {
					t.Error(got.Reason)
					return
				}
				s.HandleTask(matchIdentity(42, 1), 13, leaderboardReadFields(13, []uint64{uint64(i + 1)}, []uint32{9}))
			}
		}(i)
	}
	wg.Wait()
	if len(s.data.Entries) != 136 {
		t.Fatal("concurrent rows lost", len(s.data.Entries))
	}
}

func TestLeaderboardRejectsCorruptSnapshot(t *testing.T) {
	path := filepath.Join(t.TempDir(), "leaderboards.json")
	for _, doc := range []string{"null", "{}", `{"version":1,"entries":null}`, `{"version":2,"entries":[]}`, `{"version":1,"entries":[],"extra":true}`, `{"version":1,"entries":[]} {}`} {
		if err := os.WriteFile(path, []byte(doc), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := NewLeaderboardStore(path); err == nil {
			t.Fatal("corrupt leaderboard store accepted", doc)
		}
	}
	rows := leaderboardSnapshot{Version: 1, Entries: []LeaderboardEntry{
		{OwnerAccountID: 42, BoardID: 7, EntityID: 42, WriteType: 1, Score: math.MaxInt64},
		{OwnerAccountID: 43, BoardID: 8, EntityID: 42, WriteType: 1, Score: math.MinInt64},
	}}
	b, err := json.Marshal(rows)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, b, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewLeaderboardStore(path); err == nil {
		t.Fatal("conflicting entity ownership accepted")
	}
}

func leaderboardPageFields(board uint32, start uint64, limit uint32) []byte {
	w := NewTaggedWriter(64)
	w.U32(board)
	w.U64(start)
	w.U32(limit)
	w.U32(0)
	b, _ := w.Bytes()
	return b
}

func TestLeaderboardSeasonalBestAndRankPages(t *testing.T) {
	s, err := NewLeaderboardStore("")
	if err != nil {
		t.Fatal(err)
	}
	for i, score := range []int64{20, 40, 60, 80, 100} {
		id := uint64(i + 1)
		row := LeaderboardEntry{BoardID: 29, EntityID: id, WriteType: 4, Score: score, Blob: []byte{byte(id)}}
		if got := s.HandleTask(matchIdentity(id, 1), 1, leaderboardWriteFields(row)); got.ErrorCode != 0 {
			t.Fatal(got.Reason)
		}
	}
	// A later worse seasonal run cannot erase the best run's score/metadata.
	for _, score := range []int64{1, 60} {
		row := LeaderboardEntry{BoardID: 29, EntityID: 3, WriteType: 4, Score: score, Blob: []byte{99}}
		if got := s.HandleTask(matchIdentity(3, 1), 1, leaderboardWriteFields(row)); got.ErrorCode != 0 {
			t.Fatal(got.Reason)
		}
	}
	for _, task := range []uint8{4, 5} {
		got := s.HandleTask(matchIdentity(42, 1), task, leaderboardPageFields(29, 3, 3))
		if got.ErrorCode != 0 || got.Total != 5 || len(got.Records) != 3 {
			t.Fatalf("page task %d: %+v", task, got)
		}
		wantIDs := []uint64{3, 2, 1}
		if task == 5 {
			wantIDs = []uint64{4, 3, 2}
		}
		for i, record := range got.Records {
			r := NewTaggedReader(record)
			id, score, rank := r.U64(), r.I64(), r.U64()
			r.CString(63)
			r.U32()
			r.U64()
			r.I64()
			blob := r.Blob(8192)
			if id != wantIDs[i] || score != int64(id*20) || rank != 6-id || !bytes.Equal(blob, []byte{byte(id)}) || r.Done() != nil {
				t.Fatal("rank/pivot page corrupted", task, id, score, rank, blob)
			}
		}
		request := leaderboardPageFields(29, 3, 3)
		for n := 0; n < len(request); n++ {
			if got := s.HandleTask(matchIdentity(42, 1), task, request[:n]); got.ErrorCode == 0 {
				t.Fatal("truncated page accepted", task, n)
			}
		}
		for _, request := range [][]byte{append(request, 0), leaderboardPageFields(29, 3, maxLeaderboardPage+1)} {
			if got := s.HandleTask(matchIdentity(42, 1), task, request); got.ErrorCode == 0 {
				t.Fatal("invalid page accepted", task)
			}
		}
	}
	// Starts beyond the end, including the compact max-U64 sentinel, are
	// valid empty pages. An absent pivot also returns no invented score.
	for _, task := range []uint8{4, 5} {
		if got := s.HandleTask(matchIdentity(42, 1), task, leaderboardPageFields(29, math.MaxUint64, 3)); got.ErrorCode != 0 || len(got.Records) != 0 {
			t.Fatal("end/absent pivot page invalid", task, got)
		}
	}
	row := LeaderboardEntry{BoardID: 29, EntityID: 3, WriteType: 4, Score: 110, Blob: []byte{8}}
	if got := s.HandleTask(matchIdentity(3, 1), 1, leaderboardWriteFields(row)); got.ErrorCode != 0 {
		t.Fatal(got.Reason)
	}
	got := s.HandleTask(matchIdentity(42, 1), 4, leaderboardPageFields(29, 1, 1))
	r := NewTaggedReader(got.Records[0])
	if r.U64() != 3 || r.I64() != 110 || r.U64() != 1 {
		t.Fatal("improved seasonal result did not reach first rank")
	}
	row.WriteType, row.Score = 1, -1
	if got := s.HandleTask(matchIdentity(3, 1), 1, leaderboardWriteFields(row)); got.ErrorCode != 0 {
		t.Fatal("type 1 replacement failed", got.Reason)
	}
	got = s.HandleTask(matchIdentity(42, 1), 13, leaderboardReadFields(13, []uint64{3}, []uint32{29}))
	r = NewTaggedReader(got.Records[0])
	if r.U64() != 3 || r.I64() != -1 || r.U64() != 5 {
		t.Fatal("type 1 replacement did not update ranks")
	}
}
