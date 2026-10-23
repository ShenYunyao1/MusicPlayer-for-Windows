package main

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"musicplayer/updater/internal/desktop"
	"musicplayer/updater/internal/trust"
	"musicplayer/updater/internal/update"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

type testUI struct{}

func (testUI) Status(string)                        {}
func (testUI) SetCancel(context.CancelFunc)         {}
func (testUI) Progress(int64, int64, time.Duration) {}
func (testUI) Ask(string, []string) int             { return 0 }
func sha(b []byte) string                           { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

func TestNativeApplyAndFailedStartupRollback(t *testing.T) {
	goexe, e := exec.LookPath("go")
	if e != nil {
		t.Fatal(e)
	}
	fixture := filepath.Join(t.TempDir(), "player.exe")
	cmd := exec.Command(goexe, "build", "-o", fixture, "../../internal/testplayer")
	if b, e := cmd.CombinedOutput(); e != nil {
		t.Fatalf("fixture build: %s %v", b, e)
	}
	newExe, e := os.ReadFile(fixture)
	if e != nil {
		t.Fatal(e)
	}
	oldKey, oldVersion, oldTimeout := verificationKey, currentVersion, startupTimeout
	defer func() { verificationKey, currentVersion, startupTimeout = oldKey, oldVersion, oldTimeout }()
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	verificationKey = pub
	currentVersion = "0.1.9"
	startupTimeout = 2 * time.Second
	for _, fail := range []bool{false, true} {
		t.Run(strconv.FormatBool(fail), func(t *testing.T) {
			root, work := t.TempDir(), t.TempDir()
			if realPackage := os.Getenv("MUSICPLAYER_REAL_TEST_PAYLOAD"); realPackage != "" && !fail {
				base := filepath.Join(filepath.Dir(realPackage), "test-results")
				os.MkdirAll(base, 0700)
				root, e = os.MkdirTemp(base, "更新 test-")
				if e != nil {
					t.Fatal(e)
				}
				t.Log("Isolated real-player installation:", root)
			}
			defer stopTestPlayers(root)
			parentPath := filepath.Join(root, "MusicPlayer2.exe")
			os.WriteFile(parentPath, newExe, 0700)
			os.WriteFile(filepath.Join(root, "global_cfg.ini"), []byte("personal"), 0600)
			os.WriteFile(filepath.Join(root, "remote-library.json"), []byte("encrypted-fixture"), 0600)
			parent := exec.Command(parentPath)
			if e = parent.Start(); e != nil {
				t.Fatal(e)
			}
			defer parent.Process.Kill()
			var window uint64
			for i := 0; i < 100; i++ {
				b, _ := os.ReadFile(filepath.Join(root, "fixture-window"))
				window, _ = strconv.ParseUint(string(b), 10, 64)
				if window != 0 {
					break
				}
				time.Sleep(20 * time.Millisecond)
			}
			if window == 0 {
				t.Fatal("fixture window missing")
			}
			var created, ex, k, u syscall.Filetime
			h, e := syscall.OpenProcess(0x1000, false, uint32(parent.Process.Pid))
			if e != nil {
				t.Fatal(e)
			}
			syscall.GetProcessTimes(h, &created, &ex, &k, &u)
			syscall.CloseHandle(h)
			identity := uint64(created.HighDateTime)<<32 | uint64(created.LowDateTime)
			payload := map[string][]byte{"MusicPlayer2.exe": newExe, "MusicPlayerUpdater.exe": []byte("fixture updater"), "global_cfg.ini": []byte("default must not replace personal")}
			if fail {
				payload["MusicPlayer2.exe"] = []byte("not executable")
			}
			var buf bytes.Buffer
			z := zip.NewWriter(&buf)
			m := update.Manifest{Protocol: 1, Product: update.Product, Version: "0.2.0", Commit: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", Platform: "windows-x64", MinimumUpdater: 1, KeyID: "client-v1"}
			for name, b := range payload {
				w, _ := z.Create("MusicPlayer2/" + name)
				w.Write(b)
				m.Files = append(m.Files, update.File{Path: name, Size: int64(len(b)), SHA256: sha(b)})
			}
			z.Close()
			m.ExecutableSHA256 = sha(payload["MusicPlayer2.exe"])
			m.Assets = []update.Asset{{Name: "MusicPlayer2.zip", Size: int64(buf.Len()), SHA256: sha(buf.Bytes())}}
			raw, _ := json.Marshal(m)
			if realPackage := os.Getenv("MUSICPLAYER_REAL_TEST_PAYLOAD"); realPackage != "" && !fail {
				// Opt-in local test of the real signed distribution and MFC readiness.
				work = realPackage
				verificationKey = trust.PublicKey()
				startupTimeout = 40 * time.Second
				actual, _, _, actualFiles, e := load(realPackage, true)
				if e != nil {
					t.Fatal(e)
				}
				for name, b := range actualFiles {
					if name == "MusicPlayer2.exe" || name == "MusicPlayerUpdater.exe" || name == "global_cfg.ini" {
						continue
					}
					p := filepath.Join(root, filepath.FromSlash(name))
					os.MkdirAll(filepath.Dir(p), 0700)
					if e = os.WriteFile(p, b, 0600); e != nil {
						t.Fatal(e)
					}
				}
				os.WriteFile(filepath.Join(root, "global_cfg.ini"), []byte("[config]\nportable_mode=true\n"), 0600)
				os.WriteFile(filepath.Join(root, "config.ini"), []byte("[general]\ncheck_update_when_start=false\n[config]\nauto_play_when_start=false\n"), 0600)
				os.WriteFile(filepath.Join(root, "song_data.dat"), nil, 0600)
				_ = actual
			} else {
				verificationKey = pub
				startupTimeout = 2 * time.Second
				os.WriteFile(filepath.Join(work, "client-release-manifest.json"), raw, 0600)
				os.WriteFile(filepath.Join(work, "client-release-manifest.json.sig"), ed25519.Sign(priv, raw), 0600)
				os.WriteFile(filepath.Join(work, "MusicPlayer2.zip"), buf.Bytes(), 0600)
			}
			e = applyInstall(testUI{}, root, work, uint32(parent.Process.Pid), identity, uintptr(window))
			parent.Wait()
			if (e != nil) != fail {
				t.Fatalf("failure=%v error=%v", fail, e)
			}
			for _, name := range []string{"global_cfg.ini", "remote-library.json"} {
				b, _ := os.ReadFile(filepath.Join(root, name))
				want := "personal"
				if os.Getenv("MUSICPLAYER_REAL_TEST_PAYLOAD") != "" && !fail {
					want = "[config]\nportable_mode=true\n"
				}
				if name == "remote-library.json" {
					want = "encrypted-fixture"
				}
				if name == "global_cfg.ini" && os.Getenv("MUSICPLAYER_REAL_TEST_PAYLOAD") != "" && !fail {
					if !bytes.Contains(b, []byte("portable_mode")) {
						t.Fatal("lost portable mode")
					}
					continue
				}
				if string(b) != want {
					t.Fatal("changed data", name)
				}
			}
			tx, e := update.LoadTransaction(root)
			if e != nil {
				t.Fatal(e)
			}
			want := "complete"
			if fail {
				want = "rolled_back"
			}
			if tx.State != want {
				t.Fatal(tx.State)
			}
			for i := 0; i < 100; i++ {
				b, _ := os.ReadFile(filepath.Join(root, "fixture-pid"))
				pid, _ := strconv.Atoi(string(b))
				if pid != parent.Process.Pid && pid > 0 {
					if p, e := os.FindProcess(pid); e == nil {
						p.Kill()
						p.Wait()
					}
					break
				}
				time.Sleep(20 * time.Millisecond)
			}
		})
	}
}

func stopTestPlayers(root string) {
	snapshot, e := syscall.CreateToolhelp32Snapshot(syscall.TH32CS_SNAPPROCESS, 0)
	if e != nil {
		return
	}
	defer syscall.CloseHandle(snapshot)
	var entry syscall.ProcessEntry32
	entry.Size = uint32(unsafe.Sizeof(entry))
	for e = syscall.Process32First(snapshot, &entry); e == nil; e = syscall.Process32Next(snapshot, &entry) {
		if syscall.UTF16ToString(entry.ExeFile[:]) != "MusicPlayer2.exe" {
			continue
		}
		p, e := desktop.OpenParent(entry.ProcessID, root)
		if e != nil {
			continue
		}
		u := syscall.NewLazyDLL("user32.dll")
		pid := entry.ProcessID
		cb := syscall.NewCallback(func(hwnd, lp uintptr) uintptr {
			var owner uint32
			u.NewProc("GetWindowThreadProcessId").Call(hwnd, uintptr(unsafe.Pointer(&owner)))
			if owner == pid {
				u.NewProc("PostMessageW").Call(hwnd, 0x8000+214, 0x4D505550, 0)
			}
			return 1
		})
		u.NewProc("EnumWindows").Call(cb, 0)
		if e = p.Wait(10 * time.Second); e != nil {
			if proc, e := os.FindProcess(int(pid)); e == nil {
				proc.Kill()
				proc.Wait()
			}
		}
		p.Close()
	}
}
