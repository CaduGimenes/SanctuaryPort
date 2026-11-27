// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"mime"
	"mime/multipart"
	"net/http"
	"sort"
	"strconv"
	"time"
)

type webInvitation struct {
	ID       string
	From, To *client
	Session  *webSession
	Created  time.Time
	Used     bool
	Message  string
	Data     []byte
}

func (w *webSession) invited(c *client) bool {
	for _, inv := range w.Invitations {
		if inv.To == c && time.Since(inv.Created) < time.Hour {
			return true
		}
	}
	return false
}
func webInvitationJSON(i *webInvitation) map[string]any {
	return map[string]any{"invitationId": i.ID, "sessionId": i.Session.ID, "receivedDate": i.Created.Format(time.RFC3339), "updateDate": i.Created.Format(time.RFC3339), "usedFlag": i.Used, "expired": time.Since(i.Created) >= time.Hour, "fromUser": webUser(i.From), "message": i.Message, "session": webSessionJSON(i.Session)}
}
func (s *Server) webSendInvitations(c *client, w *webSession, r *http.Request, body []byte) webResponse {
	if !w.Members[c] {
		return webError(403, "session member required")
	}
	media, params, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil {
		return webError(400, "invalid invitation content type")
	}
	var request struct {
		To      []string `json:"to"`
		Message string   `json:"message"`
	}
	var data []byte
	if media == "multipart/mixed" {
		reader := multipart.NewReader(bytes.NewReader(body), params["boundary"])
		found := false
		for {
			part, e := reader.NextPart()
			if e == io.EOF {
				break
			}
			if e != nil {
				return webError(400, "invalid invitation multipart")
			}
			b, e := io.ReadAll(io.LimitReader(part, 65537))
			if e != nil || len(b) > 65536 {
				return webError(413, "invitation too large")
			}
			switch part.Header.Get("Content-Description") {
			case "invitation-request":
				if found || json.Unmarshal(b, &request) != nil {
					return webError(400, "invalid invitation request")
				}
				found = true
			case "invitation-data":
				data = b
			default:
				return webError(400, "unknown invitation part")
			}
		}
		if !found {
			return webError(400, "missing invitation request")
		}
	} else if media == "application/json" {
		if json.Unmarshal(body, &request) != nil {
			return webError(400, "invalid invitation request")
		}
	} else {
		return webError(400, "unsupported invitation content type")
	}
	if len(request.To) == 0 || len(request.To) > 16 || len(request.Message) > 1024 {
		return webError(400, "invalid invitation recipients or message")
	}
	to := map[*client]bool{}
	for _, id := range request.To {
		p := s.webUser(c, id)
		if p == nil {
			return webError(404, "invitation recipient is not connected")
		}
		if p != c {
			to[p] = true
		}
	}
	// Bound retained invitations and replace stale/previous invitations to the
	// same player. A resend raises a fresh push for the current private session.
	for id, i := range w.Invitations {
		if time.Since(i.Created) >= time.Hour || to[i.To] {
			delete(w.Invitations, id)
		}
	}
	if len(w.Invitations)+len(to) > 128 {
		return webError(503, "invitation capacity reached")
	}
	for p := range to {
		id := fmt.Sprintf("i%016x", s.nextWebSession)
		s.nextWebSession++
		invite := &webInvitation{ID: id, From: c, To: p, Session: w, Created: time.Now().UTC(), Message: request.Message, Data: append([]byte(nil), data...)}
		w.Invitations[id] = invite
		s.webPush(c, p, "sessionInvitation", "np:service:invitation", webInvitationJSON(invite), [][2]string{{"sessionId", w.ID}, {"invitationId", id}, {"validUntil", strconv.FormatInt(invite.Created.Add(time.Hour).UnixMilli(), 10)}})
	}
	return webResponse{status: 204}
}
func (s *Server) webInvitationRequest(c *client, r *http.Request, path []string, body []byte) webResponse {
	if len(path) == 0 && r.Method == http.MethodGet {
		invitations := []*webInvitation{}
		for _, w := range s.webSessions {
			for _, i := range w.Invitations {
				if i.To == c && !i.Used && time.Since(i.Created) < time.Hour {
					invitations = append(invitations, i)
				}
			}
		}
		sort.Slice(invitations, func(i, j int) bool { return invitations[i].ID < invitations[j].ID })
		values := make([]any, 0, len(invitations))
		for _, i := range invitations {
			values = append(values, webInvitationJSON(i))
		}
		return webList(r, "invitations", values)
	}
	if len(path) > 0 {
		var invite *webInvitation
		for _, w := range s.webSessions {
			if i := w.Invitations[path[0]]; i != nil && i.To == c {
				invite = i
				break
			}
		}
		if invite == nil {
			return webError(404, "invitation not found")
		}
		if len(path) == 2 && path[1] == "invitationData" && r.Method == http.MethodGet {
			return webResponse{status: 200, data: append([]byte(nil), invite.Data...), contentType: "application/octet-stream"}
		}
		if len(path) == 1 {
			switch r.Method {
			case http.MethodGet:
				return webJSON(200, webInvitationJSON(invite))
			case http.MethodDelete:
				delete(invite.Session.Invitations, invite.ID)
				return webResponse{status: 204}
			case http.MethodPut:
				var update struct {
					Used bool `json:"usedFlag"`
				}
				if json.Unmarshal(body, &update) != nil || !update.Used {
					return webError(400, "expected usedFlag=true")
				}
				invite.Used = true
				return webResponse{status: 204}
			}
		}
	}
	return webError(404, "unsupported private invitation route or method")
}

// Notification 17's payload is a sequence of LE length-prefixed strings with a
// service label and optional extended-data/account-id tail; see native client.cpp.
func (s *Server) webPush(from, to *client, service, kind string, value any, extra [][2]string) {
	jsonData, err := json.Marshal(value)
	if err != nil {
		return
	}
	var body []byte
	appendString := func(value string) {
		body = binary.LittleEndian.AppendUint32(body, uint32(len(value)))
		body = append(body, value...)
	}
	appendString(service)
	body = binary.LittleEndian.AppendUint32(body, 0)
	appendString(kind)
	appendString(string(jsonData))
	appendString(from.name)
	appendString(to.name)
	body = binary.LittleEndian.AppendUint32(body, uint32(len(extra)))
	for _, kv := range extra {
		appendString(kv[0])
		appendString(kv[1])
	}
	body = binary.LittleEndian.AppendUint64(body, from.account)
	body = binary.LittleEndian.AppendUint64(body, to.account)
	to.queue(packet{kind: 2, command: 17, body: body})
}
