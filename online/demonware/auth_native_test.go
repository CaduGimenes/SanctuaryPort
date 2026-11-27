// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"encoding/json"
	"os"
	"os/exec"
	"strings"
	"testing"
)

func TestPrivateAuthResponseAcceptedByRuntimeVerifier(t *testing.T) {
	fixture := os.Getenv("D3_NATIVE_TRANSPORT_TEST")
	if fixture == "" {
		t.Skip("native fixture not selected")
	}
	s, _, _ := authFixture(t)
	reply := authCall(s, authBody(t, 46, 5464, testToken))
	if reply.Code != 200 {
		t.Fatal(reply.Code, reply.Body.String())
	}
	verify := func(body, signature string, valid bool) {
		t.Helper()
		input, _ := json.Marshal(map[string]string{"body": body, "signature": signature, "bearer": testToken})
		cmd := exec.Command(fixture, "--verify-auth-response")
		cmd.Stdin = bytes.NewReader(input)
		out, err := cmd.Output()
		if err != nil || (strings.TrimSpace(string(out)) == "true") != valid {
			t.Fatalf("runtime signature verification: %q, %v; want valid=%v", out, err, valid)
		}
	}
	verify(reply.Body.String(), reply.Header().Get("X-D3-Signature"), true)
	verify(reply.Body.String()+" ", reply.Header().Get("X-D3-Signature"), false)
	verify(reply.Body.String(), strings.Repeat("0", 64), false)
}
