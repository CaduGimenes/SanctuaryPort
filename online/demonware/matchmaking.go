// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"crypto/rand"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"math"
	"sort"
	"sync"
)

var (
	ErrMatchmakingInvalidRequest = errors.New("invalid matchmaking request")
	ErrMatchmakingInvalidSession = errors.New("unknown matchmaking session")
	ErrMatchmakingForbidden      = errors.New("matchmaking session belongs to another connection")
	ErrMatchmakingCapacity       = errors.New("matchmaking capacity reached")
	ErrMatchmakingUnsupported    = errors.New("unsupported matchmaking task")
	ErrMatchmakingUnavailable    = errors.New("matchmaking service unavailable")
)

// MatchInfo is this title's bdSessionInfo extension. Attrs retain the native
// field order instead of assigning unverified names to game-specific values.
// Attrs[4] is the unsigned open-slot count; all other attributes are signed.
// HostAddress, SecurityID and SecurityKey belong to the game's native peer
// transport. The master retains their bytes without rewriting addresses/keys.
type MatchInfo struct {
	HostAddress []byte
	Kind        uint32
	MaxPlayers  uint32
	SecurityID  [8]byte
	SecurityKey [16]byte
	Attrs       [11]int64
}

func readMatchInfo(r *TaggedReader) MatchInfo {
	v := MatchInfo{HostAddress: r.Blob(255), Kind: r.U32(), MaxPlayers: r.U32()}
	for i := 0; i < 3; i++ {
		v.Attrs[i] = int64(r.I32())
	}
	security := r.Blob(8)
	key := r.Blob(16)
	if r.err == nil && (len(security) != 8 || len(key) != 16) {
		r.err = ErrValue
	}
	copy(v.SecurityID[:], security)
	copy(v.SecurityKey[:], key)
	v.Attrs[3] = int64(r.I32())
	v.Attrs[4] = int64(r.U32())
	for i := 5; i < len(v.Attrs); i++ {
		v.Attrs[i] = int64(r.I32())
	}
	if r.err == nil && (len(v.HostAddress) == 0 || v.MaxPlayers < 1 || v.MaxPlayers > 4 ||
		v.Attrs[4] > int64(v.MaxPlayers)) {
		r.err = ErrValue
	}
	return v
}

func writeMatchAttrs(w *TaggedWriter, v MatchInfo) {
	for i := 0; i < 3; i++ {
		w.I32(int32(v.Attrs[i]))
	}
	w.Blob(v.SecurityID[:], 8)
	w.Blob(v.SecurityKey[:], 16)
	w.I32(int32(v.Attrs[3]))
	w.U32(uint32(v.Attrs[4]))
	for i := 5; i < len(v.Attrs); i++ {
		w.I32(int32(v.Attrs[i]))
	}
}

func matchResult(id [8]byte, v MatchInfo) ([]byte, error) {
	w := NewTaggedWriter(1024)
	w.Blob(v.HostAddress, 255)
	w.Blob(id[:], 8)
	w.U32(v.Kind)
	w.U32(v.MaxPlayers)
	w.U32(v.MaxPlayers - uint32(v.Attrs[4]))
	writeMatchAttrs(w, v)
	return w.Bytes()
}

type matchQuery struct {
	id, offset, limit uint32
	fields            [12]int64
}

func readMatchQuery(r *TaggedReader) matchQuery {
	q := matchQuery{id: r.U32(), offset: r.U32(), limit: r.U32()}
	for i := 0; i < 6; i++ {
		q.fields[i] = int64(r.I32())
	}
	q.fields[6] = int64(r.U32())
	for i := 7; i < len(q.fields); i++ {
		q.fields[i] = int64(r.I32())
	}
	if r.err == nil && (q.id < 1 || q.id > 3 || q.limit == 0 || q.limit > 20 ||
		q.fields[1] > 0 && q.fields[2] > 0 && q.fields[1] > q.fields[2]) {
		r.err = ErrValue
	}
	return q
}

func matchWildcard(value int64) bool { return value == -1 || value == math.MaxInt32 }

// The client contains query encoders but not the original server's SQL. These
// predicates follow the corresponding session/query fields: level range,
// exact mode/region/category attributes and sufficient open slots. Query 3 is
// selected when the game has no specific quest/step and omits those predicates.
func (q matchQuery) accepts(v MatchInfo) bool {
	if v.Attrs[4] == 0 {
		return false
	}
	if min := q.fields[1]; min > 0 && !matchWildcard(min) && v.Attrs[0] < min {
		return false
	}
	if max := q.fields[2]; max > 0 && !matchWildcard(max) && v.Attrs[0] > max {
		return false
	}
	if q.id != 3 {
		for i := 3; i <= 4; i++ {
			if !matchWildcard(q.fields[i]) && q.fields[i] != v.Attrs[i-2] {
				return false
			}
		}
	}
	if !matchWildcard(q.fields[5]) && q.fields[5] != v.Attrs[3] {
		return false
	}
	if slots := q.fields[6]; slots != math.MaxUint32 && slots > v.Attrs[4] {
		return false
	}
	for i := 7; i <= 10; i++ {
		if !matchWildcard(q.fields[i]) && q.fields[i] != v.Attrs[i-2] {
			return false
		}
	}
	return matchWildcard(q.fields[11]) || q.fields[11] == v.Attrs[10]
}

type matchmakingRow struct {
	id    [8]byte
	owner uint64
	lease [32]byte
	info  MatchInfo
}

// MatchmakingResult supplies result records to the lobby envelope. The lobby
// writes task/transaction IDs and result/total counts exactly once.
type MatchmakingResult struct {
	Records    [][]byte
	Total      uint32
	Err        error
	diagnostic error
}

type Matchmaking struct {
	mu       sync.Mutex
	rows     map[[8]byte]matchmakingRow
	maxRooms int
	random   io.Reader
}

func NewMatchmaking(maxRooms int) *Matchmaking {
	if maxRooms <= 0 {
		maxRooms = 128
	}
	return &Matchmaking{rows: make(map[[8]byte]matchmakingRow), maxRooms: maxRooms, random: rand.Reader}
}

// HandleTask adapts the service to the lobby envelope. 0x835 is the session-ID
// error explicitly handled at game 42c9d1. Other refusals use the nonzero task
// failure value 3 observed in native RemoteTaskManager de55a8; no unverified
// server-specific reason enums are assigned to malformed/forbidden requests.
func (m *Matchmaking) HandleTask(session Session, service, task uint8, data []byte) TaskResult {
	if service != 21 {
		return TaskResult{ErrorCode: 3, Reason: fmt.Errorf("unsupported Demonware service %d", service)}
	}
	result := m.Handle(session, task, data)
	if result.Err != nil {
		code := uint32(3)
		if errors.Is(result.Err, ErrMatchmakingInvalidSession) {
			code = 0x835
		}
		reason := result.Err
		if result.diagnostic != nil {
			reason = result.diagnostic
		}
		return TaskResult{ErrorCode: code, Reason: reason}
	}
	return TaskResult{Records: result.Records, Total: result.Total}
}

func matchSessionID(r *TaggedReader) [8]byte {
	b := r.Blob(8)
	var id [8]byte
	if r.err == nil && len(b) != len(id) {
		r.err = ErrValue
	}
	copy(id[:], b)
	return id
}

// Handle implements service 21 tasks observed in this executable: create (1),
// update (2/12), delete (3) and search (5). Authentication is supplied by the
// lobby's verified Session; callers cannot choose another player's account.
func (m *Matchmaking) Handle(session Session, task uint8, data []byte) MatchmakingResult {
	if session.Identity.AccountID == 0 || session.TitleID != D3TitleID {
		return MatchmakingResult{Err: ErrMatchmakingForbidden}
	}
	r := NewTaggedReader(data)
	var id [8]byte
	var info MatchInfo
	var query matchQuery
	var players uint32
	switch task {
	case 1:
		info = readMatchInfo(r)
	case 2, 12:
		id = matchSessionID(r)
		if task == 12 {
			players = r.U32()
		}
		info = readMatchInfo(r)
		// The title passes SessionInfo+0x124 (numPlayers) to task 12.
		// Its separately advertised open slots must describe the same room.
		if r.err == nil && task == 12 && (players > info.MaxPlayers ||
			players != info.MaxPlayers-uint32(info.Attrs[4])) {
			r.err = ErrValue
		}
	case 3:
		id = matchSessionID(r)
	case 5:
		query = readMatchQuery(r)
	default:
		return MatchmakingResult{Err: ErrMatchmakingUnsupported}
	}
	if err := r.Done(); err != nil {
		return MatchmakingResult{Err: ErrMatchmakingInvalidRequest,
			diagnostic: fmt.Errorf("%w at byte %d of %d: %w", ErrMatchmakingInvalidRequest, r.at, len(data), err)}
	}
	lease := sha256.Sum256(session.Ticket[:])
	m.mu.Lock()
	defer m.mu.Unlock()
	if task == 1 {
		old := [8]byte{}
		var replacing bool
		for key, row := range m.rows {
			if row.owner == session.Identity.AccountID {
				old, replacing = key, true
				break
			}
		}
		if !replacing && len(m.rows) >= m.maxRooms {
			return MatchmakingResult{Err: ErrMatchmakingCapacity}
		}
		found := false
		for attempt := 0; attempt < 8; attempt++ {
			if _, err := io.ReadFull(m.random, id[:]); err != nil {
				return MatchmakingResult{Err: ErrMatchmakingUnavailable}
			}
			if id == ([8]byte{}) {
				continue
			}
			if _, exists := m.rows[id]; !exists {
				found = true
				break
			}
		}
		if !found {
			return MatchmakingResult{Err: ErrMatchmakingUnavailable}
		}
		if replacing {
			delete(m.rows, old)
		}
		info.HostAddress = append([]byte(nil), info.HostAddress...)
		m.rows[id] = matchmakingRow{id: id, owner: session.Identity.AccountID, lease: lease, info: info}
		w := NewTaggedWriter(32)
		w.Blob(id[:], 8)
		b, _ := w.Bytes()
		return MatchmakingResult{Records: [][]byte{b}, Total: 1}
	}
	if task == 5 {
		rows := make([]matchmakingRow, 0, len(m.rows))
		for _, row := range m.rows {
			if row.owner != session.Identity.AccountID && query.accepts(row.info) {
				rows = append(rows, row)
			}
		}
		sort.Slice(rows, func(i, j int) bool {
			distance := func(row matchmakingRow) int64 {
				d := row.info.Attrs[0] - query.fields[0]
				if d < 0 {
					return -d
				}
				return d
			}
			if a, b := distance(rows[i]), distance(rows[j]); a != b {
				return a < b
			}
			for k := range rows[i].id {
				if rows[i].id[k] != rows[j].id[k] {
					return rows[i].id[k] < rows[j].id[k]
				}
			}
			return false
		})
		result := MatchmakingResult{Total: uint32(len(rows))}
		start := uint64(query.offset)
		if start > uint64(len(rows)) {
			start = uint64(len(rows))
		}
		end := start + uint64(query.limit)
		if end > uint64(len(rows)) {
			end = uint64(len(rows))
		}
		for _, row := range rows[start:end] {
			b, err := matchResult(row.id, row.info)
			if err != nil {
				return MatchmakingResult{Err: ErrMatchmakingUnavailable}
			}
			result.Records = append(result.Records, b)
		}
		return result
	}
	row, exists := m.rows[id]
	if !exists {
		return MatchmakingResult{Err: ErrMatchmakingInvalidSession}
	}
	if row.owner != session.Identity.AccountID || row.lease != lease {
		return MatchmakingResult{Err: ErrMatchmakingForbidden}
	}
	if task == 3 {
		delete(m.rows, id)
	} else {
		info.HostAddress = append([]byte(nil), info.HostAddress...)
		row.info = info
		m.rows[id] = row
	}
	return MatchmakingResult{}
}

// OnDisconnect removes only advertisements created by this authentication
// lease, so an older connection closing cannot erase a replacement session.
func (m *Matchmaking) OnDisconnect(session Session) {
	lease := sha256.Sum256(session.Ticket[:])
	m.mu.Lock()
	defer m.mu.Unlock()
	for id, row := range m.rows {
		if row.owner == session.Identity.AccountID && row.lease == lease {
			delete(m.rows, id)
		}
	}
}
