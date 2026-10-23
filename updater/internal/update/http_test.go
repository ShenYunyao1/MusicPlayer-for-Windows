package update

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func TestDiscoverSignedReleaseAndCache(t *testing.T) {
	m, raw, sig, pub, body := fixture(t)
	s := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, ".sig") {
			w.Write(sig)
			return
		}
		if strings.HasSuffix(r.URL.Path, ".json") {
			w.Write(raw)
			return
		}
		if strings.HasSuffix(r.URL.Path, ".zip") {
			w.Write(body)
			return
		}
		if r.Header.Get("If-None-Match") == "fixture" {
			w.WriteHeader(304)
			return
		}
		w.Header().Set("ETag", "fixture")
		assets := []releaseAsset{}
		for _, name := range []string{"MusicPlayer2.zip", "MusicPlayer2.zip.sha256", "client-release-manifest.json", "client-release-manifest.json.sig"} {
			assets = append(assets, releaseAsset{Name: name, URL: "https://github.com/" + Repository + "/releases/download/v" + m.Version + "/" + name})
		}
		json.NewEncoder(w).Encode(releaseInfo{Tag: "v" + m.Version, Assets: assets})
	}))
	defer s.Close()
	h := NewHTTP()
	host, _ := url.Parse(s.URL)
	tr := s.Client().Transport
	h.Allow = func(u *url.URL) bool { return u.Host == host.Host || allowedURL(u) }
	h.Client.Transport = roundTripFunc(func(r *http.Request) (*http.Response, error) {
		copyR := r.Clone(r.Context())
		copyR.URL.Host = host.Host
		copyR.URL.Scheme = host.Scheme
		return tr.RoundTrip(copyR)
	})
	state := CheckState{}
	for i := 0; i < 2; i++ {
		got, _, _, link, e := h.Discover(context.Background(), pub, &state, s.URL+"/latest")
		if e != nil || got.Version != m.Version {
			t.Fatal(e)
		}
		if _, e = h.Download(context.Background(), link, got, nil); e != nil {
			t.Fatal(e)
		}
	}
	if state.ETag != "fixture" || state.Checked == 0 {
		t.Fatal("cache not recorded")
	}
	sig[0] ^= 1
	if _, _, _, _, e := h.Discover(context.Background(), pub, &state, s.URL+"/latest"); e == nil {
		t.Fatal("accepted corrupted signature")
	}
}

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
