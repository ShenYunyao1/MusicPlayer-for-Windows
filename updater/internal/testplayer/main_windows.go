// Test fixture only; excluded from the distribution. No audio or network access.
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"syscall"
	"unsafe"
)

var version = "0.2.0"
var commit = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
var user = syscall.NewLazyDLL("user32.dll")

func wide(s string) *uint16 { p, _ := syscall.UTF16PtrFromString(s); return p }

type wc struct {
	Size, Style                        uint32
	Proc                               uintptr
	ClassExtra, WindowExtra            int32
	Instance, Icon, Cursor, Background uintptr
	Menu, Name                         *uint16
	SmallIcon                          uintptr
}
type pt struct{ X, Y int32 }
type msg struct {
	HWND    uintptr
	ID      uint32
	WP, LP  uintptr
	Time    uint32
	Point   pt
	Private uint32
}

func proc(h uintptr, m uint32, w, l uintptr) uintptr {
	if m == 0x8000+214 && w == 0x4D505550 {
		os.Exit(0)
	}
	r, _, _ := user.NewProc("DefWindowProcW").Call(h, uintptr(m), w, l)
	return r
}
func main() {
	runtime.LockOSThread()
	exe, _ := os.Executable()
	root := filepath.Dir(exe)
	class := wc{Size: uint32(unsafe.Sizeof(wc{})), Proc: syscall.NewCallback(proc), Name: wide("MusicPlayerUpdaterFixture")}
	user.NewProc("RegisterClassExW").Call(uintptr(unsafe.Pointer(&class)))
	h, _, _ := user.NewProc("CreateWindowExW").Call(0, uintptr(unsafe.Pointer(class.Name)), uintptr(unsafe.Pointer(wide("Updater fixture"))), 0, 0, 0, 10, 10, 0, 0, 0, 0)
	os.WriteFile(filepath.Join(root, "fixture-window"), []byte(fmt.Sprint(h)), 0600)
	os.WriteFile(filepath.Join(root, "fixture-pid"), []byte(fmt.Sprint(os.Getpid())), 0600)
	if nonce := os.Getenv("MUSICPLAYER_UPDATE_NONCE"); nonce != "" {
		os.WriteFile(filepath.Join(root, ".musicplayer-update", "ready.txt"), []byte(nonce+"\n"+version+"\n"+commit+"\n"), 0600)
	}
	var m msg
	for {
		r, _, _ := user.NewProc("GetMessageW").Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if int32(r) <= 0 {
			break
		}
		user.NewProc("DispatchMessageW").Call(uintptr(unsafe.Pointer(&m)))
	}
}
