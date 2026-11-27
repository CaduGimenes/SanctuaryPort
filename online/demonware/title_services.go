// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"embed"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"time"
)

// The native file task owns a 16 KiB buffer. Keep one byte available for its
// text consumers' terminator; blob lengths on the wire exclude that terminator.
const MaxPublisherBytes = (16 << 10) - 1

//go:embed publisher/*.txt
var defaultPublisher embed.FS

var publisherNames = []string{"Config.txt", "Seasons.txt", "Blacklist.txt", "ChallengeRifts.txt"}

type TitleConfig struct {
	PublisherDir string
	StateDir     string
	MaxRooms     int
}

// TitleServices routes only the native title tasks implemented by this master.
// Identity and connection liveness are established by LobbyServer; the title
// and nonzero account are also checked here for direct callers.
type TitleServices struct {
	Matchmaking  *Matchmaking
	Counters     *CounterStore
	Mailbox      *MailboxStore
	Leaderboards *LeaderboardStore
	publisher    map[string][]byte
	now          func() time.Time
}

func NewTitleServices(cfg TitleConfig) (*TitleServices, error) {
	if cfg.PublisherDir == "" || cfg.StateDir == "" {
		return nil, errors.New("publisher and state directories are required")
	}
	files, err := loadPublisherFiles(cfg.PublisherDir)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(cfg.StateDir, 0700); err != nil {
		return nil, fmt.Errorf("title state directory: %w", err)
	}
	counters, err := NewCounterStore(filepath.Join(cfg.StateDir, "counters.json"))
	if err != nil {
		return nil, err
	}
	mailbox, err := NewMailboxStore(filepath.Join(cfg.StateDir, "mailbox.json"))
	if err != nil {
		return nil, err
	}
	leaderboards, err := NewLeaderboardStore(filepath.Join(cfg.StateDir, "leaderboards.json"))
	if err != nil {
		return nil, err
	}
	return &TitleServices{Matchmaking: NewMatchmaking(cfg.MaxRooms), Counters: counters,
		Mailbox: mailbox, Leaderboards: leaderboards, publisher: files, now: time.Now}, nil
}

func titleFailure(err error) TaskResult { return TaskResult{ErrorCode: 3, Reason: err} }

func (s *TitleServices) HandleTask(session Session, service, task uint8, data []byte) TaskResult {
	if session.Identity.AccountID == 0 || session.TitleID != D3TitleID {
		return titleFailure(ErrMatchmakingForbidden)
	}
	switch {
	case service == 4:
		return s.Leaderboards.HandleTask(session, task, data)
	case service == 21:
		return s.Matchmaking.HandleTask(session, service, task, data)
	case service == 10 && task == 21:
		r := NewTaggedReader(data)
		context, name := r.CString(15), r.CString(127)
		if err := r.Done(); err != nil {
			return titleFailure(fmt.Errorf("publisher file request: %w", err))
		}
		if context != "" {
			return titleFailure(errors.New("unknown publisher context"))
		}
		file, ok := s.publisher[name]
		if !ok {
			return titleFailure(fmt.Errorf("unknown publisher filename %q", name))
		}
		w := NewTaggedWriter(MaxPublisherBytes + 6)
		w.Blob(file, MaxPublisherBytes)
		b, err := w.Bytes()
		if err != nil {
			return titleFailure(err)
		}
		return TaskResult{Records: [][]byte{b}, Total: 1}
	case service == 12 && task == 6:
		if len(data) != 0 {
			return titleFailure(ErrTrailing)
		}
		seconds := s.now().Unix()
		if seconds < 0 || seconds > math.MaxUint32 {
			return titleFailure(errors.New("server clock outside native timestamp range"))
		}
		w := NewTaggedWriter(5)
		w.U32(uint32(seconds))
		b, err := w.Bytes()
		if err != nil {
			return titleFailure(err)
		}
		return TaskResult{Records: [][]byte{b}, Total: 1}
	case service == 23 && task == 1:
		return s.Counters.HandleTask(session, data)
	case service == 29 && task == 11:
		return s.Mailbox.HandleTask(session, data)
	default:
		return titleFailure(fmt.Errorf("unsupported Demonware service %d task %d", service, task))
	}
}

func (s *TitleServices) OnDisconnect(session Session) { s.Matchmaking.OnDisconnect(session) }

// Existing operator files are never overwritten. Contents are bounded and
// validated as one startup snapshot, so a client cannot see a partial edit.
func loadPublisherFiles(dir string) (map[string][]byte, error) {
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, err
	}
	files := make(map[string][]byte, len(publisherNames))
	for _, name := range publisherNames {
		path := filepath.Join(dir, name)
		seed, err := defaultPublisher.ReadFile("publisher/" + name)
		if err != nil {
			return nil, err
		}
		f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
		if err == nil {
			_, writeErr := f.Write(seed)
			if err := errors.Join(writeErr, f.Close()); err != nil {
				return nil, fmt.Errorf("seed publisher file %s: %w", name, err)
			}
		} else if !errors.Is(err, os.ErrExist) {
			return nil, fmt.Errorf("create publisher file %s: %w", name, err)
		}
		f, err = os.Open(path)
		if err != nil {
			return nil, err
		}
		b, readErr := io.ReadAll(io.LimitReader(f, MaxPublisherBytes+1))
		if err := errors.Join(readErr, f.Close()); err != nil {
			return nil, err
		}
		if len(b) > MaxPublisherBytes {
			return nil, fmt.Errorf("publisher file %s exceeds %d bytes", name, MaxPublisherBytes)
		}
		if err := validatePublisher(name, b); err != nil {
			return nil, fmt.Errorf("publisher file %s: %w", name, err)
		}
		files[name] = b
	}
	// The title downloads binary ChallengeData, while operators edit a bounded
	// text configuration. Generate it from the same validated startup snapshot.
	challenge, err := encodeChallengePublisher(files["ChallengeRifts.txt"])
	if err != nil {
		return nil, fmt.Errorf("publisher file ChallengeRifts.txt: %w", err)
	}
	files["challengerift_config.dat"] = challenge
	return files, nil
}
