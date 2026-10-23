package update

import (
	"context"
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"
)

func TestHTTPBoundaries(t *testing.T) {
	for _, raw := range []string{"http://github.com/x", "https://evil.example/x", "https://github.com:444/x", "https://user@github.com/x"} {
		u, _ := url.Parse(raw)
		if allowedURL(u) {
			t.Fatal("unsafe URL", raw)
		}
	}
	s := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-API-Key") != "" || r.Header.Get("Authorization") != "" || r.Header.Get("Cookie") != "" {
			t.Error("credential leaked")
		}
		switch r.URL.Path {
		case "/large":
			w.Write([]byte("123456"))
		case "/limited":
			w.WriteHeader(429)
		case "/cache":
			if r.Header.Get("If-None-Match") == "fixture" {
				w.WriteHeader(304)
			} else {
				w.Header().Set("ETag", "fixture")
				w.Write([]byte("ok"))
			}
		case "/redirect":
			http.Redirect(w, r, "https://evil.example/file", 302)
		default:
			w.Write([]byte("ok"))
		}
	}))
	defer s.Close()
	h := NewHTTP()
	h.Client.Transport = s.Client().Transport
	h.Allow = func(u *url.URL) bool { return u.Scheme == "https" && u.Host == s.Listener.Addr().String() }
	if _, _, _, e := h.get(context.Background(), s.URL+"/large", 4, "", nil); e == nil {
		t.Fatal("oversize accepted")
	}
	if _, _, status, e := h.get(context.Background(), s.URL+"/limited", 10, "", nil); e == nil || status != 429 {
		t.Fatal("limit ignored")
	}
	if _, _, status, e := h.get(context.Background(), s.URL+"/cache", 10, "fixture", nil); e != nil || status != 304 {
		t.Fatal("ETag ignored")
	}
	if _, _, _, e := h.get(context.Background(), s.URL+"/redirect", 10, "", nil); e == nil {
		t.Fatal("redirect escaped")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, _, _, e := h.get(ctx, s.URL, 10, "", nil); e == nil {
		t.Fatal("cancel ignored")
	}
	// A normal client must not trust the fixture's self-signed certificate.
	h = NewHTTP()
	h.Allow = func(*url.URL) bool { return true }
	if _, _, _, e := h.get(context.Background(), s.URL, 10, "", nil); e == nil {
		t.Fatal("TLS validation bypassed")
	}
}
