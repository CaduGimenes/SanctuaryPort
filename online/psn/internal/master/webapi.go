// SPDX-License-Identifier: GPL-2.0-or-later
package master

// Private NP Toolkit WebAPI. The route and field vocabulary is taken from the
// installed NP Toolkit; identities, presence and session contents come from the
// live master. This does not proxy requests to Sony or authenticate title services.
import (
	"bytes"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"mime/multipart"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"time"
)

const webBodyLimit = 2 << 20

type webPresence struct {
	GameStatus string `json:"gameStatus"`
	GameData   string `json:"gameData"`
}

type sessionFields struct {
	Privacy   string   `json:"sessionPrivacy"`
	Type      string   `json:"sessionType"`
	Max       int      `json:"sessionMaxUser"`
	Name      string   `json:"sessionName"`
	Status    string   `json:"sessionStatus"`
	Locked    bool     `json:"sessionLockFlag"`
	Platforms []string `json:"availablePlatforms"`
}
type webSession struct {
	ID                      string
	Fields                  sessionFields
	Owner                   *client
	Members                 map[*client]bool
	Created                 time.Time
	Data, Changeable, Image []byte
	Invitations             map[string]*webInvitation
}
type webResponse struct {
	status       int
	value        any
	data         []byte
	contentType  string
	errorMessage string
}

func webJSON(status int, value any) webResponse { return webResponse{status: status, value: value} }
func webError(status int, message string) webResponse {
	r := webJSON(status, map[string]any{"error": map[string]any{"code": status, "message": message}})
	r.errorMessage = message
	return r
}

// WebHandler shares the authenticated sessions of the binary master protocol.
// A bearer token expires when its TCP session closes; no separate accounts exist.
func (s *Server) WebHandler() http.Handler { return http.HandlerFunc(s.serveWebAPI) }
func (s *Server) serveWebAPI(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	const prefix = "Bearer "
	token := strings.TrimPrefix(r.Header.Get("Authorization"), prefix)
	if !strings.HasPrefix(r.Header.Get("Authorization"), prefix) || len(token) != 64 {
		writeWebResponse(w, webError(http.StatusUnauthorized, "active master session required"))
		return
	}
	// Read bodies outside the master lock. The serving HTTP server also sets a
	// read deadline. Never let a partial HTTP upload stall gameplay commands.
	s.mu.Lock()
	c := s.webClient(token)
	s.mu.Unlock()
	if c == nil {
		writeWebResponse(w, webError(http.StatusUnauthorized, "active master session required"))
		return
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, webBodyLimit))
	if err != nil {
		writeWebResponse(w, webError(http.StatusRequestEntityTooLarge, "request body too large"))
		return
	}
	s.mu.Lock()
	var result webResponse
	if s.webClient(token) != c {
		result = webError(http.StatusUnauthorized, "master session ended")
	} else {
		result = s.webRequest(c, r, body)
	}
	// Encode a snapshot while locked, then release before writing to a socket.
	if result.value != nil {
		result.data, err = json.Marshal(result.value)
		result.contentType = "application/json; charset=utf-8"
		if err != nil {
			result = webResponse{status: 500}
		}
	}
	s.mu.Unlock()
	if result.status >= http.StatusBadRequest {
		s.cfg.Logger.Warn("private WebAPI request failed", "method", r.Method, "path", r.URL.Path, "status", result.status, "reason", result.errorMessage)
	}
	writeWebResponse(w, result)
}
func writeWebResponse(w http.ResponseWriter, r webResponse) {
	if r.value != nil && r.data == nil {
		r.data, _ = json.Marshal(r.value)
		r.contentType = "application/json; charset=utf-8"
	}
	if r.contentType != "" {
		w.Header().Set("Content-Type", r.contentType)
	}
	w.WriteHeader(r.status)
	if r.status != http.StatusNoContent {
		_, _ = w.Write(r.data)
	}
}
func (s *Server) webClient(token string) *client {
	if s.closing {
		return nil
	}
	for _, c := range s.sessions {
		select {
		case <-c.done:
			continue
		default:
		}
		if len(c.token) == len(token) && subtle.ConstantTimeCompare([]byte(c.token), []byte(token)) == 1 {
			return c
		}
	}
	return nil
}
func (s *Server) webUser(c *client, id string) *client {
	if id == "me" {
		return c
	}
	for _, p := range s.sessions {
		if p.title == c.title && (p.name == id || strconv.FormatUint(p.account, 10) == id) {
			return p
		}
	}
	return nil
}
func webUser(c *client) map[string]any {
	return map[string]any{"accountId": strconv.FormatUint(c.account, 10), "onlineId": c.name}
}
func webProfile(c, self *client) map[string]any {
	relation := "friend"
	if c == self {
		relation = "me"
	}
	return map[string]any{"user": webUser(c), "region": "us", "avatarUrl": "", "aboutMe": "", "languagesUsed": []string{"en"}, "isOfficiallyVerified": false, "relation": relation,
		"personalDetail": map[string]any{"displayName": c.name, "firstName": "", "middleName": "", "lastName": "", "profilePictureUrl": ""}, "presence": webPresenceJSON(c)}
}
func webPresenceJSON(c *client) map[string]any {
	// The PSN title ID includes the service suffix; the binary master login
	// uses the CUSA serial. This master accepts only CUSA00242.
	entry := map[string]any{"onlineStatus": "online", "platform": "PS4", "gameStatus": c.presence.GameStatus, "gameData": c.presence.GameData, "gameTitleInfo": map[string]any{"npTitleId": "CUSA00242_00", "titleName": "Diablo III"}}
	return map[string]any{"onlineStatus": "online", "platform": "PS4", "incontextInfoList": []any{entry}, "platformInfoList": []any{entry}}
}
func webNpID(c *client) string {
	// Same zero-filled 36-byte OrbisNpId as the runtime's SetNpId. Toolkit
	// decodes the first 20 bytes into the online ID; accountId is separate.
	var id [36]byte
	copy(id[:16], c.name)
	return base64.StdEncoding.EncodeToString(id[:])
}
func (s *Server) webPeers(c *client) []*client {
	peers := make([]*client, 0, len(s.sessions))
	for _, p := range s.sessions {
		if p != c && p.title == c.title {
			peers = append(peers, p)
		}
	}
	sort.Slice(peers, func(i, j int) bool { return peers[i].name < peers[j].name })
	return peers
}
func webPage(r *http.Request, n int) (int, int, bool) {
	offset, limit := 0, 100
	var err error
	if v := r.URL.Query().Get("offset"); v != "" {
		offset, err = strconv.Atoi(v)
		if err != nil || offset < 0 {
			return 0, 0, false
		}
	}
	if v := r.URL.Query().Get("limit"); v != "" {
		limit, err = strconv.Atoi(v)
		if err != nil || limit < 1 || limit > 500 {
			return 0, 0, false
		}
	}
	if offset > n {
		offset = n
	}
	end := offset + limit
	if end > n {
		end = n
	}
	return offset, end, true
}
func webList(r *http.Request, key string, values []any) webResponse {
	a, b, ok := webPage(r, len(values))
	if !ok {
		return webError(400, "invalid pagination")
	}
	return webJSON(200, map[string]any{"totalResults": len(values), "start": a, "size": b - a, key: values[a:b]})
}
func (s *Server) webRequest(c *client, r *http.Request, body []byte) webResponse {
	path := strings.Split(strings.Trim(r.URL.Path, "/"), "/")
	if len(path) < 2 || path[0] != "v1" {
		return webError(404, "unsupported private WebAPI route")
	}
	if path[1] == "sessions" {
		return s.webSessionRequest(c, r, path[2:], body)
	}
	if r.Method == http.MethodGet && path[1] == "friendOnlineIdLists" && len(path) == 2 {
		lists := []any{}
		for _, id := range strings.Split(r.URL.Query().Get("accountIds"), ",") {
			if player := s.webUser(c, id); player != nil {
				friends := []any{}
				for _, peer := range s.webPeers(player) {
					friends = append(friends, webUser(peer))
				}
				lists = append(lists, map[string]any{"user": webUser(player), "friendOnlineIdList": friends})
			}
		}
		return webJSON(200, map[string]any{"friendOnlineIdLists": lists})
	}
	if r.Method == http.MethodGet && path[1] == "profiles" && len(path) == 2 {
		profiles := []any{}
		for _, id := range strings.Split(r.URL.Query().Get("accountIds"), ",") {
			if p := s.webUser(c, id); p != nil {
				profiles = append(profiles, webProfile(p, c))
			}
		}
		return webJSON(200, map[string]any{"size": len(profiles), "profiles": profiles})
	}
	if len(path) < 4 || path[1] != "users" {
		return webError(404, "unsupported private WebAPI route")
	}
	if path[2] == "@users" && path[3] == "sessions" && len(path) == 4 && r.Method == http.MethodGet {
		ids := strings.Split(r.URL.Query().Get("accountIds"), ",")
		users := make([]any, 0, len(ids))
		for _, id := range ids {
			if p := s.webUser(c, id); p != nil {
				user := webUser(p)
				sessions := []any{}
				keys := []string{}
				for key, w := range s.webSessions {
					if w.Members[p] && w.Owner.title == c.title && (w.Fields.Privacy == "public" || w.Members[c] || w.invited(c)) {
						keys = append(keys, key)
					}
				}
				sort.Strings(keys)
				for _, key := range keys {
					sessions = append(sessions, webSessionJSON(s.webSessions[key]))
				}
				user["sessions"] = sessions
				users = append(users, user)
			}
		}
		return webJSON(200, map[string]any{"users": users})
	}
	p := s.webUser(c, path[2])
	if p == nil {
		return webError(404, "player is not connected")
	}
	switch path[3] {
	case "profile":
		if len(path) == 4 && r.Method == http.MethodGet {
			return webJSON(200, webProfile(p, c))
		}
	case "friendList":
		if len(path) == 4 && r.Method == http.MethodGet && p == c {
			peers := []any{}
			for _, peer := range s.webPeers(c) {
				peers = append(peers, webProfile(peer, c))
			}
			return webList(r, "friendList", peers)
		}
	case "blockingUsers":
		if len(path) == 4 && r.Method == http.MethodGet && p == c {
			return webList(r, "blockingUsers", []any{})
		}
	case "verifiedAccountsByTitle":
		if len(path) == 4 && r.Method == http.MethodGet && p == c {
			return webList(r, "verifiedUsers", []any{})
		}
	case "presence":
		if r.Method == http.MethodGet && len(path) == 4 {
			// GetPresence's parser requires both the base64 NP identity and the
			// nested presence object. FriendList embeds just the latter.
			return webJSON(200, map[string]any{"npId": webNpID(p), "presence": webPresenceJSON(p)})
		}
		if r.Method == http.MethodGet && len(path) == 5 && (path[4] == "gameData" || path[4] == "gameStatus") {
			if path[4] == "gameStatus" {
				return webJSON(200, map[string]any{"gameStatus": p.presence.GameStatus})
			}
			return webJSON(200, map[string]any{"gameData": p.presence.GameData})
		}
		if p != c {
			return webError(403, "presence belongs to another player")
		}
		if r.Method == http.MethodDelete && len(path) == 5 && (path[4] == "gameData" || path[4] == "gameStatus") {
			if path[4] == "gameData" {
				p.presence.GameData = ""
			} else {
				p.presence.GameStatus = ""
			}
			return webResponse{status: 204}
		}
		if r.Method == http.MethodPut && len(path) == 5 && (path[4] == "inGamePresence" || path[4] == "gameData" || path[4] == "gameStatus") {
			var update webPresence
			if len(body) > 65536 || json.Unmarshal(body, &update) != nil {
				return webError(400, "invalid presence")
			}
			if _, err := base64.StdEncoding.DecodeString(update.GameData); err != nil {
				return webError(400, "game data must be base64")
			}
			if path[4] != "gameData" {
				if len(update.GameStatus) > 1024 {
					return webError(400, "game status too long")
				}
				p.presence.GameStatus = update.GameStatus
			}
			if path[4] != "gameStatus" {
				p.presence.GameData = update.GameData
			}
			return webResponse{status: 204}
		}
	case "friends":
		if len(path) == 5 && path[4] == "sessions" && r.Method == http.MethodGet && p == c {
			return s.webSessionList(c, r, func(w *webSession) bool { return !w.Members[c] })
		}
	case "invitations":
		if p == c {
			return s.webInvitationRequest(c, r, path[4:], body)
		}
	}
	return webError(404, "unsupported private WebAPI route or method")
}

func sessionInput(contentType string, body []byte) (sessionFields, []byte, []byte, []byte, error) {
	var fields sessionFields
	media, params, err := mime.ParseMediaType(contentType)
	if err != nil {
		return fields, nil, nil, nil, err
	}
	var data, changeable, image []byte
	if media == "application/json" {
		err = json.Unmarshal(body, &fields)
	} else if media == "multipart/mixed" {
		reader := multipart.NewReader(bytes.NewReader(body), params["boundary"])
		found := false
		for {
			part, e := reader.NextPart()
			if e == io.EOF {
				break
			}
			if e != nil {
				return fields, nil, nil, nil, e
			}
			b, e := io.ReadAll(io.LimitReader(part, webBodyLimit+1))
			if len(b) > webBodyLimit {
				return fields, nil, nil, nil, fmt.Errorf("multipart body too large")
			}
			// This Toolkit omits the final boundary from the transmitted size.
			// Only accept that EOF when the final part supplies one valid length
			// and every declared byte arrived. Never mask a truncated attachment.
			lengths := part.Header.Values("Content-Length")
			if len(lengths) != 0 {
				if len(lengths) != 1 {
					return fields, nil, nil, nil, fmt.Errorf("invalid session part length")
				}
				length, parseErr := strconv.ParseUint(lengths[0], 10, 64)
				if parseErr != nil || length != uint64(len(b)) {
					return fields, nil, nil, nil, fmt.Errorf("session part length mismatch")
				}
			}
			endAtLength := errors.Is(e, io.ErrUnexpectedEOF) && len(lengths) == 1
			if e != nil && !endAtLength {
				return fields, nil, nil, nil, fmt.Errorf("incomplete session part")
			}
			switch part.Header.Get("Content-Description") {
			case "session-request":
				if found {
					return fields, nil, nil, nil, fmt.Errorf("duplicate session request")
				}
				found = true
				err = json.Unmarshal(b, &fields)
			case "session-data":
				data = b
			case "changeable-session-data":
				changeable = b
			case "session-image":
				image = b
			default:
				return fields, nil, nil, nil, fmt.Errorf("unknown session part")
			}
			if err != nil {
				return fields, nil, nil, nil, err
			}
			if endAtLength {
				break
			}
		}
		if !found {
			err = fmt.Errorf("missing session request")
		}
	} else {
		err = fmt.Errorf("unsupported session content type")
	}
	if err != nil {
		return fields, nil, nil, nil, err
	}
	if len(data) > 65536 || len(changeable) > 65536 || len(image) > 1<<20 {
		return fields, nil, nil, nil, fmt.Errorf("session attachment too large")
	}
	if fields.Privacy != "public" && fields.Privacy != "private" || fields.Type != "owner-bind" && fields.Type != "owner-migration" || fields.Max < 1 || fields.Max > 4 || len(fields.Name) > 256 || len(fields.Status) > 1024 {
		return fields, nil, nil, nil, fmt.Errorf("invalid session fields")
	}
	if len(fields.Platforms) == 0 {
		fields.Platforms = []string{"PS4"}
	}
	for _, platform := range fields.Platforms {
		if platform != "PS4" && platform != "PSVITA" {
			return fields, nil, nil, nil, fmt.Errorf("unsupported platform")
		}
	}
	return fields, data, changeable, image, nil
}
func (s *Server) webSessionRequest(c *client, r *http.Request, path []string, body []byte) webResponse {
	if len(path) == 0 {
		if r.Method != http.MethodPost {
			return webError(404, "unsupported session method")
		}
		if len(s.webSessions) >= s.cfg.MaxRooms {
			return webError(503, "session capacity reached")
		}
		fields, data, changeable, image, err := sessionInput(r.Header.Get("Content-Type"), body)
		if err != nil {
			return webError(400, err.Error())
		}
		id := fmt.Sprintf("%016x", s.nextWebSession)
		s.nextWebSession++
		s.webSessions[id] = &webSession{ID: id, Fields: fields, Owner: c, Members: map[*client]bool{c: true}, Created: time.Now().UTC(), Data: data, Changeable: changeable, Image: image, Invitations: map[string]*webInvitation{}}
		return webJSON(201, map[string]any{"sessionId": id})
	}
	w := s.webSessions[path[0]]
	if w == nil || w.Owner.title != c.title || w.Fields.Privacy == "private" && !w.Members[c] && !w.invited(c) {
		return webError(404, "session not found")
	}
	if len(path) == 1 {
		switch r.Method {
		case http.MethodGet:
			return webJSON(200, webSessionJSON(w))
		case http.MethodDelete:
			if w.Owner != c {
				return webError(403, "session owner required")
			}
			delete(s.webSessions, w.ID)
			return webResponse{status: 204}
		case http.MethodPut:
			if w.Owner != c {
				return webError(403, "session owner required")
			}
			// Toolkit updates contain only changed fields; merge with the current
			// description before the same validation used by creation.
			original, _ := json.Marshal(w.Fields)
			var fields map[string]json.RawMessage
			var update map[string]json.RawMessage
			_ = json.Unmarshal(original, &fields)
			if json.Unmarshal(body, &update) != nil {
				return webError(400, "invalid session update")
			}
			for key, v := range update {
				fields[key] = v
			}
			merged, _ := json.Marshal(fields)
			f, _, _, _, err := sessionInput("application/json", merged)
			if err != nil || f.Max < len(w.Members) {
				return webError(400, "invalid session update")
			}
			w.Fields = f
			return webResponse{status: 204}
		}
	}
	if len(path) == 2 && path[1] == "members" && r.Method == http.MethodPost {
		if w.Members[c] {
			return webResponse{status: 204}
		}
		if w.Fields.Locked || len(w.Members) >= w.Fields.Max {
			return webError(409, "session is locked or full")
		}
		w.Members[c] = true
		return webResponse{status: 204}
	}
	if len(path) == 2 && path[1] == "invitations" && r.Method == http.MethodPost {
		return s.webSendInvitations(c, w, r, body)
	}
	if len(path) == 3 && path[1] == "members" && path[2] == "me" && r.Method == http.MethodDelete {
		if !w.Members[c] {
			return webError(404, "not a session member")
		}
		s.leaveWebSession(c, w)
		return webResponse{status: 204}
	}
	if len(path) == 2 && (path[1] == "sessionData" || path[1] == "changeableSessionData" || path[1] == "sessionImage") {
		data := &w.Data
		ct := "application/octet-stream"
		limit := 65536
		if path[1] == "changeableSessionData" {
			data = &w.Changeable
		}
		if path[1] == "sessionImage" {
			data = &w.Image
			ct = "image/jpeg"
			limit = 1 << 20
		}
		if r.Method == http.MethodGet {
			return webResponse{status: 200, data: append([]byte(nil), (*data)...), contentType: ct}
		}
		if r.Method == http.MethodPut && path[1] != "sessionData" {
			if w.Owner != c {
				return webError(403, "session owner required")
			}
			if len(body) > limit {
				return webError(413, "attachment too large")
			}
			*data = append([]byte(nil), body...)
			return webResponse{status: 204}
		}
	}
	return webError(404, "unsupported private session route or method")
}
func webSessionJSON(w *webSession) map[string]any {
	accounts := make([]*client, 0, len(w.Members))
	for c := range w.Members {
		accounts = append(accounts, c)
	}
	sort.Slice(accounts, func(i, j int) bool { return accounts[i].account < accounts[j].account })
	members := make([]any, 0, len(accounts))
	for _, c := range accounts {
		member := webUser(c)
		member["platform"] = "PS4"
		members = append(members, member)
	}
	return map[string]any{"platform": "PS4", "sessionId": w.ID, "sessionPrivacy": w.Fields.Privacy, "sessionType": w.Fields.Type, "sessionMaxUser": w.Fields.Max, "sessionName": w.Fields.Name, "sessionStatus": w.Fields.Status, "sessionLockFlag": w.Fields.Locked, "availablePlatforms": w.Fields.Platforms, "sessionCreateTimestamp": w.Created.UnixMilli(), "memberCount": len(members), "sessionCreator": webUser(w.Owner), "members": members, "friends": members}
}
func (s *Server) webSessionList(c *client, r *http.Request, predicate func(*webSession) bool) webResponse {
	ids := []string{}
	for id, w := range s.webSessions {
		if w.Owner.title == c.title && (w.Fields.Privacy == "public" || w.Members[c]) && predicate(w) {
			ids = append(ids, id)
		}
	}
	sort.Strings(ids)
	list := make([]any, 0, len(ids))
	for _, id := range ids {
		list = append(list, webSessionJSON(s.webSessions[id]))
	}
	return webList(r, "sessions", list)
}
func (s *Server) leaveWebSession(c *client, w *webSession) {
	delete(w.Members, c)
	if len(w.Members) == 0 || w.Owner == c && w.Fields.Type == "owner-bind" {
		delete(s.webSessions, w.ID)
		return
	}
	if w.Owner == c {
		var next *client
		for p := range w.Members {
			if next == nil || p.account < next.account {
				next = p
			}
		}
		w.Owner = next
	}
}
func (s *Server) leaveWebSessions(c *client) {
	for _, w := range s.webSessions {
		if w.Members[c] {
			s.leaveWebSession(c, w)
		}
		for id, invite := range w.Invitations {
			if invite.To == c {
				delete(w.Invitations, id)
			}
		}
	}
}
