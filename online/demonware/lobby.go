// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"crypto/hmac"
	"crypto/rand"
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"math"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

type TaskResult struct {
	Records   [][]byte
	Total     uint32
	ErrorCode uint32
	// Reason is a server-side diagnostic only; encodeTaskResult never sends it.
	Reason error
}

// TaskHandler receives the authenticated title session and service-specific
// typed request body. Transport authentication and reply envelopes stay here.
type TaskHandler func(Session, uint8, uint8, []byte) TaskResult

type LobbyServer struct {
	auth     *AuthService
	handler  TaskHandler
	limit    chan struct{}
	mu       sync.Mutex
	clients  map[net.Conn]chan struct{}
	owners   map[uint64]net.Conn
	listener net.Listener
	closed   bool
	serving  atomic.Bool
	nextID   atomic.Uint64
	// Set before Serve/ServeConn. Called only for successfully authenticated
	// connections; session tickets let handlers avoid stale-owner cleanup.
	OnDisconnect func(Session)
	Logger       *slog.Logger
}

func NewLobbyServer(auth *AuthService, handler TaskHandler, maxClients int) *LobbyServer {
	if maxClients <= 0 {
		maxClients = 128
	}
	return &LobbyServer{auth: auth, handler: handler, limit: make(chan struct{}, maxClients),
		clients: make(map[net.Conn]chan struct{}), owners: make(map[uint64]net.Conn), Logger: slog.Default()}
}

func (s *LobbyServer) Ready() bool { return s.auth != nil && s.handler != nil && s.serving.Load() }

func (s *LobbyServer) Serve(listener net.Listener) error {
	if s.auth == nil || s.handler == nil || listener == nil {
		return ErrValue
	}
	s.mu.Lock()
	if s.closed || s.listener != nil {
		s.mu.Unlock()
		return net.ErrClosed
	}
	s.listener = listener
	s.serving.Store(true)
	s.mu.Unlock()
	defer s.serving.Store(false)
	for {
		conn, err := listener.Accept()
		if err != nil {
			return err
		}
		go s.ServeConn(conn)
	}
}

func (s *LobbyServer) Close() {
	s.mu.Lock()
	s.closed = true
	s.serving.Store(false)
	if s.listener != nil {
		_ = s.listener.Close()
	}
	for conn := range s.clients {
		_ = conn.Close()
	}
	s.mu.Unlock()
}

func (s *LobbyServer) ServeConn(conn net.Conn) {
	if conn == nil {
		return
	}
	defer conn.Close()
	select {
	case s.limit <- struct{}{}:
		defer func() { <-s.limit }()
	default:
		return
	}
	s.mu.Lock()
	if s.closed || s.auth == nil || s.handler == nil {
		s.mu.Unlock()
		return
	}
	done := make(chan struct{})
	s.clients[conn] = done
	s.mu.Unlock()
	var authenticated bool
	var session Session
	defer func() {
		defer close(done)
		if authenticated && s.OnDisconnect != nil {
			s.OnDisconnect(session)
		}
		s.mu.Lock()
		delete(s.clients, conn)
		if authenticated && s.owners[session.Identity.AccountID] == conn {
			delete(s.owners, session.Identity.AccountID)
		}
		s.mu.Unlock()
	}()
	_ = conn.SetDeadline(time.Now().Add(20 * time.Second))
	if s.Logger != nil {
		s.Logger.Info("Demonware lobby connected", "peer", conn.RemoteAddr())
	}
	keys, current, err := s.handshake(conn)
	if err != nil {
		if s.Logger != nil {
			s.Logger.Warn("Demonware lobby handshake failed", "peer", conn.RemoteAddr(), "error", err)
		}
		return
	}
	session, authenticated = current, true
	s.mu.Lock()
	old := s.owners[session.Identity.AccountID]
	oldDone := s.clients[old]
	s.owners[session.Identity.AccountID] = conn
	s.mu.Unlock()
	if old != nil && old != conn {
		_ = old.Close()
		// A reconnect may reuse the same title ticket. Finish the old task
		// and its room cleanup before dispatching anything for its successor.
		if oldDone != nil {
			<-oldDone
		}
	}
	if s.Logger != nil {
		s.Logger.Info("Demonware lobby authenticated", "player", session.Identity.Name)
	}
	var incoming, outgoing uint32 = 1, 1
	for {
		_ = conn.SetDeadline(time.Now().Add(90 * time.Second))
		frame, err := ReadFrame(conn, DefaultMaxFrame-4)
		if err != nil {
			return
		}
		if !s.auth.IsLive(session) {
			return
		}
		if len(frame) == 0 {
			if WriteFrame(conn, nil, DefaultMaxFrame-4) != nil {
				return
			}
			continue
		}
		payload, kind, err := decryptLobbyRecord(framedLobbyRecord(frame), incoming, keys.clientAES[:], keys.clientMAC[:])
		if err != nil || kind != 0x86 || len(payload) < 4 || incoming == math.MaxUint32 {
			if s.Logger != nil {
				s.Logger.Warn("Demonware lobby encrypted request rejected", "player", session.Identity.Name,
					"counter", incoming, "kind", kind, "error", err)
			}
			return
		}
		incoming++
		// eboot de5360 writes service as an untyped byte, then typed U8 task.
		// de54b0 finalizes every request with dda710's raw zero end marker.
		// Remove exactly that marker; task handlers still reject extra fields.
		if payload[len(payload)-1] != 0 {
			if s.Logger != nil {
				s.Logger.Warn("Demonware lobby task end marker missing", "player", session.Identity.Name)
			}
			return
		}
		service := payload[0]
		request := NewTaggedReader(payload[1 : len(payload)-1])
		task := request.U8()
		if request.Err() != nil {
			return
		}
		data := request.Raw(request.Remaining())
		result := s.handler(session, service, task, data)
		reply, err := encodeTaskResult(s.nextID.Add(1), task, result)
		if err != nil || outgoing == math.MaxUint32 {
			return
		}
		encoded, err := encryptLobbyRecord(reply, 1, outgoing, keys.serverAES[:], keys.serverMAC[:], rand.Reader)
		if err != nil || writeLobbyBytes(conn, encoded) != nil {
			return
		}
		outgoing++
	}
}

func (s *LobbyServer) handshake(conn net.Conn) (lobbyKeys, Session, error) {
	var emptyKeys lobbyKeys
	var emptySession Session
	probe := make([]byte, 28)
	if _, err := io.ReadFull(conn, probe); err != nil {
		return emptyKeys, emptySession, fmt.Errorf("read probe: %w", err)
	}
	if binary.LittleEndian.Uint32(probe) != 200 || binary.LittleEndian.Uint32(probe[4:]) != 200 ||
		binary.LittleEndian.Uint32(probe[8:]) > lobbyProtocol || binary.LittleEndian.Uint32(probe[12:]) < lobbyProtocol ||
		binary.LittleEndian.Uint32(probe[16:]) < 512 || binary.LittleEndian.Uint32(probe[16:]) > 0x1fffff {
		return emptyKeys, emptySession, fmt.Errorf("%w: probe magic=%d/%d versions=%d..%d receive_limit=%d",
			ErrLobbyProtocol, binary.LittleEndian.Uint32(probe), binary.LittleEndian.Uint32(probe[4:]),
			binary.LittleEndian.Uint32(probe[8:]), binary.LittleEndian.Uint32(probe[12:]), binary.LittleEndian.Uint32(probe[16:]))
	}
	challenge := make([]byte, 22)
	challenge[0], challenge[1] = 0xab, 0x81
	binary.LittleEndian.PutUint32(challenge[2:], lobbyProtocol)
	if _, err := io.ReadFull(rand.Reader, challenge[6:]); err != nil {
		return emptyKeys, emptySession, err
	}
	challengeFrame := framedLobbyRecord(challenge)
	if err := writeLobbyBytes(conn, challengeFrame); err != nil {
		return emptyKeys, emptySession, fmt.Errorf("write challenge: %w", err)
	}
	client, err := ReadFrame(conn, 2048)
	if err != nil {
		return emptyKeys, emptySession, fmt.Errorf("read client auth: %w", err)
	}
	if len(client) != 149 || client[0] != 0xab || client[1] != 0x82 {
		return emptyKeys, emptySession, fmt.Errorf("%w: client auth envelope (%d bytes)", ErrLobbyProtocol, len(client))
	}
	title, _, ticket, err := parseLobbyAuth(client[2 : len(client)-8])
	if err != nil {
		return emptyKeys, emptySession, fmt.Errorf("parse client auth: %w", err)
	}
	session, ok := s.auth.Lookup(ticket)
	if !ok || session.TitleID != title {
		return emptyKeys, emptySession, fmt.Errorf("%w: unknown/expired ticket or title mismatch", ErrLobbyProtocol)
	}
	clientFrame := framedLobbyRecord(client)
	transcript := append(append(append([]byte(nil), probe[8:]...), challengeFrame...), clientFrame[:len(clientFrame)-8]...)
	keys, err := deriveLobbyKeys(session.Key, transcript)
	if err != nil || !hmac.Equal(keys.clientProof[:], client[len(client)-8:]) {
		return emptyKeys, emptySession, fmt.Errorf("%w: client proof verification", ErrLobbyProtocol)
	}
	done := append([]byte{0xab, 0x83}, keys.serverProof[:]...)
	if err := WriteFrame(conn, done, 2048); err != nil {
		return emptyKeys, emptySession, fmt.Errorf("write server proof: %w", err)
	}
	return keys, session, nil
}

func writeLobbyBytes(w io.Writer, data []byte) error {
	for len(data) > 0 {
		n, err := w.Write(data)
		if n < 0 || n > len(data) {
			return io.ErrShortWrite
		}
		data = data[n:]
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
	}
	return nil
}

// e44400/e44680 consume this typed response envelope. Empty successful replies have
// no total-count field; nonempty replies include total before result records.
func encodeTaskResult(transaction uint64, task uint8, result TaskResult) ([]byte, error) {
	w := NewTaggedWriter(DefaultMaxFrame - 64)
	w.U64(transaction)
	w.U32(result.ErrorCode)
	if result.ErrorCode == 0 {
		if len(result.Records) > 1024 || uint64(len(result.Records)) > uint64(result.Total) {
			return nil, ErrLimit
		}
		w.U8(task)
		w.U32(uint32(len(result.Records)))
		if len(result.Records) != 0 {
			w.U32(result.Total)
			for _, record := range result.Records {
				w.Raw(record)
			}
		}
	}
	return w.Bytes()
}
