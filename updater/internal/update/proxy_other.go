//go:build !windows

package update

import (
	"net/http"
	"net/url"
)

func systemProxy(r *http.Request) (*url.URL, error) { return http.ProxyFromEnvironment(r) }
