package update

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

const TransactionDir = ".musicplayer-update"

type Operation struct {
	Path    string `json:"path"`
	HadOld  bool   `json:"had_old"`
	OldHash string `json:"old_hash"`
}
type Transaction struct {
	Root       string      `json:"root"`
	State      string      `json:"state"`
	Version    string      `json:"version"`
	Commit     string      `json:"commit"`
	Operations []Operation `json:"operations"`
}

func safeRoot(root string) (string, error) {
	r, err := filepath.Abs(root)
	if err != nil {
		return "", err
	}
	if strings.HasPrefix(r, `\\`) || filepath.Dir(r) == r {
		return "", errors.New("不支持网络目录或卷根目录")
	}
	return r, noLinks(r)
}
func noLinks(p string) error {
	for s := p; ; s = filepath.Dir(s) {
		info, err := os.Lstat(s)
		if err != nil && !os.IsNotExist(err) {
			return err
		}
		if err == nil && (info.Mode()&os.ModeSymlink != 0 || isReparse(s)) {
			return fmt.Errorf("更新路径包含链接或重解析点: %s", s)
		}
		if filepath.Dir(s) == s {
			break
		}
	}
	return nil
}
func writeSync(p string, b []byte) error {
	if err := noLinks(p); err != nil {
		return err
	}
	if err := noLinks(p + ".writing"); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(p), 0700); err != nil {
		return err
	}
	f, err := os.OpenFile(p+".writing", os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	_, err = f.Write(b)
	if err == nil {
		err = f.Sync()
	}
	cerr := f.Close()
	if err != nil {
		return err
	}
	if cerr != nil {
		return cerr
	}
	return replaceFile(p+".writing", p)
}
func (t *Transaction) dir() string { return filepath.Join(t.Root, TransactionDir) }
func (t *Transaction) Save() error {
	b, err := json.MarshalIndent(t, "", "  ")
	if err != nil {
		return err
	}
	return writeSync(filepath.Join(t.dir(), "journal.json"), b)
}
func editable(p string) bool {
	return strings.HasPrefix(p, "skins/") || strings.HasPrefix(p, "language/") || strings.HasPrefix(p, "Encoder/")
}

// Prepare never mutates program files. The durable journal is written before Apply.
func Prepare(root string, m Manifest, files map[string][]byte) (*Transaction, error) {
	r, err := safeRoot(root)
	if err != nil {
		return nil, err
	}
	if err = m.Validate(); err != nil {
		return nil, err
	}
	if err = noLinks(filepath.Join(r, TransactionDir)); err != nil {
		return nil, err
	}
	if _, err = os.Stat(filepath.Join(r, TransactionDir)); !os.IsNotExist(err) {
		return nil, errors.New("存在上次更新事务，请先恢复或清理已完成事务")
	}
	baseline := map[string]string{}
	if b, e := os.ReadFile(filepath.Join(r, "PROGRAM-FILES.json")); e == nil {
		_ = json.Unmarshal(b, &baseline)
	}
	t := &Transaction{Root: r, State: "preparing", Version: m.Version, Commit: m.Commit}
	for _, spec := range m.Files {
		if spec.Path == "global_cfg.ini" {
			continue
		}
		if digest(files[spec.Path]) != spec.SHA256 {
			return nil, errors.New("staged content mismatch")
		}
		target := filepath.Join(r, filepath.FromSlash(spec.Path))
		if err = noLinks(target); err != nil {
			return nil, err
		}
		b, err := os.ReadFile(target)
		exists := err == nil
		if err != nil && !os.IsNotExist(err) {
			return nil, err
		}
		if editable(spec.Path) && exists && digest(b) != spec.SHA256 && baseline[spec.Path] != digest(b) {
			return nil, fmt.Errorf("资源有本地修改或缺少基线，请先备份并恢复原始文件: %s", spec.Path)
		}
		t.Operations = append(t.Operations, Operation{Path: spec.Path, HadOld: exists, OldHash: digest(b)})
	}
	sort.Slice(t.Operations, func(i, j int) bool { return t.Operations[i].Path < t.Operations[j].Path })
	if err = os.Mkdir(t.dir(), 0700); err != nil {
		return nil, err
	}
	if err = t.Save(); err != nil {
		return nil, err
	}
	for _, op := range t.Operations {
		if err = writeSync(filepath.Join(t.dir(), "payload", filepath.FromSlash(op.Path)), files[op.Path]); err != nil {
			return nil, err
		}
		if op.HadOld {
			b, err := os.ReadFile(filepath.Join(r, filepath.FromSlash(op.Path)))
			if err != nil {
				return nil, err
			}
			if digest(b) != op.OldHash {
				return nil, errors.New("program changed during preparation")
			}
			if err = writeSync(filepath.Join(t.dir(), "backup", filepath.FromSlash(op.Path)), b); err != nil {
				return nil, err
			}
		}
	}
	t.State = "prepared"
	return t, t.Save()
}
func LoadTransaction(root string) (*Transaction, error) {
	r, err := safeRoot(root)
	if err != nil {
		return nil, err
	}
	p := filepath.Join(r, TransactionDir, "journal.json")
	if err = noLinks(p); err != nil {
		return nil, err
	}
	b, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	if len(b) > 2<<20 {
		return nil, errors.New("journal too large")
	}
	var t Transaction
	if err = json.Unmarshal(b, &t); err != nil {
		return nil, err
	}
	if !strings.EqualFold(t.Root, r) || len(t.Operations) > 4096 {
		return nil, errors.New("invalid recovery root")
	}
	seen := map[string]bool{}
	for _, op := range t.Operations {
		if !validPath(op.Path) || op.Path == "global_cfg.ini" || seen[strings.ToLower(op.Path)] {
			return nil, errors.New("invalid recovery path")
		}
		seen[strings.ToLower(op.Path)] = true
		if err = noLinks(filepath.Join(r, filepath.FromSlash(op.Path))); err != nil {
			return nil, err
		}
	}
	return &t, nil
}
func (t *Transaction) Apply() error {
	t.State = "applying"
	if err := t.Save(); err != nil {
		return err
	}
	for _, op := range t.Operations {
		p := filepath.Join(t.Root, filepath.FromSlash(op.Path))
		b, err := os.ReadFile(filepath.Join(t.dir(), "payload", filepath.FromSlash(op.Path)))
		if err != nil {
			return err
		}
		if err = writeSync(p, b); err != nil {
			return err
		}
	}
	t.State = "awaiting_start"
	return t.Save()
}
func (t *Transaction) Rollback() error {
	t.State = "rolling_back"
	if err := t.Save(); err != nil {
		return err
	}
	for _, op := range t.Operations {
		p := filepath.Join(t.Root, filepath.FromSlash(op.Path))
		if err := noLinks(p); err != nil {
			return err
		}
		if op.HadOld {
			b, err := os.ReadFile(filepath.Join(t.dir(), "backup", filepath.FromSlash(op.Path)))
			if err != nil {
				return err
			}
			if digest(b) != op.OldHash {
				return errors.New("backup hash mismatch")
			}
			if current, e := os.ReadFile(p); e == nil && digest(current) == op.OldHash {
				continue
			}
			if err = writeSync(p, b); err != nil {
				return err
			}
		} else if err := os.Remove(p); err != nil && !os.IsNotExist(err) {
			return err
		}
	}
	t.State = "rolled_back"
	return t.Save()
}
func (t *Transaction) Complete() error { t.State = "complete"; return t.Save() }

// Cleanup is restricted to a validated finished transaction inside this installation.
func Cleanup(root string) error {
	t, err := LoadTransaction(root)
	if os.IsNotExist(err) {
		// A crash between mkdir and the first journal rename cannot have touched
		// any program file. Only discard an empty directory/partial journal.
		r, e := safeRoot(root)
		if e != nil {
			return e
		}
		dir := filepath.Join(r, TransactionDir)
		if e = noLinks(dir); e != nil {
			return e
		}
		entries, e := os.ReadDir(dir)
		if os.IsNotExist(e) {
			return nil
		}
		if e != nil {
			return e
		}
		for _, entry := range entries {
			if entry.Name() != "journal.json.writing" {
				return errors.New("暂存目录缺少事务记录，请保留该目录并手动检查")
			}
		}
		if e = os.Remove(filepath.Join(dir, "journal.json.writing")); e != nil && !os.IsNotExist(e) {
			return e
		}
		return os.Remove(dir)
	}
	if err != nil {
		return err
	}
	if t.State != "complete" && t.State != "rolled_back" && t.State != "prepared" && t.State != "preparing" {
		return errors.New("上次更新未完成，请运行更新器恢复")
	}
	if err := noLinks(t.dir()); err != nil {
		return err
	}
	return os.RemoveAll(t.dir())
}
