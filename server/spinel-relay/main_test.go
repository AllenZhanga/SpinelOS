package main

import (
	"bufio"
	"crypto/tls"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"net"
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
