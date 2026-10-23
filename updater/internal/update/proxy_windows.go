package update

import (
	"net/http"
	"net/url"
	"strings"
	"syscall"
	"unsafe"
)

// Respect explicit environment proxies, then the current user's static Windows proxy.
// PAC-only configurations fail normally rather than disabling TLS or running scripts.
func systemProxy(r *http.Request) (*url.URL, error) {
	if r.URL.Hostname() == "localhost" || r.URL.Hostname() == "127.0.0.1" || r.URL.Hostname() == "::1" {
		return nil, nil
	}
	if p, e := http.ProxyFromEnvironment(r); p != nil || e != nil {
		return p, e
	}
	type config struct {
		AutoDetect         int32
		PAC, Proxy, Bypass *uint16
	}
	var c config
	ok, _, _ := syscall.NewLazyDLL("winhttp.dll").NewProc("WinHttpGetIEProxyConfigForCurrentUser").Call(uintptr(unsafe.Pointer(&c)))
	if ok == 0 {
		return nil, nil
	}
	free := syscall.NewLazyDLL("kernel32.dll").NewProc("GlobalFree")
	defer func() {
		for _, p := range []*uint16{c.PAC, c.Proxy, c.Bypass} {
			if p != nil {
				free.Call(uintptr(unsafe.Pointer(p)))
			}
		}
	}()
	if c.Proxy == nil {
		return nil, nil
	}
	s := syscall.UTF16ToString(unsafe.Slice(c.Proxy, 32768))
	for _, entry := range strings.Split(s, ";") {
		pair := strings.SplitN(entry, "=", 2)
		if len(pair) == 2 {
			if pair[0] != "https" {
				continue
			}
			entry = pair[1]
		}
		if !strings.Contains(entry, "://") {
			entry = "http://" + entry
		}
		return url.Parse(entry)
	}
	return nil, nil
}
