package update

import (
	"context"
	"crypto/ed25519"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"
)

type HTTP struct {
	Client *http.Client
	Allow  func(*url.URL) bool
}

func allowedURL(u *url.URL) bool {
	if u.Scheme != "https" || u.User != nil || u.Port() != "" && u.Port() != "443" {
		return false
	}
	for _, h := range []string{"api.github.com", "github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"} {
		if strings.EqualFold(u.Hostname(), h) {
			return true
		}
	}
	return false
}
func NewHTTP() *HTTP {
	h := &HTTP{Allow: allowedURL}
	tr := http.DefaultTransport.(*http.Transport).Clone()
	tr.Proxy = systemProxy
	tr.ResponseHeaderTimeout = 30 * time.Second
	h.Client = &http.Client{Transport: tr, Timeout: 30 * time.Minute, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if len(via) >= 5 || !h.Allow(req.URL) {
			return errors.New("拒绝非官方 HTTPS 下载重定向")
		}
		return nil
	}}
	return h
}
func (h *HTTP) get(ctx context.Context, source string, limit int64, etag string, progress func(int64, int64)) ([]byte, string, int, error) {
	u, err := url.Parse(source)
	if err != nil || !h.Allow(u) {
		return nil, "", 0, errors.New("不允许的更新地址")
	}
	r, err := http.NewRequestWithContext(ctx, "GET", source, nil)
	if err != nil {
		return nil, "", 0, err
	}
	r.Header.Set("User-Agent", "MusicPlayer-Windows-Updater/1")
	r.Header.Set("Accept", "application/vnd.github+json")
	if etag != "" {
		r.Header.Set("If-None-Match", etag)
	}
	resp, err := h.Client.Do(r)
	if err != nil {
		return nil, "", 0, errors.New("无法连接 GitHub，请检查网络或系统代理")
	}
	defer resp.Body.Close()
	if resp.StatusCode == 304 {
		return nil, resp.Header.Get("ETag"), 304, nil
	}
	if resp.StatusCode != 200 {
		return nil, "", resp.StatusCode, fmt.Errorf("GitHub 返回 HTTP %d（限流时请稍后重试）", resp.StatusCode)
	}
	if resp.ContentLength > limit {
		return nil, "", 200, errors.New("下载内容超过允许大小")
	}
	var out []byte
	buf := make([]byte, 64<<10)
	for {
		n, e := resp.Body.Read(buf)
		if int64(len(out)+n) > limit {
			return nil, "", 200, errors.New("下载内容超过允许大小")
		}
		out = append(out, buf[:n]...)
		if progress != nil {
			progress(int64(len(out)), limit)
		}
		if e == io.EOF {
			break
		}
		if e != nil {
			return nil, "", 200, e
		}
	}
	return out, resp.Header.Get("ETag"), 200, nil
}

type releaseAsset struct {
	Name string `json:"name"`
	URL  string `json:"browser_download_url"`
}
type releaseInfo struct {
	Tag        string         `json:"tag_name"`
	Draft      bool           `json:"draft"`
	Prerelease bool           `json:"prerelease"`
	Assets     []releaseAsset `json:"assets"`
}
type CheckState struct {
	Checked int64           `json:"checked"`
	ETag    string          `json:"etag"`
	Body    json.RawMessage `json:"body"`
	Skipped string          `json:"skipped"`
}

func LoadCheckState(dir string) CheckState {
	var s CheckState
	b, _ := os.ReadFile(filepath.Join(dir, "check.json"))
	if len(b) < 2<<20 {
		_ = json.Unmarshal(b, &s)
	}
	return s
}
func SaveCheckState(dir string, s CheckState) error {
	b, err := json.Marshal(s)
	if err != nil {
		return err
	}
	return writeSync(filepath.Join(dir, "check.json"), b)
}

// Discover authenticates the manifest before its contents are shown or downloaded.
func (h *HTTP) Discover(ctx context.Context, pub ed25519.PublicKey, state *CheckState, endpoint string) (Manifest, []byte, []byte, string, error) {
	var empty Manifest
	body, etag, status, err := h.get(ctx, endpoint, 1<<20, state.ETag, nil)
	if err != nil {
		return empty, nil, nil, "", err
	}
	if status == 304 {
		body = state.Body
	}
	var r releaseInfo
	if err = json.Unmarshal(body, &r); err != nil {
		return empty, nil, nil, "", err
	}
	if r.Draft || r.Prerelease || !strings.HasPrefix(r.Tag, "v") {
		return empty, nil, nil, "", errors.New("不是正式发行版本")
	}
	urls := map[string]string{}
	for _, a := range r.Assets {
		want := "https://github.com/" + Repository + "/releases/download/" + r.Tag + "/" + a.Name
		if a.URL != want || urls[a.Name] != "" {
			return empty, nil, nil, "", errors.New("发行资产来源或名称异常")
		}
		urls[a.Name] = a.URL
	}
	if len(urls) != 4 || urls["MusicPlayer2.zip"] == "" || urls["MusicPlayer2.zip.sha256"] == "" || urls["client-release-manifest.json"] == "" || urls["client-release-manifest.json.sig"] == "" {
		return empty, nil, nil, "", errors.New("该版本没有完整的签名更新包，请手动下载升级")
	}
	raw, _, _, err := h.get(ctx, urls["client-release-manifest.json"], 2<<20, "", nil)
	if err != nil {
		return empty, nil, nil, "", err
	}
	sig, _, _, err := h.get(ctx, urls["client-release-manifest.json.sig"], 64, "", nil)
	if err != nil {
		return empty, nil, nil, "", err
	}
	m, err := ParseManifest(raw, sig, pub)
	if err != nil {
		return empty, nil, nil, "", err
	}
	if r.Tag != "v"+m.Version {
		return empty, nil, nil, "", errors.New("版本标签与签名清单不符")
	}
	state.Checked = time.Now().Unix()
	if etag != "" {
		state.ETag = etag
	}
	state.Body = body
	return m, raw, sig, urls["MusicPlayer2.zip"], nil
}
func (h *HTTP) Download(ctx context.Context, source string, m Manifest, progress func(int64, int64)) ([]byte, error) {
	b, _, _, err := h.get(ctx, source, m.Assets[0].Size, "", progress)
	if err != nil {
		return nil, err
	}
	if int64(len(b)) != m.Assets[0].Size || digest(b) != m.Assets[0].SHA256 {
		return nil, errors.New("下载包大小或 SHA-256 不符")
	}
	return b, nil
}
