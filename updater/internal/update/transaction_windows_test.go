package update

import (
	"os"
	"path/filepath"
	"syscall"
	"testing"
)

func TestOccupiedFileRestoresPreviouslyReplacedFiles(t *testing.T) {
	m, _, _, _, b := fixture(t)
	files, _ := ReadPackage(b, m)
	root := t.TempDir()
	os.WriteFile(filepath.Join(root, "MusicPlayer2.exe"), []byte("old exe"), 0600)
	locked := filepath.Join(root, "MusicPlayerUpdater.exe")
	os.WriteFile(locked, []byte("old updater"), 0600)
	tx, e := Prepare(root, m, files)
	if e != nil {
		t.Fatal(e)
	}
	p, _ := syscall.UTF16PtrFromString(locked)
	h, e := syscall.CreateFile(p, syscall.GENERIC_READ, syscall.FILE_SHARE_READ, nil, syscall.OPEN_EXISTING, syscall.FILE_ATTRIBUTE_NORMAL, 0)
	if e != nil {
		t.Fatal(e)
	}
	defer syscall.CloseHandle(h)
	if e = tx.Apply(); e == nil {
		t.Fatal("replaced a locked program file")
	}
	if e = tx.Rollback(); e != nil {
		t.Fatal("unchanged locked file prevented rollback", e)
	}
	if old, _ := os.ReadFile(filepath.Join(root, "MusicPlayer2.exe")); string(old) != "old exe" {
		t.Fatal("prior file not restored")
	}
}
