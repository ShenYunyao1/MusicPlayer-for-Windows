// Package update implements the signed, bounded, program-only update protocol.
package update

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"path"
	"regexp"
	"strconv"
	"strings"
)

const Product = "musicplayer-windows"
const Repository = "ShenYunyao1/MusicPlayer-for-Windows"
const MaxArchive = 256 << 20
const MaxExpanded = 512 << 20

type File struct {
	Path   string `json:"path"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}
type Asset struct {
	Name   string `json:"name"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}
type Manifest struct {
	Protocol         int             `json:"protocol"`
	Product          string          `json:"product"`
	Version          string          `json:"version"`
	Commit           string          `json:"commit"`
	Platform         string          `json:"platform"`
	MinimumUpdater   int             `json:"minimum_updater"`
	KeyID            string          `json:"key_id"`
	Notes            string          `json:"notes"`
	Assets           []Asset         `json:"assets"`
	Files            []File          `json:"files"`
	ExecutableSHA256 string          `json:"executable_sha256"`
	ServerBaseline   json.RawMessage `json:"server_baseline"`
}

var versionRE = regexp.MustCompile(`^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$`)
var hashRE = regexp.MustCompile(`^[a-f0-9]{64}$`)
var commitRE = regexp.MustCompile(`^[a-f0-9]{40}$`)

func version(v string) ([3]uint64, error) {
	var parts [3]uint64
	if !versionRE.MatchString(v) {
		return parts, errors.New("invalid stable version")
	}
	for i, s := range strings.Split(v, ".") {
		n, err := strconv.ParseUint(s, 10, 16)
		if err != nil {
			return parts, err
		}
		parts[i] = n
	}
	return parts, nil
}
func CompareVersion(a, b string) (int, error) {
	x, err := version(a)
	if err != nil {
		return 0, err
	}
	y, err := version(b)
	if err != nil {
		return 0, err
	}
	for i := range x {
		if x[i] > y[i] {
			return 1, nil
		}
		if x[i] < y[i] {
			return -1, nil
		}
	}
	return 0, nil
}
func digest(body []byte) string { h := sha256.Sum256(body); return hex.EncodeToString(h[:]) }

func validPath(p string) bool {
	if p == "" || len(p) > 230 || path.Clean(p) != p || strings.HasPrefix(p, "/") || strings.ContainsAny(p, `\:<>"|?*`) {
		return false
	}
	for _, c := range p {
		if c < 32 || c > 126 {
			return false
		}
	}
	segments := strings.Split(p, "/")
	for i, s := range segments {
		if s == "" || s == "." || s == ".." || strings.TrimRight(s, " .") != s {
			return false
		}
		if i < len(segments)-1 && strings.HasSuffix(strings.ToLower(s), ".exe") {
			return false
		}
		n := strings.ToUpper(strings.Split(s, ".")[0])
		if n == "CON" || n == "PRN" || n == "AUX" || n == "NUL" || (len(n) == 4 && (strings.HasPrefix(n, "COM") || strings.HasPrefix(n, "LPT")) && n[3] >= '0' && n[3] <= '9') {
			return false
		}
	}
	for _, n := range []string{"MusicPlayer2.exe", "MusicPlayerUpdater.exe", "bass.dll", "bass_fx.dll", "tag.dll", "SciLexer.dll", "LICENSE", "README.txt", "BUILD-INFO.txt", "PROGRAM-FILES.json", "global_cfg.ini"} {
		if p == n {
			return true
		}
	}
	return strings.Count(p, "/") >= 1 && (strings.HasPrefix(p, "language/") || strings.HasPrefix(p, "skins/") || strings.HasPrefix(p, "Encoder/"))
}
func (m Manifest) Validate() error {
	if m.Protocol != 1 || m.Product != Product || m.Platform != "windows-x64" || m.MinimumUpdater != 1 || m.KeyID != "client-v1" {
		return errors.New("unsupported update protocol, product, platform or signing key; download manually")
	}
	if _, err := version(m.Version); err != nil {
		return err
	}
	if !commitRE.MatchString(m.Commit) || len(m.Notes) > 64000 || len(m.Assets) != 1 || m.Assets[0].Name != "MusicPlayer2.zip" || m.Assets[0].Size <= 0 || m.Assets[0].Size > MaxArchive || !hashRE.MatchString(m.Assets[0].SHA256) {
		return errors.New("invalid release identity or asset")
	}
	if len(m.Files) == 0 || len(m.Files) > 4096 {
		return errors.New("invalid file count")
	}
	seen := map[string]bool{}
	total := int64(0)
	for _, f := range m.Files {
		p := strings.ToLower(f.Path)
		if !validPath(f.Path) || seen[p] || f.Size < 0 || f.Size > 100<<20 || !hashRE.MatchString(f.SHA256) {
			return fmt.Errorf("invalid or duplicate program file: %s", f.Path)
		}
		seen[p] = true
		total += f.Size
		if f.Path == "MusicPlayer2.exe" && f.SHA256 != m.ExecutableSHA256 {
			return errors.New("executable hash mismatch")
		}
	}
	for _, f := range m.Files {
		for parent := path.Dir(f.Path); parent != "."; parent = path.Dir(parent) {
			if seen[strings.ToLower(parent)] {
				return errors.New("file/directory collision")
			}
		}
	}
	if total > MaxExpanded || !seen["musicplayer2.exe"] || !seen["musicplayerupdater.exe"] {
		return errors.New("missing executable or excessive expanded size")
	}
	return nil
}
func ParseManifest(raw, sig []byte, pub ed25519.PublicKey) (Manifest, error) {
	var m Manifest
	if len(raw) > 2<<20 || len(pub) != ed25519.PublicKeySize || !ed25519.Verify(pub, raw, sig) {
		return m, errors.New("发行清单签名无效，已拒绝安装")
	}
	d := json.NewDecoder(bytes.NewReader(raw))
	d.DisallowUnknownFields()
	if err := d.Decode(&m); err != nil {
		return m, err
	}
	if err := d.Decode(new(any)); err != io.EOF {
		return m, errors.New("trailing manifest content")
	}
	return m, m.Validate()
}
func ReadPackage(body []byte, m Manifest) (map[string][]byte, error) {
	if err := m.Validate(); err != nil {
		return nil, err
	}
	if int64(len(body)) != m.Assets[0].Size || digest(body) != m.Assets[0].SHA256 {
		return nil, errors.New("下载包大小或 SHA-256 不符")
	}
	z, err := zip.NewReader(bytes.NewReader(body), int64(len(body)))
	if err != nil {
		return nil, err
	}
	if len(z.File) != len(m.Files) {
		return nil, errors.New("archive file count mismatch")
	}
	want := map[string]File{}
	for _, f := range m.Files {
		want[f.Path] = f
	}
	out := map[string][]byte{}
	for _, f := range z.File {
		n := strings.TrimPrefix(f.Name, "MusicPlayer2/")
		spec, ok := want[n]
		if !strings.HasPrefix(f.Name, "MusicPlayer2/") || !ok || out[n] != nil || !f.Mode().IsRegular() || f.UncompressedSize64 != uint64(spec.Size) {
			return nil, errors.New("unexpected archive path, link, duplicate or size")
		}
		r, err := f.Open()
		if err != nil {
			return nil, err
		}
		b, err := io.ReadAll(io.LimitReader(r, spec.Size+1))
		r.Close()
		if err != nil || int64(len(b)) != spec.Size || digest(b) != spec.SHA256 {
			return nil, fmt.Errorf("file hash mismatch: %s", n)
		}
		out[n] = b
	}
	return out, nil
}
