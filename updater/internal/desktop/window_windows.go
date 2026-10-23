// Package desktop supplies a small native Windows update window, without a webview/runtime.
package desktop

import (
	"context"
	"fmt"
	"os"
	"runtime"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

var user = syscall.NewLazyDLL("user32.dll")
var kernel = syscall.NewLazyDLL("kernel32.dll")
var createWindow = user.NewProc("CreateWindowExW")
var send = user.NewProc("SendMessageW")
var post = user.NewProc("PostMessageW")
var destroy = user.NewProc("DestroyWindow")
var setText = user.NewProc("SetWindowTextW")
var active *Window

func wide(s string) *uint16 { p, _ := syscall.UTF16PtrFromString(s); return p }

type Window struct {
	mu              sync.Mutex
	hwnd, text, bar uintptr
	buttons         [3]uintptr
	content         string
	labels          []string
	answer          chan int
	cancel          context.CancelFunc
	percent         int
	show            bool
	finished        bool
	LogPath         string
}
type windowClass struct {
	Size, Style                        uint32
	Proc                               uintptr
	ClassExtra, WindowExtra            int32
	Instance, Icon, Cursor, Background uintptr
	Menu, Name                         *uint16
	SmallIcon                          uintptr
}
type point struct{ X, Y int32 }
type message struct {
	Window         uintptr
	ID             uint32
	WParam, LParam uintptr
	Time           uint32
	Point          point
	Private        uint32
}

func Run(visible bool, work func(*Window) error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	instance, _, _ := kernel.NewProc("GetModuleHandleW").Call(0)
	name := wide("MusicPlayerSignedUpdater")
	cursor, _, _ := user.NewProc("LoadCursorW").Call(0, 32512)
	class := windowClass{Size: uint32(unsafe.Sizeof(windowClass{})), Proc: syscall.NewCallback(wndProc), Instance: instance, Cursor: cursor, Background: 6, Name: name}
	user.NewProc("RegisterClassExW").Call(uintptr(unsafe.Pointer(&class)))
	syscall.NewLazyDLL("comctl32.dll").NewProc("InitCommonControls").Call()
	w := &Window{show: visible, content: "正在准备更新检查…", labels: []string{"取消"}}
	active = w
	w.hwnd, _, _ = createWindow.Call(0, uintptr(unsafe.Pointer(name)), uintptr(unsafe.Pointer(wide("MusicPlayer — 软件更新"))), 0x00CA0000, 0x80000000, 0x80000000, 680, 485, 0, 0, instance, 0)
	if w.hwnd == 0 {
		return
	}
	makeControl := func(class, text string, style, x, y, width, height uintptr) uintptr {
		h, _, _ := createWindow.Call(0, uintptr(unsafe.Pointer(wide(class))), uintptr(unsafe.Pointer(wide(text))), 0x50000000|style, x, y, width, height, w.hwnd, 0, instance, 0)
		return h
	}
	w.text = makeControl("EDIT", "", 0x4|0x800|0x40|0x200000, 20, 20, 625, 320)
	w.bar = makeControl("msctls_progress32", "", 0, 20, 352, 625, 20)
	font, _, _ := syscall.NewLazyDLL("gdi32.dll").NewProc("GetStockObject").Call(17)
	send.Call(w.text, 0x30, font, 1)
	for i := range w.buttons {
		w.buttons[i] = makeControl("BUTTON", "", 0x10000, uintptr(20+i*212), 390, 200, 32)
		send.Call(w.buttons[i], 0x30, font, 1)
	}
	w.refresh()
	go func() {
		err := work(w)
		if err != nil {
			w.WriteLog("错误: " + err.Error())
			for {
				choice := w.Ask("操作未完成：\r\n"+err.Error()+"\r\n\r\n可以重试或从官方 Release 手动下载。\r\n中断恢复：运行 MusicPlayerUpdater.exe --recover", []string{"关闭", "查看日志", "手动下载"})
				if choice == 1 {
					Open(w.LogPath)
				} else if choice == 2 {
					Open("https://github.com/ShenYunyao1/MusicPlayer-for-Windows/releases")
				} else {
					break
				}
			}
		}
		w.mu.Lock()
		w.finished = true
		w.mu.Unlock()
		post.Call(w.hwnd, 0x8002, 0, 0)
	}()
	var m message
	for {
		r, _, _ := user.NewProc("GetMessageW").Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if int32(r) <= 0 {
			break
		}
		user.NewProc("TranslateMessage").Call(uintptr(unsafe.Pointer(&m)))
		user.NewProc("DispatchMessageW").Call(uintptr(unsafe.Pointer(&m)))
	}
}
func (w *Window) refresh() {
	w.mu.Lock()
	defer w.mu.Unlock()
	setText.Call(w.text, uintptr(unsafe.Pointer(wide(w.content))))
	send.Call(w.bar, 0x402, uintptr(w.percent), 0)
	for i, h := range w.buttons {
		label := ""
		if i < len(w.labels) {
			label = w.labels[i]
		}
		setText.Call(h, uintptr(unsafe.Pointer(wide(label))))
		show := uintptr(0)
		if label != "" {
			show = 5
		}
		user.NewProc("ShowWindow").Call(h, show)
	}
	if w.show {
		user.NewProc("ShowWindow").Call(w.hwnd, 5)
	}
}
func (w *Window) Status(s string) {
	w.WriteLog(s)
	w.mu.Lock()
	w.content = s
	w.percent = 0
	if w.cancel != nil {
		w.labels = []string{"取消"}
	} else {
		w.labels = nil
	}
	w.mu.Unlock()
	post.Call(w.hwnd, 0x8001, 0, 0)
}
func (w *Window) WriteLog(s string) {
	if w.LogPath == "" {
		return
	}
	f, e := os.OpenFile(w.LogPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if e == nil {
		fmt.Fprintln(f, time.Now().Format(time.RFC3339), s)
		f.Close()
	}
}
func Open(target string) {
	syscall.NewLazyDLL("shell32.dll").NewProc("ShellExecuteW").Call(0, uintptr(unsafe.Pointer(wide("open"))), uintptr(unsafe.Pointer(wide(target))), 0, 0, 1)
}
func (w *Window) Progress(done, total int64, elapsed time.Duration) {
	if total <= 0 {
		return
	}
	seconds := elapsed.Seconds()
	if seconds < 0.1 {
		seconds = 0.1
	}
	w.mu.Lock()
	w.percent = int(done * 100 / total)
	w.content = fmt.Sprintf("正在下载更新…\r\n\r\n%.1f / %.1f MB　%d%%\r\n速度：%.2f MB/s\r\n\r\n下载期间可继续使用播放器。", float64(done)/(1<<20), float64(total)/(1<<20), w.percent, float64(done)/(1<<20)/seconds)
	w.mu.Unlock()
	post.Call(w.hwnd, 0x8001, 0, 0)
}
func (w *Window) SetCancel(f context.CancelFunc) {
	w.mu.Lock()
	w.cancel = f
	if f == nil {
		w.labels = nil
	}
	w.mu.Unlock()
	post.Call(w.hwnd, 0x8001, 0, 0)
}
func (w *Window) Ask(content string, labels []string) int {
	ch := make(chan int, 1)
	w.mu.Lock()
	w.content = content
	w.labels = labels
	w.answer = ch
	w.show = true
	w.mu.Unlock()
	post.Call(w.hwnd, 0x8001, 0, 0)
	return <-ch
}
func wndProc(hwnd uintptr, msg uint32, wp, lp uintptr) uintptr {
	w := active
	if w != nil {
		switch msg {
		case 0x8001:
			w.refresh()
			return 0
		case 0x8002:
			destroy.Call(hwnd)
			return 0
		case 0x111:
			w.mu.Lock()
			for i, h := range w.buttons {
				if h == lp {
					if w.answer != nil {
						w.answer <- i
						w.answer = nil
						w.labels = nil
					} else if w.cancel != nil {
						w.cancel()
					}
				}
			}
			w.mu.Unlock()
			w.refresh()
			return 0
		case 0x10:
			w.mu.Lock()
			if w.answer != nil {
				w.answer <- -1
				w.answer = nil
				w.labels = nil
			} else if w.cancel != nil {
				w.cancel()
			}
			w.mu.Unlock()
			return 0
		case 0x2:
			user.NewProc("PostQuitMessage").Call(0)
			return 0
		}
	}
	r, _, _ := user.NewProc("DefWindowProcW").Call(hwnd, uintptr(msg), wp, lp)
	return r
}
