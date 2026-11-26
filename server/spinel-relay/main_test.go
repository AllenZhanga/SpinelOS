package main

import (
	"bufio"
	"crypto/tls"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"net"
	"strings"
	"testing"
)

func sendCtrl(t *testing.T, bw *bufio.Writer, v any) {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatal(err)
	}
	var hdr [5]byte
	hdr[0] = FrameControl
	binary.BigEndian.PutUint32(hdr[1:5], uint32(len(b)))
	if _, err := bw.Write(hdr[:]); err != nil {
		t.Fatal(err)
	}
	if _, err := bw.Write(b); err != nil {
		t.Fatal(err)
	}
	if err := bw.Flush(); err != nil {
		t.Fatal(err)
	}
}

func readResp(t *testing.T, br *bufio.Reader) map[string]any {
	t.Helper()
	ft, pl, err := readFrame(br)
	if err != nil {
		t.Fatal(err)
	}
	if ft != FrameControl {
		t.Fatalf("expected control frame, got %d", ft)
	}
	var m map[string]any
	if err := json.Unmarshal(pl, &m); err != nil {
		t.Fatal(err)
	}
	return m
}

func TestRelayAccountPairAndForward(t *testing.T) {
	dir := t.TempDir()
	srv, err := NewRelayServer(dir)
	if err != nil {
		t.Fatal(err)
	}
	cert, fp, err := ensureTLSCert(dir+"/cert.pem", dir+"/key.pem")
	if err != nil {
		t.Fatal(err)
	}
	if len(fp) < 16 {
		t.Fatalf("unexpected fingerprint: %s", fp)
	}

	ln, err := tls.Listen("tcp", "127.0.0.1:0", &tls.Config{Certificates: []tls.Certificate{cert}})
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go srv.handleConn(c)
		}
	}()

	dial := func() (*tls.Conn, *bufio.Reader, *bufio.Writer) {
		c, err := tls.Dial("tcp", ln.Addr().String(), &tls.Config{InsecureSkipVerify: true})
		if err != nil {
			t.Fatal(err)
		}
		return c, bufio.NewReader(c), bufio.NewWriter(c)
	}

	// Device A registers account + generates pair code
	c1, br1, bw1 := dial()
	defer c1.Close()
	sendCtrl(t, bw1, ControlMsg{
		Type:        "register",
		ReqID:       "1",
		Account:     "allen",
		Password:    "secret123",
		DeviceID:    "dev-desktop",
		DeviceName:  "HomeDesktop",
		AllowRemote: true,
	})
	r1 := readResp(t, br1)
	if r1["ok"] != true {
		t.Fatalf("register failed: %+v", r1)
	}
	tok1, _ := r1["token"].(string)

	sendCtrl(t, bw1, ControlMsg{Type: "pair_create", ReqID: "2"})
	r2 := readResp(t, br1)
	code, _ := r2["pair_code"].(string)
	if len(code) != 6 {
		t.Fatalf("invalid pair code: %v", code)
	}

	// Device B claims pair code
	c2, br2, bw2 := dial()
	defer c2.Close()
	sendCtrl(t, bw2, ControlMsg{
		Type:       "pair_claim",
		ReqID:      "3",
		PairCode:   code,
		DeviceID:   "dev-laptop",
		DeviceName: "TravelLaptop",
	})
	r3 := readResp(t, br2)
	if r3["ok"] != true || r3["account"] != "allen" {
		t.Fatalf("pair claim failed: %+v", r3)
	}
	tok2, _ := r3["token"].(string)

	// Open binary relay tunnel between two auxiliary TLS streams
	sidBytes := make([]byte, 16)
	for i := range sidBytes {
		sidBytes[i] = byte(i + 1)
	}
	sidHex := hex.EncodeToString(sidBytes)

	t1, tbr1, tbw1 := dial()
	defer t1.Close()
	sendCtrl(t, tbw1, ControlMsg{Type: "relay_join", ReqID: "j1", SessionID: sidHex, DeviceID: "dev-desktop", Token: tok1})
	if r := readResp(t, tbr1); r["ok"] != true {
		t.Fatalf("join1 failed: %+v", r)
	}

	t2, tbr2, tbw2 := dial()
	defer t2.Close()
	sendCtrl(t, tbw2, ControlMsg{Type: "relay_join", ReqID: "j2", SessionID: sidHex, DeviceID: "dev-laptop", Token: tok2, TargetDeviceID: "dev-desktop"})
	if r := readResp(t, tbr2); r["ok"] != true || r["paired"] != true {
		t.Fatalf("join2 failed: %+v", r)
	}

	// Send binary packet t2 -> t1
	msg := append(append([]byte{}, sidBytes...), []byte("hello-e2ee")...)
	var hdr [5]byte
	hdr[0] = FrameRelay
	binary.BigEndian.PutUint32(hdr[1:5], uint32(len(msg)))
	if _, err := tbw2.Write(hdr[:]); err != nil {
		t.Fatal(err)
	}
	if _, err := tbw2.Write(msg); err != nil {
		t.Fatal(err)
	}
	_ = tbw2.Flush()

	ft, got, err := readFrame(tbr1)
	if err != nil || ft != FrameRelay || string(got[16:]) != "hello-e2ee" {
		t.Fatalf("relay forward mismatch: err=%v ft=%d got=%q", err, ft, string(got))
	}
}

func TestUDPStunLoop(t *testing.T) {
	pc, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer pc.Close()
	go runUDPStunLoop(pc)

	c, err := net.Dial("udp", pc.LocalAddr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	_, _ = c.Write([]byte("SPINELST1234"))
	buf := make([]byte, 256)
	n, err := c.Read(buf)
	if err != nil {
		t.Fatal(err)
	}
	if string(buf[:12]) != "SPINELOK1234" || n <= 12 {
		t.Fatalf("unexpected stun reply: %q", string(buf[:n]))
	}
}

func TestEphemeralRpcPreservesHostOnline(t *testing.T) {
	srv, err := NewRelayServer(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	dial := func() (net.Conn, *bufio.Reader, *bufio.Writer) {
		a, b := net.Pipe()
		go srv.handleConn(a)
		return b, bufio.NewReader(b), bufio.NewWriter(b)
	}

	// Hostd registers & heartbeats on persistent connection
	hc, hbr, hbw := dial()
	defer hc.Close()
	sendCtrl(t, hbw, ControlMsg{
		Type: "register", ReqID: "r1", Account: "alice", Password: "secret123",
		DeviceID: "spinelos", DeviceName: "spinelos", AllowRemote: true, Role: "hostd",
	})
	resp := readResp(t, hbr)
	if resp["ok"] != true {
		t.Fatalf("host register failed: %+v", resp)
	}
	tok, _ := resp["token"].(string)
	sendCtrl(t, hbw, ControlMsg{Type: "heartbeat", ReqID: "hb", DeviceID: "spinelos", AllowRemote: true})
	_ = readResp(t, hbr)

	// Same device opens short-lived ephemeral pair_create RPC and closes
	rc, rbr, rbw := dial()
	sendCtrl(t, rbw, ControlMsg{
		Type: "login", ReqID: "p1", Account: "alice", Token: tok,
		DeviceID: "spinelos", AllowRemote: true, Ephemeral: true,
	})
	if r := readResp(t, rbr); r["ok"] != true {
		t.Fatalf("ephemeral login failed: %+v", r)
	}
	sendCtrl(t, rbw, ControlMsg{Type: "pair_create", ReqID: "p2"})
	pr := readResp(t, rbr)
	code, _ := pr["pair_code"].(string)
	_ = rc.Close()

	// Initiator claims pair code and lists devices -> spinelos MUST still be online: true
	mc, mbr, mbw := dial()
	defer mc.Close()
	sendCtrl(t, mbw, ControlMsg{
		Type: "pair_claim", ReqID: "c1", PairCode: code,
		DeviceID: "mac-1", DeviceName: "Mac", Ephemeral: true,
	})
	cr := readResp(t, mbr)
	if cr["ok"] != true {
		t.Fatalf("claim failed: %+v", cr)
	}
	devs, _ := cr["devices"].([]any)
	foundOnline := false
	for _, item := range devs {
		m, _ := item.(map[string]any)
		if m["device_id"] == "spinelos" && m["online"] == true {
			foundOnline = true
		}
	}
	if !foundOnline {
		t.Fatalf("expected spinelos online=true after ephemeral RPC close, got %+v", devs)
	}

	// Verify punch_offer -> punch_answer works even when initiator is ephemeral
	sid := "0123456789abcdef0123456789abcdef"
	sendCtrl(t, mbw, ControlMsg{
		Type: "punch_offer", ReqID: "o1", TargetDeviceID: "spinelos", SessionID: sid, EphemeralPub: "aa",
	})
	_, offerRaw, err := readFrame(hbr)
	if err != nil || !strings.Contains(string(offerRaw), `"type":"punch_offer"`) {
		t.Fatalf("host did not receive punch_offer: err=%v raw=%q", err, string(offerRaw))
	}
	if or := readResp(t, mbr); or["ok"] != true {
		t.Fatalf("punch_offer failed: %+v", or)
	}
	sendCtrl(t, hbw, ControlMsg{
		Type: "punch_answer", ReqID: "a1", TargetDeviceID: "mac-1", SessionID: sid, EphemeralPub: "bb",
	})
	_, ansRaw, err := readFrame(mbr)
	if err != nil || !strings.Contains(string(ansRaw), `"type":"punch_answer"`) {
		t.Fatalf("ephemeral initiator did not receive punch_answer: err=%v raw=%q", err, string(ansRaw))
	}
	if ar := readResp(t, hbr); ar["ok"] != true {
		t.Fatalf("punch_answer failed: %+v", ar)
	}
}
