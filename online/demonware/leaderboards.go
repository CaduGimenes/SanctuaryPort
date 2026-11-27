// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"errors"
	"fmt"
	"sort"
	"strings"
	"sync"
)

const (
	maxLeaderboardRows           = 16384
	maxLeaderboardBoards         = 1024
	maxLeaderboardRowsPerAccount = 1024
	maxLeaderboardEntities       = 64
	maxLeaderboardQueries        = 1024
	maxLeaderboardColumns        = 64
	maxLeaderboardWriteBatch     = 64
	maxLeaderboardBlob           = 8192
	maxLeaderboardReplyBytes     = DefaultMaxFrame - 256
	maxLeaderboardPage           = 100
)

// LeaderboardEntry stores a real client submission and its authenticated owner.
// Native extension fields retain object offsets until their meaning is known.
// OwnerAccountID and WriteType are storage metadata, not read-reply fields.
type LeaderboardEntry struct {
	OwnerAccountID uint64 `json:"owner_account_id"`
	BoardID        uint32 `json:"board_id"`
	EntityID       uint64 `json:"entity_id"`
	Name           string `json:"name"`
	WriteType      uint8  `json:"write_type"`
	Score          int64  `json:"score"`
	Field7C        uint32 `json:"field_7c"`
	Field80        uint64 `json:"field_80"`
	Field88        int64  `json:"field_88"`
	Blob           []byte `json:"blob"`
}

type leaderboardSnapshot struct {
	Version int                `json:"version"`
	Entries []LeaderboardEntry `json:"entries"`
}

type LeaderboardStore struct {
	mu   sync.Mutex
	path string
	data leaderboardSnapshot
}

func validateLeaderboardSnapshot(snapshot leaderboardSnapshot) error {
	if snapshot.Version != 1 || snapshot.Entries == nil || len(snapshot.Entries) > maxLeaderboardRows {
		return ErrValue
	}
	type key struct {
		board  uint32
		entity uint64
	}
	seen := make(map[key]struct{}, len(snapshot.Entries))
	boards := make(map[uint32]struct{})
	counts := make(map[uint64]int)
	owners := make(map[uint64]uint64)
	for _, row := range snapshot.Entries {
		if row.OwnerAccountID == 0 || row.EntityID != row.OwnerAccountID || !leaderboardWriteTypeSupported(row.WriteType) || len(row.Name) > 63 || strings.IndexByte(row.Name, 0) >= 0 || len(row.Blob) > maxLeaderboardBlob {
			return ErrValue
		}
		k := key{row.BoardID, row.EntityID}
		if _, exists := seen[k]; exists {
			return ErrValue
		}
		seen[k] = struct{}{}
		if owner, exists := owners[row.EntityID]; exists && owner != row.OwnerAccountID {
			return ErrValue
		}
		owners[row.EntityID] = row.OwnerAccountID
		boards[row.BoardID] = struct{}{}
		counts[row.OwnerAccountID]++
		if len(boards) > maxLeaderboardBoards || counts[row.OwnerAccountID] > maxLeaderboardRowsPerAccount {
			return ErrLimit
		}
	}
	return nil
}

func leaderboardWriteTypeSupported(kind uint8) bool { return kind == 1 || kind == 4 }

func NewLeaderboardStore(path string) (*LeaderboardStore, error) {
	s := &LeaderboardStore{path: path}
	exists, err := readTitleStore(path, &s.data)
	if err != nil {
		return nil, fmt.Errorf("load leaderboards: %w", err)
	}
	if !exists {
		s.data = leaderboardSnapshot{Version: 1, Entries: make([]LeaderboardEntry, 0)}
	}
	if err := validateLeaderboardSnapshot(s.data); err != nil {
		return nil, fmt.Errorf("load leaderboards: %w", err)
	}
	if !exists {
		if err := writeTitleStore(path, s.data); err != nil {
			return nil, fmt.Errorf("create leaderboards: %w", err)
		}
	}
	return s, nil
}

func leaderboardFullRecord(row LeaderboardEntry, rank uint64) ([]byte, error) {
	w := NewTaggedWriter(maxLeaderboardBlob + 256)
	w.U64(row.EntityID)
	w.I64(row.Score)
	w.U64(rank)
	w.CString(row.Name, 63)
	w.U32(row.Field7C)
	w.U64(row.Field80)
	w.I64(row.Field88)
	w.Blob(row.Blob, maxLeaderboardBlob)
	return w.Bytes()
}

func leaderboardEntities(r *TaggedReader) []uint64 {
	n := r.U32()
	if r.Err() != nil || n == 0 || n > maxLeaderboardEntities {
		r.err = ErrLimit
		return nil
	}
	ids := make([]uint64, n)
	for i := range ids {
		ids[i] = r.U64()
		if ids[i] == 0 && r.err == nil {
			r.err = ErrValue
		}
	}
	return ids
}

// leaderboardRanks assigns deterministic private-board ranks. Higher submitted
// scores sort first; entity ID breaks ties. Retail per-board rules are absent.
func leaderboardRanks(entries []LeaderboardEntry) map[uint32]map[uint64]uint64 {
	boards := make(map[uint32][]LeaderboardEntry)
	for _, row := range entries {
		boards[row.BoardID] = append(boards[row.BoardID], row)
	}
	ranks := make(map[uint32]map[uint64]uint64, len(boards))
	for id, rows := range boards {
		sort.Slice(rows, func(i, j int) bool {
			if rows[i].Score != rows[j].Score {
				return rows[i].Score > rows[j].Score
			}
			return rows[i].EntityID < rows[j].EntityID
		})
		board := make(map[uint64]uint64, len(rows))
		for i, row := range rows {
			board[row.EntityID] = uint64(i + 1)
		}
		ranks[id] = board
	}
	return ranks
}

func (s *LeaderboardStore) HandleTask(session Session, task uint8, data []byte) TaskResult {
	if !titleStoreSessionValid(session) {
		return titleStoreFailure(errors.New("leaderboard account unavailable"))
	}
	switch task {
	case 1:
		return s.write(session, data)
	case 4, 5:
		return s.page(task, data)
	case 11, 13:
		return s.read(task, data)
	default:
		return titleStoreFailure(fmt.Errorf("unsupported leaderboard task %d", task))
	}
}

// The native title uses task 4 for rank pages and task 5 for a page around an
// entity. Both bind the same complete row decoder as task 13. Private ranks
// start at one; zero is accepted as the beginning of a board.
func (s *LeaderboardStore) page(task uint8, data []byte) TaskResult {
	r := NewTaggedReader(data)
	board, start, limit, columns := r.U32(), r.U64(), r.U32(), r.U32()
	if r.Err() != nil || limit == 0 || limit > maxLeaderboardPage || columns > maxLeaderboardColumns {
		return titleStoreFailure(ErrLimit)
	}
	for i := uint32(0); i < columns; i++ {
		r.CString(64)
	}
	if err := r.Done(); err != nil {
		return titleStoreFailure(fmt.Errorf("invalid leaderboard page: %w", err))
	}
	if columns != 0 {
		return titleStoreFailure(errors.New("named leaderboard columns are not supported by the recovered title contract"))
	}
	s.mu.Lock()
	rows := make([]LeaderboardEntry, 0)
	for _, row := range s.data.Entries {
		if row.BoardID == board {
			rows = append(rows, row)
		}
	}
	s.mu.Unlock()
	sort.Slice(rows, func(i, j int) bool {
		if rows[i].Score != rows[j].Score {
			return rows[i].Score > rows[j].Score
		}
		return rows[i].EntityID < rows[j].EntityID
	})
	var offset uint64
	if task == 5 {
		found := false
		for i, row := range rows {
			if row.EntityID == start {
				found = true
				if i > int(limit/2) {
					offset = uint64(i - int(limit/2))
				}
				break
			}
		}
		if !found {
			return TaskResult{}
		}
	} else if start > 0 {
		offset = start - 1
	}
	result := TaskResult{Total: uint32(len(rows))}
	var resultBytes int
	for i := offset; i < uint64(len(rows)) && len(result.Records) < int(limit); i++ {
		record, err := leaderboardFullRecord(rows[i], i+1)
		if err != nil {
			return titleStoreFailure(err)
		}
		if len(record) > maxLeaderboardReplyBytes-resultBytes {
			return titleStoreFailure(ErrLimit)
		}
		resultBytes += len(record)
		result.Records = append(result.Records, record)
	}
	return result
}

func (s *LeaderboardStore) read(task uint8, data []byte) TaskResult {
	r := NewTaggedReader(data)
	var entities []uint64
	var boards []uint32
	if task == 11 {
		entities = leaderboardEntities(r)
		n := r.U32()
		if r.Err() != nil || n == 0 || n > maxLeaderboardQueries || uint64(n)*uint64(len(entities)) > maxLeaderboardQueries {
			return titleStoreFailure(ErrLimit)
		}
		boards = make([]uint32, n)
		for i := range boards {
			boards[i] = r.U32()
		}
	} else {
		boards = []uint32{r.U32()}
		entities = leaderboardEntities(r)
		n := r.U32()
		if r.Err() != nil || n > maxLeaderboardColumns {
			return titleStoreFailure(ErrLimit)
		}
		for i := uint32(0); i < n; i++ {
			r.CString(64)
		}
		if n != 0 && r.Err() == nil {
			return titleStoreFailure(errors.New("named leaderboard columns are not supported by the recovered title contract"))
		}
	}
	if err := r.Done(); err != nil {
		return titleStoreFailure(fmt.Errorf("invalid leaderboard read: %w", err))
	}
	s.mu.Lock()
	entries := append([]LeaderboardEntry(nil), s.data.Entries...)
	s.mu.Unlock()
	ranks := leaderboardRanks(entries)
	type key struct {
		board  uint32
		entity uint64
	}
	rows := make(map[key]LeaderboardEntry, len(entries))
	for _, row := range entries {
		rows[key{row.BoardID, row.EntityID}] = row
	}
	result := TaskResult{}
	var resultBytes int
	for _, board := range boards {
		for _, entity := range entities {
			row, exists := rows[key{board, entity}]
			var record []byte
			var err error
			if task == 11 {
				w := NewTaggedWriter(maxLeaderboardBlob + 272)
				w.U32(board)
				if exists {
					w.U32(0)
					var full []byte
					full, err = leaderboardFullRecord(row, ranks[board][entity])
					if err == nil {
						w.Raw(full)
					}
				} else {
					// Private missing-row status. Native e210e0 accepts a nonzero
					// status followed by entity only; retail enum is unknown.
					w.U32(1)
					w.U64(entity)
				}
				if err == nil {
					record, err = w.Bytes()
				}
			} else {
				if !exists {
					continue
				}
				record, err = leaderboardFullRecord(row, ranks[board][entity])
			}
			if err != nil {
				return titleStoreFailure(err)
			}
			if len(record) > maxLeaderboardReplyBytes-resultBytes {
				return titleStoreFailure(ErrLimit)
			}
			resultBytes += len(record)
			result.Records = append(result.Records, record)
		}
	}
	result.Total = uint32(len(result.Records))
	return result
}

func (s *LeaderboardStore) write(session Session, data []byte) TaskResult {
	r := NewTaggedReader(data)
	changes := make([]LeaderboardEntry, 0, 4)
	for r.Remaining() != 0 && r.Err() == nil {
		if len(changes) >= maxLeaderboardWriteBatch {
			return titleStoreFailure(ErrLimit)
		}
		row := LeaderboardEntry{OwnerAccountID: session.Identity.AccountID, Name: session.Identity.Name}
		row.BoardID, row.EntityID, row.WriteType, row.Score = r.U32(), r.U64(), r.U8(), r.I64()
		row.Field80, row.Field88 = r.U64(), r.I64()
		row.Blob = append([]byte(nil), r.Blob(maxLeaderboardBlob)...)
		if row.EntityID != session.Identity.AccountID && r.err == nil {
			return titleStoreFailure(errors.New("leaderboard entity must match authenticated account"))
		}
		if !leaderboardWriteTypeSupported(row.WriteType) && r.err == nil {
			return titleStoreFailure(errors.New("leaderboard write type is unsupported by the recovered title contract"))
		}
		changes = append(changes, row)
	}
	if err := r.Done(); err != nil {
		return titleStoreFailure(fmt.Errorf("invalid leaderboard write: %w", err))
	}
	if len(changes) == 0 {
		return titleStoreFailure(ErrValue)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	next := leaderboardSnapshot{Version: 1, Entries: append(make([]LeaderboardEntry, 0, len(s.data.Entries)+len(changes)), s.data.Entries...)}
	for _, row := range changes {
		replaced := false
		for i, old := range next.Entries {
			if old.EntityID == row.EntityID && old.OwnerAccountID != session.Identity.AccountID {
				return titleStoreFailure(errors.New("leaderboard entity belongs to another account"))
			}
			if old.BoardID == row.BoardID && old.EntityID == row.EntityID {
				// Type 4 is emitted by the native seasonal score creator. Keep
				// the best score under this private master's descending policy;
				// type 1 replaces the row. Equal scores retain their metadata.
				if row.WriteType == 1 || row.Score > old.Score {
					next.Entries[i] = row
				}
				replaced = true
				break
			}
		}
		if !replaced {
			next.Entries = append(next.Entries, row)
		}
	}
	if err := validateLeaderboardSnapshot(next); err != nil {
		return titleStoreFailure(err)
	}
	if err := writeTitleStore(s.path, next); err != nil {
		return titleStoreFailure(fmt.Errorf("persist leaderboard scores: %w", err))
	}
	s.data = next
	return TaskResult{}
}
