// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"io"
	"log/slog"
	"net"
	"path/filepath"
	"testing"
	"time"

	pb "sanctuaryport/online/psn/internal/protocol"
	"google.golang.org/protobuf/proto"
)

type remoteConn struct{ net.Conn }

func (remoteConn) RemoteAddr() net.Addr {
	return &net.TCPAddr{IP: net.ParseIP("127.0.0.1"), Port: 40000}
}

type testClient struct {
	t             *testing.T
	conn          net.Conn
	id            uint64
	notifications []packet
	name, token   string
}

func fixture(t *testing.T) *Server {
	t.Helper()
	path := filepath.Join(t.TempDir(), "users.json")
	for _, name := range []string{"Alice", "Bobby", "Carol", "David", "Ellen"} {
		if err := AddAccount(path, name, "test-password-123"); err != nil {
			t.Fatal(err)
		}
	}
	accounts, err := LoadAccounts(path)
	if err != nil {
		t.Fatal(err)
	}
	s := New(Config{Accounts: accounts, Logger: slog.New(slog.NewTextHandler(io.Discard, nil))})
	t.Cleanup(s.Close)
	return s
}
func connect(t *testing.T, s *Server) *testClient {
	t.Helper()
	a, b := net.Pipe()
	go s.ServeConn(remoteConn{a})
	c := &testClient{t: t, conn: b}
	t.Cleanup(func() { b.Close() })
	b.SetDeadline(time.Now().Add(10 * time.Second))
	p, err := readPacket(b)
	if err != nil || p.kind != 3 || !bytes.Equal(p.body, []byte{1, 0, 0, 0}) {
		t.Fatalf("handshake: %v %v", p, err)
	}
	return c
}
func (c *testClient) request(command uint16, m proto.Message, code byte, out proto.Message) {
	c.t.Helper()
	c.id++
	p := packet{command: command, id: c.id}
	if m != nil {
		p.body = blob(m)
	}
	c.conn.SetDeadline(time.Now().Add(10 * time.Second))
	if err := writePacket(c.conn, p); err != nil {
		c.t.Fatal(err)
	}
	for {
		r, err := readPacket(c.conn)
		if err != nil {
			c.t.Fatal(err)
		}
		if r.kind == 2 {
			c.notifications = append(c.notifications, r)
			continue
		}
		if r.id != c.id || r.command != command || len(r.body) == 0 || r.body[0] != code {
			c.t.Fatalf("reply command %d: got %#v wanted code %d", command, r, code)
		}
		if out != nil {
			if err = decode(r.body[1:], out); err != nil {
				c.t.Fatal(err)
			}
		}
		return
	}
}
func login(t *testing.T, s *Server, name string) *testClient {
	c := connect(t, s)
	c.name = name
	c.request(0, &pb.LoginRequest{Npid: name, Password: "test-password-123", TitleId: "CUSA00242"}, 0, &pb.LoginReply{})
	var token pb.GetTokenReply
	c.request(39, nil, 0, &token)
	c.token = token.Token
	c.request(100, &pb.ContextStartRequest{CtxId: 7}, 0, nil)
	return c
}
func create(t *testing.T, c *testClient, flags uint32) uint64 {
	t.Helper()
	var out pb.CreateRoomReply
	c.request(101, &pb.CreateRoomRequest{MaxSlots: 4, WorldId: 1, Flags: flags, ExternalSearchIntAttrs: []*pb.MatchingIntAttr{{AttrId: 0x4c, AttrValue: 70}}, InternalBinAttrs: []*pb.MatchingBinAttr{{AttrId: 0x57, Data: []byte("game settings")}}}, 0, &out)
	if out.Details == nil || out.Details.MeMemberId != 1 || out.Details.OwnerMemberId != 1 || out.Details.RoomData.OpenPublicSlots != 3 {
		t.Fatalf("incomplete room callback: %v", &out)
	}
	return out.RoomId
}
func TestAccounts(t *testing.T) {
	path := filepath.Join(t.TempDir(), "users.json")
	if err := AddAccount(path, "Alice", "short"); err == nil {
		t.Fatal("short password accepted")
	}
	if err := AddAccount(path, "Alice", "test-password-123"); err != nil {
		t.Fatal(err)
	}
	if err := AddAccount(path, "Alice", "test-password-123"); err == nil {
		t.Fatal("duplicate account accepted")
	}
	a, err := LoadAccounts(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := a.Authenticate("Alice", "wrong"); ok {
		t.Fatal("wrong password accepted")
	}
	if _, ok := a.Authenticate("Missing", "test-password-123"); ok {
		t.Fatal("missing user accepted")
	}
	if _, ok := a.Authenticate("Alice", "test-password-123"); !ok {
		t.Fatal("valid password rejected")
	}
}
func TestAuthenticationAndWireBounds(t *testing.T) {
	s := fixture(t)
	c := connect(t, s)
	c.request(12, nil, 23, nil)
	c.request(0, &pb.LoginRequest{Npid: "Alice", Password: "bad", TitleId: "CUSA00242"}, 5, nil)
	c.request(0, &pb.LoginRequest{Npid: "Alice", Password: "test-password-123", TitleId: "CUSA00242"}, 0, &pb.LoginReply{})
	duplicate := connect(t, s)
	duplicate.request(0, &pb.LoginRequest{Npid: "Alice", Password: "test-password-123", TitleId: "CUSA00242"}, 6, nil)
	var features pb.ServerFeaturesReply
	c.request(12, nil, 0, &features)
	if !features.Matching2Enabled || features.TrophiesEnabled {
		t.Fatal("invalid capabilities")
	}
	c.request(101, &pb.CreateRoomRequest{MaxSlots: 4, WorldId: 1}, 23, nil)
	for _, size := range []uint32{0, 14, maxPacket + 1} {
		var b [15]byte
		binary.LittleEndian.PutUint32(b[3:], size)
		if _, err := readPacket(bytes.NewReader(b[:])); err == nil {
			t.Fatal("invalid packet size accepted", size)
		}
	}
	if decode([]byte{0, 0, 0, 2}, &pb.LoginRequest{}) == nil {
		t.Fatal("bad envelope accepted")
	}
	if _, err := readPacket(bytes.NewReader([]byte{0, 1})); err == nil {
		t.Fatal("truncated header accepted")
	}
}

func TestNameOnlyLoginWithoutRegistration(t *testing.T) {
	s := New(Config{Logger: slog.New(slog.NewTextHandler(io.Discard, nil))})
	t.Cleanup(s.Close)
	a := connect(t, s)
	var logged pb.LoginReply
	a.request(0, &pb.LoginRequest{Npid: "NewPlayer", TitleId: "CUSA00242"}, 0, &logged)
	if logged.UserId == 0 {
		t.Fatal("name-only login did not get an account ID")
	}
	duplicate := connect(t, s)
	duplicate.request(0, &pb.LoginRequest{Npid: "NewPlayer", TitleId: "CUSA00242"}, 6, nil)
	var lookup pb.LookupOnlineIdReply
	a.request(41, &pb.LookupOnlineIdRequest{Npid: "NewPlayer"}, 0, &lookup)
	if lookup.AccountId != logged.UserId {
		t.Fatal("lookup disagrees with name-only login")
	}
	other := connect(t, s)
	other.request(0, &pb.LoginRequest{Npid: "OtherPlayer", TitleId: "CUSA00242"}, 0, &pb.LoginReply{})
	a.request(100, &pb.ContextStartRequest{CtxId: 7}, 0, nil)
	id := create(t, a, 0)
	other.request(100, &pb.ContextStartRequest{CtxId: 8}, 0, nil)
	other.request(102, &pb.JoinRoomRequest{RoomId: id}, 0, &pb.JoinRoomReply{})
	fresh := New(Config{})
	defer fresh.Close()
	expected, ok := fresh.account("NewPlayer")
	if !ok || expected.ID != logged.UserId {
		t.Fatal("name-derived ID changes across server restarts")
	}
	a.conn.Close()
	deadline := time.Now().Add(time.Second)
	for {
		s.mu.Lock()
		gone := s.sessions["NewPlayer"] == nil
		s.mu.Unlock()
		if gone {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("disconnected name stayed reserved")
		}
		time.Sleep(time.Millisecond)
	}
	rejoined := connect(t, s)
	var again pb.LoginReply
	rejoined.request(0, &pb.LoginRequest{Npid: "NewPlayer", TitleId: "CUSA00242"}, 0, &again)
	if again.UserId != logged.UserId {
		t.Fatal("name-derived ID changed on reconnect")
	}
}
func TestRoomLifecycleFiltersAndAuthorization(t *testing.T) {
	s := fixture(t)
	a := login(t, s, "Alice")
	b := login(t, s, "Bobby")
	roomID := create(t, a, 0)
	var world pb.GetWorldInfoListReply
	b.request(111, &pb.GetWorldInfoListRequest{ServerId: 1}, 0, &world)
	if len(world.Worlds) != 1 || world.Worlds[0].RoomsNum != 1 {
		t.Fatal("world missing room")
	}
	var search pb.SearchRoomReply
	b.request(104, &pb.SearchRoomRequest{WorldId: 1, RangeFilterStart: 1, RangeFilterMax: 10, IntFilters: []*pb.MatchingIntSearchFilter{{Op: 1, AttrId: 0x4c, AttrValue: 70}}}, 0, &search)
	if len(search.Rooms) != 1 || search.Rooms[0].RoomId != roomID {
		t.Fatal("matching room missing")
	}
	b.request(104, &pb.SearchRoomRequest{WorldId: 1, RangeFilterMax: 10, IntFilters: []*pb.MatchingIntSearchFilter{{Op: 1, AttrId: 0x4c, AttrValue: 69}}}, 0, &search)
	if len(search.Rooms) != 0 {
		t.Fatal("mismatching room returned")
	}
	var joined pb.JoinRoomReply
	b.request(102, &pb.JoinRoomRequest{RoomId: roomID}, 0, &joined)
	if len(joined.Details.Members) != 2 || joined.Details.MeMemberId != 2 {
		t.Fatal("join callback lacks members")
	}
	b.request(108, &pb.SetRoomDataInternalRequest{RoomId: roomID, FlagFilter: 0xffffffff, FlagAttr: 0xffffffff}, 23, nil)
	a.request(12, nil, 0, &pb.ServerFeaturesReply{})
	if len(a.notifications) != 1 {
		t.Fatal("owner did not receive member joined")
	}
	var event pb.NotifyRoomEvent
	if err := decode(a.notifications[0].body, &event); err != nil || event.CtxId != 7 || event.Event != 0x1101 || event.Member.Npid != "Bobby" {
		t.Fatal("bad member callback", &event, err)
	}
	b.request(115, &pb.SendRoomMessageRequest{RoomId: roomID, CastType: 2, DstMemberIds: []uint32{1}, Msg: []byte("hello")}, 0, &pb.SendRoomMessageReply{})
	a.request(12, nil, 0, &pb.ServerFeaturesReply{})
	var message pb.NotifyRoomMessage
	if err := decode(a.notifications[1].body, &message); err != nil || string(message.Msg) != "hello" || message.SrcMemberId != 2 {
		t.Fatal("room message was not delivered")
	}
	a.request(103, &pb.LeaveRoomRequest{RoomId: roomID}, 0, &pb.LeaveRoomReply{})
	b.request(104, &pb.SearchRoomRequest{WorldId: 1, RangeFilterMax: 10}, 0, &search)
	if len(search.Rooms) != 0 {
		t.Fatal("owner's room was not destroyed")
	}
	if len(b.notifications) != 2 {
		t.Fatalf("missing owner leave/destroy: %v", b.notifications)
	}
}
func TestRoomCapacityMigrationAndCleanup(t *testing.T) {
	s := fixture(t)
	a := login(t, s, "Alice")
	id := create(t, a, 0x80000000)
	peers := []*testClient{login(t, s, "Bobby"), login(t, s, "Carol"), login(t, s, "David")}
	for _, p := range peers {
		p.request(102, &pb.JoinRoomRequest{RoomId: id}, 0, &pb.JoinRoomReply{})
	}
	fifth := login(t, s, "Ellen")
	fifth.request(102, &pb.JoinRoomRequest{RoomId: id}, 16, nil)
	a.conn.Close()
	deadline := time.Now().Add(time.Second)
	for {
		s.mu.Lock()
		r := s.rooms[id]
		migrated := r != nil && r.owner.name == "Bobby" && len(r.members) == 3
		s.mu.Unlock()
		if migrated {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("disconnect did not migrate ownership")
		}
		time.Sleep(time.Millisecond)
	}
	peers[0].request(109, &pb.SetRoomDataExternalRequest{RoomId: id, SearchIntAttrs: []*pb.MatchingIntAttr{{AttrId: 0x4c, AttrValue: 60}}}, 0, &pb.SetRoomDataExternalReply{})
	fifth.request(102, &pb.JoinRoomRequest{RoomId: id}, 0, &pb.JoinRoomReply{})
	peers[0].request(110, &pb.KickoutRoomMemberRequest{RoomId: id, TargetMemberId: 3}, 0, &pb.KickoutRoomMemberReply{})
	peers[1].request(115, &pb.SendRoomMessageRequest{RoomId: id, CastType: 1}, 23, nil)
}

func TestRoomSlotReuseAndAttributeBounds(t *testing.T) {
	s := fixture(t)
	a := login(t, s, "Alice")
	b := login(t, s, "Bobby")
	id := create(t, a, 0)
	for i := 0; i < 20; i++ {
		var joined pb.JoinRoomReply
		b.request(102, &pb.JoinRoomRequest{RoomId: id}, 0, &joined)
		if joined.MemberId != 2 || joined.Details.RoomData.JoinedSlotMask != 3 {
			t.Fatal("vacated slot was not reused")
		}
		b.request(103, &pb.LeaveRoomRequest{RoomId: id}, 0, &pb.LeaveRoomReply{})
	}
	a.request(108, &pb.SetRoomDataInternalRequest{RoomId: id, BinAttrs: []*pb.MatchingBinAttr{{AttrId: 0x57, Data: make([]byte, 4097)}}}, 1, nil)
	a.request(109, &pb.SetRoomDataExternalRequest{RoomId: id, SearchIntAttrs: []*pb.MatchingIntAttr{{AttrId: 0xffff, AttrValue: 42}}}, 1, nil)
}
func signUDP(prefix []byte, token string, now time.Time) []byte {
	b := append([]byte(nil), prefix...)
	b = binary.BigEndian.AppendUint64(b, uint64(now.Unix()))
	m := hmac.New(sha256.New, []byte(token))
	m.Write(b)
	return append(b, m.Sum(nil)...)
}
func TestAuthenticatedSignalingAndRoomScopedRelay(t *testing.T) {
	s := fixture(t)
	a := login(t, s, "Alice")
	b := login(t, s, "Bobby")
	outsider := login(t, s, "Carol")
	id := create(t, a, 0)
	b.request(102, &pb.JoinRoomRequest{RoomId: id}, 0, &pb.JoinRoomReply{})
	create(t, outsider, 0)
	now := time.Now()
	aa := &net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: 3658}
	ba := &net.UDPAddr{IP: aa.IP, Port: 3659}
	registration := func(c *testClient, addr *net.UDPAddr) {
		prefix := make([]byte, 21)
		copy(prefix, "BBR1\x01")
		copy(prefix[5:], c.name)
		signed := signUDP(prefix, c.token, now)
		reply, dst := s.Datagram(signed, addr, now)
		if !bytes.Equal(reply, []byte("BBR1\x02")) || !sameUDP(dst, addr) {
			t.Fatal("registration failed")
		}
		signed[30] ^= 1
		if out, _ := s.Datagram(signed, addr, now); out != nil {
			t.Fatal("forged signature accepted")
		}
		if out, _ := s.Datagram(signUDP(prefix, c.token, now.Add(-time.Minute)), addr, now); out != nil {
			t.Fatal("stale signature accepted")
		}
	}
	registration(a, aa)
	registration(b, ba)
	var target pb.RequestSignalingInfosReply
	a.request(105, &pb.RequestSignalingInfosRequest{TargetNpid: "Bobby"}, 0, &target)
	if target.TargetPort != 3659 || target.TargetIp != "127.0.0.1" {
		t.Fatal("incorrect observed endpoint")
	}
	outsider.request(105, &pb.RequestSignalingInfosRequest{TargetNpid: "Alice"}, 26, nil)
	frame := make([]byte, 43)
	copy(frame, "BBR1\x03")
	copy(frame[5:21], "Bobby")
	copy(frame[21:37], "Alice")
	frame = append(frame, []byte("native-p2p-packet")...)
	out, dst := s.Datagram(frame, aa, now)
	if !sameUDP(dst, ba) || !bytes.Equal(out[27:], []byte("native-p2p-packet")) || !bytes.Equal(out[21:27], endpoint(aa)) {
		t.Fatal("relay altered payload or trusted forged endpoint")
	}
	if out, _ := s.Datagram(frame, ba, now); out != nil {
		t.Fatal("spoofed sender accepted")
	}
	copy(frame[5:21], make([]byte, 16))
	copy(frame[5:21], "Carol")
	if out, _ := s.Datagram(frame, aa, now); out != nil {
		t.Fatal("cross-room relay accepted")
	}
	ping := make([]byte, 21)
	ping[0] = 1
	copy(ping[1:17], "Alice")
	echo, dst := s.Datagram(signUDP(ping, a.token, now), aa, now)
	if !bytes.Equal(echo, endpoint(aa)) || !sameUDP(dst, aa) {
		t.Fatal("STUN echo mismatch")
	}
}
