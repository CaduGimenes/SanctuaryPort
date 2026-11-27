// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"bytes"
	"sort"
	"time"

	pb "sanctuaryport/online/psn/internal/protocol"
)

type room struct {
	id                            uint64
	title                         string
	owner                         *client
	world, lobby, flags, max      uint32
	members                       map[*client]*pb.MatchingRoomMemberData
	internal, external, searchBin []*pb.MatchingBinAttr
	searchInt                     []*pb.MatchingIntAttr
}

func (r *room) member(c *client) *pb.MatchingRoomMemberData {
	// Occupied slot IDs are stable while connected and may be reused after leave.
	id := uint32(1)
	for {
		used := false
		for _, m := range r.members {
			if m.MemberId == id {
				used = true
				break
			}
		}
		if !used {
			break
		}
		id++
	}
	return &pb.MatchingRoomMemberData{Npid: c.name, AccountId: c.account, Platform: 1, MemberId: id, IsOwner: c == r.owner, FlagAttr: ownerFlag(c == r.owner), JoinDate: rtcTick(), NatType: 2}
}
func rtcTick() uint64 { return uint64(time.Now().UnixMicro()) + 62135596800000000 }
func memberRole(owner bool) uint32 {
	if owner {
		return 2
	}
	return 1
}
func ownerFlag(owner bool) uint32 {
	if owner {
		return 0x80000000
	}
	return 0
}
func (r *room) details(c *client) *pb.CreateJoinRoomResponse {
	members := make([]*pb.MatchingRoomMemberData, 0, len(r.members))
	var mask uint32
	for _, m := range r.members {
		members = append(members, m)
		mask |= 1 << (m.MemberId - 1)
	}
	sort.Slice(members, func(i, j int) bool { return members[i].MemberId < members[j].MemberId })
	return &pb.CreateJoinRoomResponse{RoomData: &pb.MatchingRoomDataInternal{PublicSlots: r.max, OpenPublicSlots: r.max - uint32(len(r.members)), MaxSlot: r.max, ServerId: 1, WorldId: r.world, LobbyId: r.lobby, RoomId: r.id, JoinedSlotMask: mask, Flags: r.flags, BinAttrsInternal: r.internal}, Members: members, MeMemberId: r.members[c].MemberId, OwnerMemberId: r.members[r.owner].MemberId}
}
func (r *room) public() *pb.MatchingRoomDataExternal {
	var mask uint64
	for _, m := range r.members {
		mask |= 1 << (m.MemberId - 1)
	}
	flags := r.flags
	if uint32(len(r.members)) == r.max {
		flags |= 0x20000000
	}
	return &pb.MatchingRoomDataExternal{MaxSlot: r.max, CurMembers: uint32(len(r.members)), Flags: flags, ServerId: 1, WorldId: r.world, LobbyId: uint64(r.lobby), RoomId: r.id, JoinedSlotMask: mask, PublicSlots: r.max, OpenPublicSlots: r.max - uint32(len(r.members)), ExternalSearchIntAttrs: r.searchInt, ExternalSearchBinAttrs: r.searchBin, ExternalBinAttrs: r.external, OwnerNpid: r.owner.name, OwnerAccountId: r.owner.account, OwnerPlatform: 1}
}
func (s *Server) leave(c *client, cause uint32) {
	r := s.rooms[c.room]
	if r == nil {
		c.room = 0
		return
	}
	m := r.members[c]
	delete(r.members, c)
	c.room = 0
	if len(r.members) == 0 {
		delete(s.rooms, r.id)
		return
	}
	if cause == 0 {
		cause = 5
	}
	s.notify(r, 0x1102, m, cause, nil)
	if r.owner == c {
		if r.flags&0x80000000 == 0 {
			s.notify(r, 0x1104, nil, cause, nil)
			for peer := range r.members {
				peer.room = 0
			}
			delete(s.rooms, r.id)
			return
		}
		var next *client
		for peer, v := range r.members {
			if next == nil || v.MemberId < r.members[next].MemberId {
				next = peer
			}
		}
		r.owner = next
		r.members[next].IsOwner = true
		r.members[next].FlagAttr |= 0x80000000
		s.notify(r, 0x1105, r.members[next], 4, nil)
	}
}
func (s *Server) command(c *client, p packet) {
	bad := func() { c.reply(p, 1, nil) }
	if p.command >= 101 && p.command <= 116 && p.command != 106 && c.ctx == 0 {
		c.reply(p, 23, nil)
		return
	}
	switch p.command {
	case 1:
		c.reply(p, 0, nil)
	case 12:
		c.reply(p, 0, &pb.ServerFeaturesReply{Matching2Enabled: true})
	case 13, 40:
		c.reply(p, 0, nil)
	case 39:
		c.reply(p, 0, &pb.GetTokenReply{Token: c.token, UserId: c.account, Npid: c.name})
	case 41:
		var q pb.LookupOnlineIdRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		a, ok := s.account(q.Npid)
		if !ok {
			c.reply(p, 26, nil)
			return
		}
		c.reply(p, 0, &pb.LookupOnlineIdReply{AccountId: a.ID, Npid: q.Npid})
	case 100:
		var q pb.ContextStartRequest
		if decode(p.body, &q) != nil || q.CtxId == 0 {
			bad()
			return
		}
		if c.ctx != 0 && c.ctx != q.CtxId {
			c.reply(p, 2, nil)
			return
		}
		c.ctx = q.CtxId
		c.reply(p, 0, nil)
	case 106:
		var q pb.ContextStopRequest
		if decode(p.body, &q) != nil || q.CtxId != c.ctx {
			bad()
			return
		}
		s.leave(c, 11)
		c.ctx = 0
		c.reply(p, 0, nil)
	case 101:
		var q pb.CreateRoomRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		if c.room != 0 {
			c.reply(p, 15, nil)
			return
		}
		if len(s.rooms) >= s.cfg.MaxRooms {
			c.reply(p, 16, nil)
			return
		}
		if q.MaxSlots < 1 || q.MaxSlots > 4 || q.WorldId != 1 {
			c.reply(p, 3, nil)
			return
		}
		// Reject unsupported policy instead of silently making a private room public.
		if q.RoomPasswordPresent || q.HasPasswdSlotMask && q.PasswdSlotMask != 0 || len(q.GroupConfigs) > 0 || q.GroupConfigCount > 0 || q.AllowedUserCount > 0 || q.BlockedUserCount > 0 || len(q.AllowedOnlineIds) > 0 || len(q.BlockedOnlineIds) > 0 || len(q.AllowedAccountIds) > 0 || len(q.BlockedAccountIds) > 0 {
			c.reply(p, 33, nil)
			return
		}
		r := &room{id: s.nextRoom, title: c.title, owner: c, world: q.WorldId, lobby: q.LobbyId, flags: q.Flags, max: q.MaxSlots, members: map[*client]*pb.MatchingRoomMemberData{}, internal: q.InternalBinAttrs, external: q.ExternalBinAttrs, searchBin: q.ExternalSearchBinAttrs, searchInt: q.ExternalSearchIntAttrs}
		s.nextRoom++
		m := r.member(c)
		m.TeamId = q.TeamId
		m.BinAttrsInternal = q.MemberBinAttrs
		r.members[c] = m
		s.rooms[r.id] = r
		c.room = r.id
		c.reply(p, 0, &pb.CreateRoomReply{RoomId: r.id, ServerId: 1, WorldId: r.world, LobbyId: r.lobby, MemberId: m.MemberId, MaxSlots: r.max, Flags: r.flags, CurMembers: 1, Details: r.details(c)})
	case 102:
		var q pb.JoinRoomRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		r := s.rooms[q.RoomId]
		if r == nil || r.title != c.title {
			c.reply(p, 14, nil)
			return
		}
		if c.room != 0 {
			c.reply(p, 15, nil)
			return
		}
		if uint32(len(r.members)) >= r.max {
			c.reply(p, 16, nil)
			return
		}
		if r.flags&0x40000000 != 0 {
			c.reply(p, 23, nil)
			return
		}
		if q.RoomPasswordPresent || q.JoinGroupLabelPresent || q.BlockedUserCount > 0 || len(q.BlockedOnlineIds) > 0 || len(q.BlockedAccountIds) > 0 {
			c.reply(p, 33, nil)
			return
		}
		m := r.member(c)
		m.TeamId = q.TeamId
		m.BinAttrsInternal = q.MemberBinAttrs
		r.members[c] = m
		c.room = r.id
		c.reply(p, 0, &pb.JoinRoomReply{RoomId: r.id, MemberId: m.MemberId, MaxSlots: r.max, Flags: r.flags, CurMembers: uint32(len(r.members)), Details: r.details(c)})
		s.notify(r, 0x1101, m, 1, c)
	case 103:
		var q pb.LeaveRoomRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		if c.room == 0 || c.room != q.RoomId {
			c.reply(p, 14, nil)
			return
		}
		s.leave(c, 1)
		c.reply(p, 0, &pb.LeaveRoomReply{RoomId: q.RoomId})
	case 104:
		var q pb.SearchRoomRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		for _, f := range q.IntFilters {
			if f.Op < 1 || f.Op > 6 {
				bad()
				return
			}
		}
		for _, f := range q.BinFilters {
			if f.Op < 1 || f.Op > 2 {
				bad()
				return
			}
		}
		matches := []*pb.MatchingRoomDataExternal{}
		for _, r := range s.rooms {
			if r.title == c.title && matchesRoom(r, &q) {
				matches = append(matches, r.public())
			}
		}
		sort.Slice(matches, func(i, j int) bool { return matches[i].RoomId < matches[j].RoomId })
		total := len(matches)
		start := q.RangeFilterStart
		if start == 0 {
			start = 1
		}
		offset := int(start - 1)
		if offset > total {
			offset = total
		}
		limit := int(q.RangeFilterMax)
		if limit > 100 {
			limit = 100
		}
		end := offset + limit
		if end > total {
			end = total
		}
		c.reply(p, 0, &pb.SearchRoomReply{Rooms: matches[offset:end], RangeStart: start, RangeTotal: uint32(total), RangeResult: uint32(end - offset)})
	case 105:
		var q pb.RequestSignalingInfosRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		target := s.sessions[q.TargetNpid]
		if target == nil || c.room == 0 || target.room != c.room || target.udp == nil || time.Since(target.udpSeen) > 30*time.Second {
			c.reply(p, 26, nil)
			return
		}
		c.reply(p, 0, &pb.RequestSignalingInfosReply{TargetNpid: target.name, TargetIp: target.udp.IP.String(), TargetPort: uint32(target.udp.Port), TargetMemberId: s.rooms[c.room].members[target].MemberId})
	case 107:
		var q pb.SetUserInfoRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		c.attrs = mergeBins(c.attrs, q.UserBinAttrs)
		c.reply(p, 0, &pb.SetUserInfoReply{})
	case 108:
		var q pb.SetRoomDataInternalRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		r := s.owned(c, q.RoomId)
		if r == nil {
			c.reply(p, 23, nil)
			return
		}
		if q.HasPasswdMask && q.PasswdSlotMask != 0 {
			c.reply(p, 33, nil)
			return
		}
		r.flags = (r.flags & ^q.FlagFilter) | (q.FlagAttr & q.FlagFilter)
		r.internal = mergeBins(r.internal, q.BinAttrs)
		c.reply(p, 0, &pb.SetRoomDataInternalReply{RoomId: r.id})
		s.notify(r, 0x1106, nil, 1, nil)
	case 109:
		var q pb.SetRoomDataExternalRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		r := s.owned(c, q.RoomId)
		if r == nil {
			c.reply(p, 23, nil)
			return
		}
		r.searchInt = mergeInts(r.searchInt, q.SearchIntAttrs)
		r.searchBin = mergeBins(r.searchBin, q.SearchBinAttrs)
		r.external = mergeBins(r.external, q.ExtBinAttrs)
		c.reply(p, 0, &pb.SetRoomDataExternalReply{RoomId: r.id})
	case 110:
		var q pb.KickoutRoomMemberRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		r := s.owned(c, q.RoomId)
		if r == nil {
			c.reply(p, 23, nil)
			return
		}
		if q.BlockKickFlag != 0 {
			c.reply(p, 33, nil)
			return
		}
		for peer, m := range r.members {
			if m.MemberId == q.TargetMemberId && peer != c {
				peer.queue(packet{kind: 2, command: 10, body: blob(&pb.NotifyRoomEvent{CtxId: peer.ctx, RoomId: r.id, Event: 0x1103, EventCause: 2, Member: m})})
				s.leave(peer, 2)
				c.reply(p, 0, &pb.KickoutRoomMemberReply{RoomId: r.id})
				return
			}
		}
		c.reply(p, 26, nil)
	case 111:
		var q pb.GetWorldInfoListRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		if q.ServerId != 1 {
			c.reply(p, 26, nil)
			return
		}
		var n uint32
		for _, r := range s.rooms {
			n += uint32(len(r.members))
		}
		c.reply(p, 0, &pb.GetWorldInfoListReply{Worlds: []*pb.MatchingWorld{{WorldId: 1, RoomsNum: uint32(len(s.rooms)), RoomMembersNum: n}}})
	case 112:
		var q pb.GetRoomDataExternalListRequest
		if decode(p.body, &q) != nil || len(q.RoomIds) > 100 {
			bad()
			return
		}
		out := &pb.GetRoomDataExternalListReply{}
		for _, id := range q.RoomIds {
			if r := s.rooms[id]; r != nil && r.title == c.title && r.flags&0x10000000 == 0 {
				out.Rooms = append(out.Rooms, r.public())
			}
		}
		c.reply(p, 0, out)
	case 113:
		var q pb.GetUserInfoListRequest
		if decode(p.body, &q) != nil || len(q.Npids) > 100 {
			bad()
			return
		}
		out := &pb.GetUserInfoListReply{}
		for _, name := range q.Npids {
			if a, ok := s.account(name); ok {
				u := &pb.MatchingUserInfo{Npid: name, AccountId: a.ID, Platform: 1}
				if peer := s.sessions[name]; peer != nil {
					u.UserBinAttrs = peer.attrs
				}
				out.Users = append(out.Users, u)
			}
		}
		c.reply(p, 0, out)
	case 114:
		var q pb.GetRoomMemberDataExternalListRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		r := s.rooms[q.RoomId]
		if r == nil || c.room != r.id {
			c.reply(p, 23, nil)
			return
		}
		out := &pb.GetRoomMemberDataExternalListReply{}
		for _, m := range r.details(c).Members {
			out.Members = append(out.Members, &pb.MatchingRoomMemberDataExternal{Npid: m.Npid, MemberId: m.MemberId, AccountId: m.AccountId, Platform: 1, JoinDate: m.JoinDate, Role: memberRole(m.IsOwner)})
		}
		c.reply(p, 0, out)
	case 115:
		var q pb.SendRoomMessageRequest
		if decode(p.body, &q) != nil || len(q.Msg) > 16384 {
			bad()
			return
		}
		r := s.rooms[q.RoomId]
		if r == nil || c.room != r.id {
			c.reply(p, 23, nil)
			return
		}
		if q.CastType < 1 || q.CastType > 3 || q.CastType == 2 && len(q.DstMemberIds) != 1 {
			bad()
			return
		}
		m := r.members[c]
		for peer, dst := range r.members {
			deliver := q.CastType == 1
			for _, id := range q.DstMemberIds {
				deliver = deliver || dst.MemberId == id
			}
			if deliver {
				peer.queue(packet{kind: 2, command: 11, body: blob(&pb.NotifyRoomMessage{CtxId: peer.ctx, RoomId: r.id, SrcMemberId: m.MemberId, Event: 0x2102, CastType: q.CastType, DstMemberIds: q.DstMemberIds, SrcNpid: c.name, SrcAccountId: c.account, SrcPlatform: 1, Msg: q.Msg})})
			}
		}
		c.reply(p, 0, &pb.SendRoomMessageReply{RoomId: r.id})
	case 116:
		var q pb.SetRoomMemberDataInternalRequest
		if decode(p.body, &q) != nil {
			bad()
			return
		}
		r := s.rooms[q.RoomId]
		if r == nil || c.room != r.id {
			c.reply(p, 23, nil)
			return
		}
		for peer, m := range r.members {
			if m.MemberId == q.MemberId || (q.MemberId == 0 && peer == c) {
				if peer != c && r.owner != c {
					c.reply(p, 23, nil)
					return
				}
				m.TeamId = q.TeamId
				m.FlagAttr = ((m.FlagAttr & ^q.FlagFilter)|(q.FlagAttr&q.FlagFilter))&0x7fffffff | ownerFlag(peer == r.owner)
				m.BinAttrsInternal = mergeBins(m.BinAttrsInternal, q.BinAttrs)
				c.reply(p, 0, &pb.SetRoomMemberDataInternalReply{RoomId: r.id})
				s.notify(r, 0x1107, m, 1, nil)
				return
			}
		}
		c.reply(p, 26, nil)
	default:
		c.reply(p, 33, nil)
	}
}
func (s *Server) owned(c *client, id uint64) *room {
	r := s.rooms[id]
	if r == nil || c.room != id || r.owner != c {
		return nil
	}
	return r
}
func mergeBins(old, update []*pb.MatchingBinAttr) []*pb.MatchingBinAttr {
	for _, v := range update {
		found := false
		for i, o := range old {
			if o.AttrId == v.AttrId {
				old[i] = v
				found = true
				break
			}
		}
		if !found {
			old = append(old, v)
		}
	}
	return old
}
func mergeInts(old, update []*pb.MatchingIntAttr) []*pb.MatchingIntAttr {
	for _, v := range update {
		found := false
		for i, o := range old {
			if o.AttrId == v.AttrId {
				old[i] = v
				found = true
				break
			}
		}
		if !found {
			old = append(old, v)
		}
	}
	return old
}
func matchesRoom(r *room, q *pb.SearchRoomRequest) bool {
	if r.flags&0x10000000 != 0 || q.WorldId != 0 && q.WorldId != r.world || q.LobbyId != 0 && q.LobbyId != uint64(r.lobby) || r.public().Flags&q.FlagFilter != q.FlagAttr&q.FlagFilter {
		return false
	}
	for _, f := range q.IntFilters {
		found := false
		for _, a := range r.searchInt {
			if a.AttrId == f.AttrId {
				v, w := a.AttrValue, f.AttrValue
				found = f.Op == 1 && v == w || f.Op == 2 && v != w || f.Op == 3 && v < w || f.Op == 4 && v <= w || f.Op == 5 && v > w || f.Op == 6 && v >= w
				break
			}
		}
		if !found {
			return false
		}
	}
	for _, f := range q.BinFilters {
		found := false
		for _, a := range r.searchBin {
			if a.AttrId == f.AttrId {
				eq := bytes.Equal(a.Data, f.Data)
				found = f.Op == 1 && eq || f.Op == 2 && !eq
				break
			}
		}
		if !found {
			return false
		}
	}
	return true
}
