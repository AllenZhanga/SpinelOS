package main

import (
	"bufio"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"sync"
	"time"
)

const (
	FrameControl byte = 0x01
	FrameRelay   byte = 0x02

	MaxFrameBytes = 8 * 1024 * 1024
)

type Account struct {
	Name         string   `json:"name"`
	SaltHex      string   `json:"salt_hex"`
	PasswordHash string   `json:"password_hash"`
	CreatedAt    int64    `json:"created_at"`
	DeviceIDs    []string `json:"device_ids"`
}

type DeviceRecord struct {
	DeviceID       string   `json:"device_id"`
	Account        string   `json:"account"`
	Name           string   `json:"name"`
	PubKeyHex      string   `json:"pub_key_hex"`
	AllowRemote    bool     `json:"allow_remote"`
	SandboxRunning bool     `json:"sandbox_running"`
	LanAddrs       []string `json:"lan_addrs"`
	IPv6Addrs      []string `json:"ipv6_addrs"`
	UdpMapped      string   `json:"udp_mapped"`
	Token          string   `json:"token"`
	LastSeen       int64    `json:"last_seen"`
}

type PairCode struct {
	Code      string `json:"code"`
	Account   string `json:"account"`
	ExpiresAt int64  `json:"expires_at"`
}

type PersistedState struct {
	Accounts map[string]*Account      `json:"accounts"`
	Devices  map[string]*DeviceRecord `json:"devices"`
}

type ClientConn struct {
	mu          sync.Mutex
	conn        net.Conn
	bw          *bufio.Writer
	deviceID    string
	account     string
	relaySid    string
	ephemeral   bool
	isHeartbeat bool
}

func (c *ClientConn) SendControl(v any) error {
	payload, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return c.SendFrame(FrameControl, payload)
}

func (c *ClientConn) SendFrame(ftype byte, payload []byte) error {
	if len(payload) > MaxFrameBytes {
		return fmt.Errorf("frame too large: %d", len(payload))
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	var hdr [5]byte
	hdr[0] = ftype
	binary.BigEndian.PutUint32(hdr[1:5], uint32(len(payload)))
	if _, err := c.bw.Write(hdr[:]); err != nil {
		return err
	}
	if len(payload) > 0 {
		if _, err := c.bw.Write(payload); err != nil {
			return err
		}
	}
	return c.bw.Flush()
}

type RelayPair struct {
	a *ClientConn
	b *ClientConn
}

type RelayServer struct {
	mu           sync.RWMutex
	dataDir      string
	accounts     map[string]*Account
	devices      map[string]*DeviceRecord
	pairCodes    map[string]*PairCode
	online       map[string]*ClientConn
	tunnels      map[string]*RelayPair
	punchWaiters map[string]*ClientConn
}

func NewRelayServer(dataDir string) (*RelayServer, error) {
	if err := os.MkdirAll(dataDir, 0700); err != nil {
		return nil, err
	}
	s := &RelayServer{
		dataDir:      dataDir,
		accounts:     make(map[string]*Account),
		devices:      make(map[string]*DeviceRecord),
		pairCodes:    make(map[string]*PairCode),
		online:       make(map[string]*ClientConn),
		tunnels:      make(map[string]*RelayPair),
		punchWaiters: make(map[string]*ClientConn),
	}
	_ = s.loadState()
	return s, nil
}

func (s *RelayServer) statePath() string {
	return filepath.Join(s.dataDir, "state.json")
}

func (s *RelayServer) loadState() error {
	b, err := os.ReadFile(s.statePath())
	if err != nil {
		return err
	}
	var st PersistedState
	if err := json.Unmarshal(b, &st); err != nil {
		return err
	}
	if st.Accounts != nil {
		s.accounts = st.Accounts
	}
	if st.Devices != nil {
		s.devices = st.Devices
	}
	return nil
}

func (s *RelayServer) saveStateLocked() error {
	st := PersistedState{
		Accounts: s.accounts,
		Devices:  s.devices,
	}
	b, err := json.MarshalIndent(st, "", "  ")
	if err != nil {
		return err
	}
	tmp := s.statePath() + ".tmp"
	if err := os.WriteFile(tmp, b, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, s.statePath())
}

func hashPassword(password, saltHex string) string {
	h := sha256.New()
	h.Write([]byte("spinel-relay-v1:"))
	h.Write([]byte(saltHex))
	h.Write([]byte(":"))
	h.Write([]byte(password))
	sum := h.Sum(nil)
	for i := 0; i < 4096; i++ {
		h.Reset()
		h.Write(sum)
		sum = h.Sum(nil)
	}
	return hex.EncodeToString(sum)
}

func randomHex(nBytes int) string {
	b := make([]byte, nBytes)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

func randomDigits(n int) string {
	out := make([]byte, n)
	for i := 0; i < n; i++ {
		v, err := rand.Int(rand.Reader, big.NewInt(10))
		if err != nil {
			out[i] = '0'
		} else {
			out[i] = byte('0' + v.Int64())
		}
	}
	return string(out)
}

type ControlMsg struct {
	Type           string   `json:"type"`
	ReqID          string   `json:"req_id,omitempty"`
	Account        string   `json:"account,omitempty"`
	Password       string   `json:"password,omitempty"`
	Token          string   `json:"token,omitempty"`
	PairCode       string   `json:"pair_code,omitempty"`
	DeviceID       string   `json:"device_id,omitempty"`
	DeviceName     string   `json:"device_name,omitempty"`
	PubKeyHex      string   `json:"pub_key_hex,omitempty"`
	AllowRemote    bool     `json:"allow_remote,omitempty"`
	SandboxRunning bool     `json:"sandbox_running,omitempty"`
	LanAddrs       []string `json:"lan_addrs,omitempty"`
	IPv6Addrs      []string `json:"ipv6_addrs,omitempty"`
	UdpMapped      string   `json:"udp_mapped,omitempty"`
	TargetDeviceID string   `json:"target_device_id,omitempty"`
	SessionID      string   `json:"session_id,omitempty"`
	Candidates     []string `json:"candidates,omitempty"`
	EphemeralPub   string   `json:"ephemeral_pub,omitempty"`
	Signature      string   `json:"signature,omitempty"`
	Ephemeral      bool     `json:"ephemeral,omitempty"`
	Role           string   `json:"role,omitempty"`
}

func readFrame(r *bufio.Reader) (byte, []byte, error) {
	var hdr [5]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return 0, nil, err
	}
	sz := binary.BigEndian.Uint32(hdr[1:5])
	if sz > MaxFrameBytes {
		return 0, nil, fmt.Errorf("oversized frame: %d", sz)
	}
	buf := make([]byte, sz)
	if sz > 0 {
		if _, err := io.ReadFull(r, buf); err != nil {
			return 0, nil, err
		}
	}
	return hdr[0], buf, nil
}

func (s *RelayServer) handleConn(conn net.Conn) {
	cc := &ClientConn{
		conn: conn,
		bw:   bufio.NewWriterSize(conn, 64*1024),
	}
	defer func() {
		s.cleanupConn(cc)
		_ = conn.Close()
	}()

	br := bufio.NewReaderSize(conn, 64*1024)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(90 * time.Second))
		ftype, payload, err := readFrame(br)
		if err != nil {
			return
		}
		if ftype == FrameRelay {
			s.forwardRelayPayload(cc, payload)
			continue
		}
		if ftype != FrameControl {
			return
		}
		var msg ControlMsg
		if err := json.Unmarshal(payload, &msg); err != nil {
			_ = cc.SendControl(map[string]any{"type": "error", "error": "invalid json"})
			continue
		}
		s.handleControl(cc, &msg)
	}
}

func (s *RelayServer) forwardRelayPayload(cc *ClientConn, payload []byte) {
	if len(payload) < 16 {
		return
	}
	sid := hex.EncodeToString(payload[:16])
	s.mu.RLock()
	pair := s.tunnels[sid]
	var peer *ClientConn
	if pair != nil {
		if pair.a == cc {
			peer = pair.b
		} else if pair.b == cc {
			peer = pair.a
		}
	}
	s.mu.RUnlock()
	if peer != nil {
		_ = peer.SendFrame(FrameRelay, payload)
	}
}

func (s *RelayServer) cleanupConn(cc *ClientConn) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if cc.deviceID != "" && s.online[cc.deviceID] == cc {
		delete(s.online, cc.deviceID)
	}
	for sid, waiter := range s.punchWaiters {
		if waiter == cc {
			delete(s.punchWaiters, sid)
		}
	}
	if cc.relaySid != "" {
		if p, ok := s.tunnels[cc.relaySid]; ok {
			if p.a == cc {
				p.a = nil
			}
			if p.b == cc {
				p.b = nil
			}
			if p.a == nil && p.b == nil {
				delete(s.tunnels, cc.relaySid)
			}
		}
	}
}

func (s *RelayServer) sendResp(cc *ClientConn, reqID string, ok bool, errMsg string, extra map[string]any) {
	m := map[string]any{
		"type":   "resp",
		"req_id": reqID,
		"ok":     ok,
	}
	if errMsg != "" {
		m["error"] = errMsg
	}
	for k, v := range extra {
		m[k] = v
	}
	_ = cc.SendControl(m)
}

func (s *RelayServer) deviceListLocked(account string) []map[string]any {
	acc := s.accounts[account]
	if acc == nil {
		return nil
	}
	out := make([]map[string]any, 0, len(acc.DeviceIDs))
	for _, did := range acc.DeviceIDs {
		d := s.devices[did]
		if d == nil {
			continue
		}
		_, isOnline := s.online[did]
		out = append(out, map[string]any{
			"device_id":       d.DeviceID,
			"name":            d.Name,
			"pub_key_hex":     d.PubKeyHex,
			"online":          isOnline,
			"allow_remote":    d.AllowRemote,
			"sandbox_running": d.SandboxRunning,
			"lan_addrs":       d.LanAddrs,
			"ipv6_addrs":      d.IPv6Addrs,
			"udp_mapped":      d.UdpMapped,
			"last_seen":       d.LastSeen,
		})
	}
	return out
}

func (s *RelayServer) bindDeviceLocked(cc *ClientConn, account string, msg *ControlMsg, issueToken bool) string {
	acc := s.accounts[account]
	d := s.devices[msg.DeviceID]
	if d == nil {
		d = &DeviceRecord{
			DeviceID: msg.DeviceID,
			Account:  account,
		}
		s.devices[msg.DeviceID] = d
		acc.DeviceIDs = append(acc.DeviceIDs, msg.DeviceID)
	}
	if d.Account != account {
		return ""
	}
	if msg.DeviceName != "" {
		d.Name = msg.DeviceName
	}
	if msg.PubKeyHex != "" {
		d.PubKeyHex = msg.PubKeyHex
	}
	d.AllowRemote = msg.AllowRemote
	d.SandboxRunning = msg.SandboxRunning
	d.LanAddrs = msg.LanAddrs
	d.IPv6Addrs = msg.IPv6Addrs
	if msg.UdpMapped != "" {
		d.UdpMapped = msg.UdpMapped
	}
	d.LastSeen = time.Now().Unix()
	if issueToken || d.Token == "" {
		d.Token = randomHex(24)
	}
	if cc.deviceID == msg.DeviceID || cc.deviceID == "" {
		cc.deviceID = msg.DeviceID
		cc.account = account
		cc.ephemeral = msg.Ephemeral
		if !msg.Ephemeral {
			existing := s.online[msg.DeviceID]
			if existing == nil || existing == cc || !existing.isHeartbeat || msg.Role == "hostd" {
				s.online[msg.DeviceID] = cc
			}
		}
	}
	_ = s.saveStateLocked()
	return d.Token
}

func (s *RelayServer) handleControl(cc *ClientConn, msg *ControlMsg) {
	switch msg.Type {
	case "register":
		if msg.Account == "" || len(msg.Password) < 6 || msg.DeviceID == "" {
			s.sendResp(cc, msg.ReqID, false, "账号、密码(至少6位)与设备ID不能为空", nil)
			return
		}
		s.mu.Lock()
		if s.accounts[msg.Account] != nil {
			s.mu.Unlock()
			s.sendResp(cc, msg.ReqID, false, "该 Spinel ID 已存在，请直接登录", nil)
			return
		}
		salt := randomHex(16)
		s.accounts[msg.Account] = &Account{
			Name:         msg.Account,
			SaltHex:      salt,
			PasswordHash: hashPassword(msg.Password, salt),
			CreatedAt:    time.Now().Unix(),
		}
		tok := s.bindDeviceLocked(cc, msg.Account, msg, true)
		devs := s.deviceListLocked(msg.Account)
		s.mu.Unlock()
		s.sendResp(cc, msg.ReqID, true, "", map[string]any{"account": msg.Account, "token": tok, "devices": devs})

	case "login":
		if msg.Account == "" || msg.DeviceID == "" {
			s.sendResp(cc, msg.ReqID, false, "账号与设备 ID 不能为空", nil)
			return
		}
		s.mu.Lock()
		acc := s.accounts[msg.Account]
		if acc == nil {
			s.mu.Unlock()
			s.sendResp(cc, msg.ReqID, false, "账号不存在，请先注册", nil)
			return
		}
		authOK := false
		if msg.Token != "" {
			if existing := s.devices[msg.DeviceID]; existing != nil && existing.Account == msg.Account {
				authOK = subtle.ConstantTimeCompare([]byte(existing.Token), []byte(msg.Token)) == 1
			}
		} else if msg.Password != "" {
			want := hashPassword(msg.Password, acc.SaltHex)
			authOK = subtle.ConstantTimeCompare([]byte(want), []byte(acc.PasswordHash)) == 1
		}
		if !authOK {
			s.mu.Unlock()
			s.sendResp(cc, msg.ReqID, false, "账号或密码/凭证无效", nil)
			return
		}
		if existing := s.devices[msg.DeviceID]; existing != nil && existing.Account != msg.Account {
			s.mu.Unlock()
			s.sendResp(cc, msg.ReqID, false, "设备已绑定其他账号", nil)
			return
		}
		tok := s.bindDeviceLocked(cc, msg.Account, msg, msg.Password != "")
		devs := s.deviceListLocked(msg.Account)
		s.mu.Unlock()
		s.sendResp(cc, msg.ReqID, true, "", map[string]any{"account": msg.Account, "token": tok, "devices": devs})

	case "pair_create":
		if cc.account == "" {
			s.sendResp(cc, msg.ReqID, false, "请先登录 Spinel ID", nil)
			return
		}
		s.mu.Lock()
		code := randomDigits(6)
		exp := time.Now().Add(5 * time.Minute).Unix()
		s.pairCodes[code] = &PairCode{Code: code, Account: cc.account, ExpiresAt: exp}
		s.mu.Unlock()
		s.sendResp(cc, msg.ReqID, true, "", map[string]any{"pair_code": code, "expires_at": exp})

	case "pair_claim":
		if msg.PairCode == "" || msg.DeviceID == "" {
			s.sendResp(cc, msg.ReqID, false, "配对码和设备 ID 不能为空", nil)
			return
		}
		s.mu.Lock()
		pc := s.pairCodes[msg.PairCode]
		if pc == nil || time.Now().Unix() > pc.ExpiresAt {
			delete(s.pairCodes, msg.PairCode)
			s.mu.Unlock()
			s.sendResp(cc, msg.ReqID, false, "配对码无效或已过期", nil)
			return
		}
		account := pc.Account
		delete(s.pairCodes, msg.PairCode)
		if existing := s.devices[msg.DeviceID]; existing != nil && existing.Account != account {
			s.mu.Unlock()
			s.sendResp(cc, msg.ReqID, false, "该设备已绑定其他账号", nil)
			return
		}
		tok := s.bindDeviceLocked(cc, account, msg, true)
		devs := s.deviceListLocked(account)
		s.mu.Unlock()
		s.sendResp(cc, msg.ReqID, true, "", map[string]any{"account": account, "token": tok, "devices": devs})

	case "heartbeat":
		if cc.account == "" || cc.deviceID == "" {
			s.sendResp(cc, msg.ReqID, false, "未认证连接", nil)
			return
		}
		s.mu.Lock()
		cc.ephemeral = false
		cc.isHeartbeat = true
		s.online[cc.deviceID] = cc
		d := s.devices[cc.deviceID]
		if d != nil {
			if msg.DeviceName != "" {
				d.Name = msg.DeviceName
			}
			d.AllowRemote = msg.AllowRemote
			d.SandboxRunning = msg.SandboxRunning
			d.LanAddrs = msg.LanAddrs
			d.IPv6Addrs = msg.IPv6Addrs
			if msg.UdpMapped != "" {
				d.UdpMapped = msg.UdpMapped
			}
			d.LastSeen = time.Now().Unix()
		}
		devs := s.deviceListLocked(cc.account)
		s.mu.Unlock()
		s.sendResp(cc, msg.ReqID, true, "", map[string]any{"devices": devs})

	case "device_list":
		if cc.account == "" {
			s.sendResp(cc, msg.ReqID, false, "未登录", nil)
			return
		}
		s.mu.RLock()
		devs := s.deviceListLocked(cc.account)
		s.mu.RUnlock()
		s.sendResp(cc, msg.ReqID, true, "", map[string]any{"devices": devs})

	case "device_remove":
		if cc.account == "" || msg.TargetDeviceID == "" {
			s.sendResp(cc, msg.ReqID, false, "参数缺失", nil)
			return
		}
		s.mu.Lock()
		d := s.devices[msg.TargetDeviceID]
		if d == nil || d.Account != cc.account {
			s.mu.Unlock()
			s.sendResp(cc, msg.ReqID, false, "设备不存在或无权操作", nil)
			return
		}
		delete(s.devices, msg.TargetDeviceID)
		if acc := s.accounts[cc.account]; acc != nil {
			filtered := acc.DeviceIDs[:0]
			for _, id := range acc.DeviceIDs {
				if id != msg.TargetDeviceID {
					filtered = append(filtered, id)
				}
			}
			acc.DeviceIDs = filtered
		}
		if peer := s.online[msg.TargetDeviceID]; peer != nil {
			delete(s.online, msg.TargetDeviceID)
			_ = peer.conn.Close()
		}
		_ = s.saveStateLocked()
		devs := s.deviceListLocked(cc.account)
		s.mu.Unlock()
		s.sendResp(cc, msg.ReqID, true, "", map[string]any{"devices": devs})

	case "punch_offer", "punch_answer":
		if cc.account == "" || msg.TargetDeviceID == "" {
			s.sendResp(cc, msg.ReqID, false, "未登录或目标设备为空", nil)
			return
		}
		s.mu.Lock()
		target := s.devices[msg.TargetDeviceID]
		var peerConn *ClientConn
		if msg.Type == "punch_answer" && msg.SessionID != "" {
			if w := s.punchWaiters[msg.SessionID]; w != nil {
				peerConn = w
				delete(s.punchWaiters, msg.SessionID)
			}
		}
		if peerConn == nil {
			peerConn = s.online[msg.TargetDeviceID]
		}
		if target == nil || target.Account != cc.account || peerConn == nil {
			s.mu.Unlock()
			s.sendResp(cc, msg.ReqID, false, "目标设备当前不在线", nil)
			return
		}
		if msg.Type == "punch_offer" && msg.SessionID != "" {
			s.punchWaiters[msg.SessionID] = cc
		}
		s.mu.Unlock()
		fwd := map[string]any{
			"type":           msg.Type,
			"from_device_id": cc.deviceID,
			"from_pub_hex":   s.getDevicePubKey(cc.deviceID),
			"session_id":     msg.SessionID,
			"candidates":     msg.Candidates,
			"ephemeral_pub":  msg.EphemeralPub,
			"signature":      msg.Signature,
		}
		if err := peerConn.SendControl(fwd); err != nil {
			s.sendResp(cc, msg.ReqID, false, "转发信令失败", nil)
			return
		}
		s.sendResp(cc, msg.ReqID, true, "", nil)

	case "relay_join":
		// Authenticates a secondary or primary TLS stream into a relay tunnel identified by 32-hex SessionID.
		if len(msg.SessionID) != 32 || msg.DeviceID == "" || msg.Token == "" {
			s.sendResp(cc, msg.ReqID, false, "无效中继会话参数", nil)
			return
		}
		s.mu.Lock()
		d := s.devices[msg.DeviceID]
		if d == nil || subtle.ConstantTimeCompare([]byte(d.Token), []byte(msg.Token)) != 1 {
			s.mu.Unlock()
			s.sendResp(cc, msg.ReqID, false, "中继设备凭证无效", nil)
			return
		}
		if msg.TargetDeviceID != "" {
			td := s.devices[msg.TargetDeviceID]
			if td == nil || td.Account != d.Account {
				s.mu.Unlock()
				s.sendResp(cc, msg.ReqID, false, "目标设备不属于同一账号", nil)
				return
			}
		}
		cc.deviceID = d.DeviceID
		cc.account = d.Account
		cc.relaySid = msg.SessionID
		pair := s.tunnels[msg.SessionID]
		if pair == nil {
			pair = &RelayPair{a: cc}
			s.tunnels[msg.SessionID] = pair
		} else if pair.a == nil || pair.a == cc {
			pair.a = cc
		} else {
			pair.b = cc
		}
		paired := (pair.a != nil && pair.b != nil)
		s.mu.Unlock()
		s.sendResp(cc, msg.ReqID, true, "", map[string]any{"paired": paired})

	default:
		s.sendResp(cc, msg.ReqID, false, "unknown command: "+msg.Type, nil)
	}
}

func (s *RelayServer) getDevicePubKey(did string) string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if d := s.devices[did]; d != nil {
		return d.PubKeyHex
	}
	return ""
}

func ensureTLSCert(certPath, keyPath string) (tls.Certificate, string, error) {
	if _, err := os.Stat(certPath); err == nil {
		if _, err2 := os.Stat(keyPath); err2 == nil {
			cert, loadErr := tls.LoadX509KeyPair(certPath, keyPath)
			if loadErr == nil && len(cert.Certificate) > 0 {
				fp := sha256.Sum256(cert.Certificate[0])
				return cert, "sha256-" + hex.EncodeToString(fp[:]), nil
			}
		}
	}

	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, "", err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 62))
	if err != nil {
		return tls.Certificate{}, "", err
	}
	tmpl := x509.Certificate{
		SerialNumber: serial,
		Subject:      pkix.Name{CommonName: "spinel-relay"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(10 * 365 * 24 * time.Hour),
		KeyUsage:     x509.KeyUsageKeyEncipherment | x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, &tmpl, &tmpl, &priv.PublicKey, priv)
	if err != nil {
		return tls.Certificate{}, "", err
	}
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	keyDER, err := x509.MarshalECPrivateKey(priv)
	if err != nil {
		return tls.Certificate{}, "", err
	}
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: keyDER})

	if err := os.MkdirAll(filepath.Dir(certPath), 0700); err != nil {
		return tls.Certificate{}, "", err
	}
	if err := os.WriteFile(certPath, certPEM, 0644); err != nil {
		return tls.Certificate{}, "", err
	}
	if err := os.WriteFile(keyPath, keyPEM, 0600); err != nil {
		return tls.Certificate{}, "", err
	}
	cert, err := tls.X509KeyPair(certPEM, keyPEM)
	if err != nil {
		return tls.Certificate{}, "", err
	}
	fp := sha256.Sum256(der)
	return cert, "sha256-" + hex.EncodeToString(fp[:]), nil
}

func runUDPStunLoop(pc net.PacketConn) {
	buf := make([]byte, 2048)
	for {
		n, addr, err := pc.ReadFrom(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		if n < 12 || string(buf[:8]) != "SPINELST" {
			continue
		}
		// Echo back JSON mapped address packet with original 8+4 byte nonce prefix: "SPINELOK" + nonce(4) + addr
		nonce := buf[8:12]
		resp := make([]byte, 12+len(addr.String()))
		copy(resp[:8], "SPINELOK")
		copy(resp[8:12], nonce)
		copy(resp[12:], addr.String())
		_, _ = pc.WriteTo(resp, addr)
	}
}

func main() {
	listenAddr := flag.String("listen", ":7443", "TLS control & relay listen address")
	udpAddr := flag.String("udp", ":7444", "UDP STUN probe listen address")
	dataDir := flag.String("data", "/var/lib/spinel-relay", "Data and TLS certificate directory")
	flag.Parse()

	srv, err := NewRelayServer(*dataDir)
	if err != nil {
		log.Fatalf("init relay server: %v", err)
	}

	cert, fp, err := ensureTLSCert(
		filepath.Join(*dataDir, "cert.pem"),
		filepath.Join(*dataDir, "key.pem"),
	)
	if err != nil {
		log.Fatalf("init TLS cert: %v", err)
	}

	udpPC, err := net.ListenPacket("udp", *udpAddr)
	if err != nil {
		log.Fatalf("listen UDP %s: %v", *udpAddr, err)
	}
	go runUDPStunLoop(udpPC)

	tlsCfg := &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS13,
	}
	ln, err := tls.Listen("tcp", *listenAddr, tlsCfg)
	if err != nil {
		log.Fatalf("listen TLS %s: %v", *listenAddr, err)
	}

	log.Printf("spinel-relay listening TLS=%s UDP=%s", *listenAddr, *udpAddr)
	log.Printf("Server TLS Fingerprint: %s", fp)
	_ = os.WriteFile(filepath.Join(*dataDir, "fingerprint.txt"), []byte(fp+"\n"), 0644)

	for {
		conn, err := ln.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		go srv.handleConn(conn)
	}
}
