package update

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func fixture(t *testing.T) (Manifest, []byte, []byte, ed25519.PublicKey, []byte) {
	t.Helper()
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	var b bytes.Buffer
	z := zip.NewWriter(&b)
	m := Manifest{Protocol: 1, Product: Product, Platform: "windows-x64", Version: "0.2.1", Commit: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", MinimumUpdater: 1, KeyID: "client-v1"}
	for _, name := range []string{"MusicPlayer2.exe", "MusicPlayerUpdater.exe", "global_cfg.ini", "language/test.ini"} {
		body := []byte("new " + name)
		w, _ := z.Create("MusicPlayer2/" + name)
		w.Write(body)
		h := sha256.Sum256(body)
		m.Files = append(m.Files, File{Path: name, Size: int64(len(body)), SHA256: hex.EncodeToString(h[:])})
	}
	z.Close()
	h := sha256.Sum256(b.Bytes())
	m.Assets = []Asset{{Name: "MusicPlayer2.zip", Size: int64(b.Len()), SHA256: hex.EncodeToString(h[:])}}
	m.ExecutableSHA256 = m.Files[0].SHA256
	raw, _ := json.Marshal(m)
	return m, raw, ed25519.Sign(priv, raw), pub, b.Bytes()
}

func TestSignedManifest(t *testing.T) {
	m, raw, sig, pub, _ := fixture(t)
	if _, err := ParseManifest(raw, sig, pub); err != nil {
		t.Fatal(err)
	}
	sig[0] ^= 1
	if _, err := ParseManifest(raw, sig, pub); err == nil {
		t.Fatal("accepted bad signature")
	}
	for _, mutate := range []func(*Manifest){
		func(m *Manifest) { m.Protocol = 2 }, func(m *Manifest) { m.Platform = "linux" },
		func(m *Manifest) { m.Version = "0.02.1" }, func(m *Manifest) { m.Commit = "main" },
		func(m *Manifest) { m.MinimumUpdater = 2 }, func(m *Manifest) { m.Product = "server" },
		func(m *Manifest) { m.Files[0].Path = "../secret" },
	} {
		copyM := m
		copyM.Files = append([]File(nil), m.Files...)
		mutate(&copyM)
		if err := copyM.Validate(); err == nil {
			t.Fatalf("accepted invalid manifest: %+v", copyM)
		}
	}
}

func TestVersionAndPaths(t *testing.T) {
	if n, err := CompareVersion("0.10.0", "0.9.0"); err != nil || n != 1 {
		t.Fatal(n, err)
	}
	for _, p := range []string{"../x", "/x", "C:/x", `language\x`, "language/../x", "skins/CON.txt", "skins/a:b", "skins/a.", "config.ini", "remote-library.json", "Encoder/x.exe/x", "skins//a"} {
		if validPath(p) {
			t.Fatal("accepted", p)
		}
	}
}

func TestArchiveAndTampering(t *testing.T) {
	m, _, _, _, body := fixture(t)
	if _, err := ReadPackage(body, m); err != nil {
		t.Fatal(err)
	}
	body[len(body)/2] ^= 1
	if _, err := ReadPackage(body, m); err == nil {
		t.Fatal("accepted modified ZIP")
	}
}

func TestTransactionRollbackAndData(t *testing.T) {
	m, _, _, _, body := fixture(t)
	files, _ := ReadPackage(body, m)
	root := t.TempDir()
	os.WriteFile(filepath.Join(root, "MusicPlayer2.exe"), []byte("old"), 0600)
	os.WriteFile(filepath.Join(root, "global_cfg.ini"), []byte("personal"), 0600)
	os.WriteFile(filepath.Join(root, "remote-library.json"), []byte("protected key"), 0600)
	// An absent editable resource is safe to add. Existing unknown edits are not.
	tx, err := Prepare(root, m, files)
	if err != nil {
		t.Fatal(err)
	}
	if err = tx.Apply(); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(filepath.Join(root, "global_cfg.ini")); string(b) != "personal" {
		t.Fatal("overwrote global config")
	}
	if err = tx.Rollback(); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(filepath.Join(root, "MusicPlayer2.exe")); string(b) != "old" {
		t.Fatal("rollback failed")
	}
	if _, err = os.Stat(filepath.Join(root, "MusicPlayerUpdater.exe")); !os.IsNotExist(err) {
		t.Fatal("new file survived rollback")
	}
	if b, _ := os.ReadFile(filepath.Join(root, "remote-library.json")); string(b) != "protected key" {
		t.Fatal("touched data")
	}
}

func TestResourceConflict(t *testing.T) {
	m, _, _, _, body := fixture(t)
	files, _ := ReadPackage(body, m)
	root := t.TempDir()
	os.MkdirAll(filepath.Join(root, "language"), 0700)
	os.WriteFile(filepath.Join(root, "language/test.ini"), []byte("custom"), 0600)
	if _, err := Prepare(root, m, files); err == nil {
		t.Fatal("overwrote untracked resource")
	}
}

func TestInterruptedRecoveryAndMissingGlobalConfig(t *testing.T) {
	m, _, _, _, body := fixture(t)
	files, _ := ReadPackage(body, m)
	for _, state := range []string{"applying", "awaiting_start", "rolling_back"} {
		t.Run(state, func(t *testing.T) {
			root := t.TempDir()
			os.WriteFile(filepath.Join(root, "MusicPlayer2.exe"), []byte("old"), 0600)
			tx, e := Prepare(root, m, files)
			if e != nil {
				t.Fatal(e)
			}
			tx.State = state
			if e = tx.Save(); e != nil {
				t.Fatal(e)
			}
			os.WriteFile(filepath.Join(root, "MusicPlayer2.exe"), []byte("partial new"), 0600)
			if e = Cleanup(root); e == nil {
				t.Fatal("discarded interrupted transaction")
			}
			tx, e = LoadTransaction(root)
			if e != nil {
				t.Fatal(e)
			}
			if e = tx.Rollback(); e != nil {
				t.Fatal(e)
			}
			if e = tx.Rollback(); e != nil {
				t.Fatal("repeated recovery", e)
			}
			if b, _ := os.ReadFile(filepath.Join(root, "MusicPlayer2.exe")); string(b) != "old" {
				t.Fatal("lost old program")
			}
			if _, e = os.Stat(filepath.Join(root, "global_cfg.ini")); !os.IsNotExist(e) {
				t.Fatal("introduced portable configuration")
			}
		})
	}
}

func TestRecoveryRejectsEscapesAndBadBackup(t *testing.T) {
	m, _, _, _, b := fixture(t)
	files, _ := ReadPackage(b, m)
	root := t.TempDir()
	os.WriteFile(filepath.Join(root, "MusicPlayer2.exe"), []byte("old"), 0600)
	tx, e := Prepare(root, m, files)
	if e != nil {
		t.Fatal(e)
	}
	os.WriteFile(filepath.Join(root, TransactionDir, "backup", "MusicPlayer2.exe"), []byte("corrupt"), 0600)
	if e = tx.Rollback(); e == nil {
		t.Fatal("accepted corrupt backup")
	}
	tx.Operations[0].Path = "../outside"
	tx.Save()
	if _, e = LoadTransaction(root); e == nil {
		t.Fatal("accepted escaped journal")
	}
}

func TestRestoreEveryPartialApply(t *testing.T) {
	m, _, _, _, b := fixture(t)
	files, _ := ReadPackage(b, m)
	for count := 0; count < 4; count++ {
		root := t.TempDir()
		os.WriteFile(filepath.Join(root, "MusicPlayer2.exe"), []byte("old"), 0600)
		tx, e := Prepare(root, m, files)
		if e != nil {
			t.Fatal(e)
		}
		tx.State = "applying"
		tx.Save()
		for i, op := range tx.Operations {
			if i >= count {
				break
			}
			if e = writeSync(filepath.Join(root, filepath.FromSlash(op.Path)), files[op.Path]); e != nil {
				t.Fatal(e)
			}
		}
		recovered, e := LoadTransaction(root)
		if e != nil {
			t.Fatal(e)
		}
		if e = recovered.Rollback(); e != nil {
			t.Fatal(e)
		}
		if body, _ := os.ReadFile(filepath.Join(root, "MusicPlayer2.exe")); string(body) != "old" {
			t.Fatal("partial restore failed", count)
		}
		for _, p := range []string{"MusicPlayerUpdater.exe", "language/test.ini", "global_cfg.ini"} {
			if _, e = os.Stat(filepath.Join(root, filepath.FromSlash(p))); !os.IsNotExist(e) {
				t.Fatal("new file remained", count, p)
			}
		}
	}
}

func TestInterruptedPreparationCleanup(t *testing.T) {
	m, _, _, _, b := fixture(t)
	files, _ := ReadPackage(b, m)
	root := t.TempDir()
	tx, e := Prepare(root, m, files)
	if e != nil {
		t.Fatal(e)
	}
	tx.State = "preparing"
	tx.Save()
	os.RemoveAll(filepath.Join(root, TransactionDir, "backup"))
	if e = Cleanup(root); e != nil {
		t.Fatal(e)
	}
	if _, e = Prepare(root, m, files); e != nil {
		t.Fatal("could not retry preparation", e)
	}
}
