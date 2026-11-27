// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"io"
	"log/slog"
	"net"
	"sync"
	"time"

	pb "sanctuaryport/online/psn/internal/protocol"
	"google.golang.org/protobuf/proto"
)

type Config struct {
	// Nil selects name-only login; a supplied database enables password checks.
	Accounts             Accounts
	MaxClients, MaxRooms int
	Logger               *slog.Logger
}
type Server struct {
	mu             sync.Mutex
	cfg            Config
	clients        map[*client]bool
	sessions       map[string]*client
	rooms          map[uint64]*room
	webSessions    map[string]*webSession
	nextRoom       uint64
	nextWebSession uint64
	closing        bool
}
type client struct {
	s                  *Server
	conn               net.Conn
	send               chan packet
	done               chan struct{}
	once               sync.Once
	name, title, token string
	account            uint64
	ctx                uint32
	room               uint64
	attrs              []*pb.MatchingBinAttr
	udp                *net.UDPAddr
	udpSeen            time.Time
	udpWindow          time.Time
	udpCount           int
	presence           webPresence
}

func New(cfg Config) *Server {
	if cfg.MaxClients <= 0 {
		cfg.MaxClients = 128
	}
	if cfg.MaxRooms <= 0 {
		cfg.MaxRooms = 128
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	return &Server{cfg: cfg, clients: map[*client]bool{}, sessions: map[string]*client{}, rooms: map[uint64]*room{}, webSessions: map[string]*webSession{}, nextRoom: 1, nextWebSession: 1}
}
func (s *Server) Serve(l net.Listener) error {
	for {
		conn, err := l.Accept()
		if err != nil {
			return err
		}
		go s.ServeConn(conn)
	}
}
func (s *Server) Close() {
	s.mu.Lock()
	s.closing = true
	for c := range s.clients {
		c.close()
	}
	s.mu.Unlock()
}

// ResolvePrivateIdentity lets the title backend reuse a live name-only master
// login. It never provisions another account or asks for another password.
func (s *Server) ResolvePrivateIdentity(token string) (uint64, string, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c := s.webClient(token)
	if c == nil {
		return 0, "", false
	}
	return c.account, c.name, true
}
func (c *client) close() { c.once.Do(func() { close(c.done); c.conn.Close() }) }
func (c *client) queue(p packet) {
	select {
	case <-c.done:
		return
	default:
	}
	select {
	case c.send <- p:
	default:
		c.close()
	}
}
func (c *client) reply(req packet, code byte, m proto.Message) {
	b := []byte{code}
	if m != nil {
		b = append(b, blob(m)...)
	}
	c.queue(packet{kind: 1, command: req.command, id: req.id, body: b})
}
func (s *Server) ServeConn(conn net.Conn) {
	c := &client{s: s, conn: conn, send: make(chan packet, 128), done: make(chan struct{})}
	s.mu.Lock()
	if s.closing || len(s.clients) >= s.cfg.MaxClients {
		s.mu.Unlock()
		conn.Close()
		return
	}
	s.clients[c] = true
	s.mu.Unlock()
	defer func() {
		c.close()
		s.mu.Lock()
		s.leave(c, 0)
		s.leaveWebSessions(c)
		if s.sessions[c.name] == c {
			delete(s.sessions, c.name)
		}
		delete(s.clients, c)
		s.mu.Unlock()
	}()
	writerDone := make(chan struct{})
	go func() {
		defer close(writerDone)
		defer c.close()
		for {
			select {
			case <-c.done:
				return
			case p := <-c.send:
				conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
				if writePacket(conn, p) != nil {
					return
				}
			}
		}
	}()
	defer func() { c.close(); <-writerDone }()
	c.queue(packet{kind: 3, body: []byte{1, 0, 0, 0}})
	// Every frame, including its body, must arrive within this deadline. Idle
	// authenticated clients are retained; disconnect/room cleanup follows EOF.
	window := time.Now()
	requests := 0
	for {
		timeout := 2 * time.Minute
		if c.name == "" {
			timeout = 10 * time.Second
		}
		conn.SetReadDeadline(time.Now().Add(timeout))
		req, err := readPacket(conn)
		if err != nil {
			if !errors.Is(err, io.EOF) {
				s.cfg.Logger.Debug("connection ended", "error", err)
			}
			return
		}
		if req.kind != 0 {
			return
		}
		if time.Since(window) >= time.Second {
			window = time.Now()
			requests = 0
		}
		requests++
		if requests > 200 {
			return
		}
		if c.name == "" && req.command != 0 {
			c.reply(req, 23, nil)
			continue
		}
		if req.command == 0 {
			if c.name != "" {
				c.reply(req, 6, nil)
				continue
			}
			if !s.login(c, req) {
				return
			}
			continue
		}
		s.mu.Lock()
		s.command(c, req)
		s.mu.Unlock()
		if req.command == 1 {
			return
		}
	}
}
func (s *Server) login(c *client, p packet) bool {
	var r pb.LoginRequest
	if decode(p.body, &r) != nil || !onlineID.MatchString(r.Npid) || len(r.Password) > 256 || r.TitleId != "CUSA00242" {
		c.reply(p, 3, nil)
		return true
	}
	a, ok := s.account(r.Npid)
	if s.cfg.Accounts != nil {
		a, ok = s.cfg.Accounts.Authenticate(r.Npid, r.Password)
	}
	if !ok {
		c.reply(p, 5, nil)
		return true
	}
	token := make([]byte, 32)
	if _, err := rand.Read(token); err != nil {
		return false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.sessions[r.Npid] != nil {
		c.reply(p, 6, nil)
		return true
	}
	c.name = r.Npid
	c.account = a.ID
	c.title = r.TitleId
	c.token = hex.EncodeToString(token)
	s.sessions[c.name] = c
	c.reply(p, 0, &pb.LoginReply{UserId: c.account})
	s.cfg.Logger.Info("signed in", "online_id", c.name, "title", c.title)
	return true
}
func (s *Server) notify(r *room, event uint32, member *pb.MatchingRoomMemberData, cause uint32, except *client) {
	for c := range r.members {
		if c != except {
			c.queue(packet{kind: 2, command: 10, body: blob(&pb.NotifyRoomEvent{CtxId: c.ctx, RoomId: r.id, Event: event, EventCause: cause, Member: member, Flags: r.flags, BinAttrs: r.internal})})
		}
	}
}
