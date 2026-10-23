package desktop

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

type Process struct {
	handle syscall.Handle
	pid    uint32
}

func OpenParent(pid uint32, install string) (*Process, error) {
	h, err := syscall.OpenProcess(0x1000|0x100000, false, pid)
	if err != nil {
		return nil, err
	}
	var buf [32768]uint16
	size := uint32(len(buf))
	ok, _, e := kernel.NewProc("QueryFullProcessImageNameW").Call(uintptr(h), 0, uintptr(unsafe.Pointer(&buf[0])), uintptr(unsafe.Pointer(&size)))
	want, _ := filepath.Abs(filepath.Join(install, "MusicPlayer2.exe"))
	if ok == 0 || !strings.EqualFold(syscall.UTF16ToString(buf[:size]), want) {
		syscall.CloseHandle(h)
		return nil, fmt.Errorf("无法确认待退出播放器的安装路径: %v", e)
	}
	return &Process{h, pid}, nil
}
func (p *Process) Close() { syscall.CloseHandle(p.handle) }
func (p *Process) VerifyCreation(expected uint64) error {
	var created, exit, kernelTime, userTime syscall.Filetime
	if err := syscall.GetProcessTimes(p.handle, &created, &exit, &kernelTime, &userTime); err != nil {
		return err
	}
	actual := uint64(created.HighDateTime)<<32 | uint64(created.LowDateTime)
	if expected == 0 || actual != expected {
		return errors.New("播放器已重启或进程身份不匹配，请重新检查更新")
	}
	return nil
}
func (p *Process) Wait(timeout time.Duration) error {
	r, e := syscall.WaitForSingleObject(p.handle, uint32(timeout.Milliseconds()))
	if e != nil {
		return e
	}
	if r != 0 {
		return errors.New("播放器未完成退出，尚未替换任何文件")
	}
	var code uint32
	if e := syscall.GetExitCodeProcess(p.handle, &code); e != nil {
		return e
	}
	if code != 0 {
		return errors.New("播放器异常退出，尚未替换任何文件，请重新启动检查后重试")
	}
	return nil
}
func RequestExit(p *Process, window uintptr) error {
	var pid uint32
	user.NewProc("GetWindowThreadProcessId").Call(window, uintptr(unsafe.Pointer(&pid)))
	if pid != p.pid {
		return errors.New("更新窗口与播放器进程不匹配")
	}
	ok, _, e := post.Call(window, 0x8000+214, 0x4D505550, 0)
	if ok == 0 {
		return e
	}
	return nil
}
func Lock(path string) (func(), error) {
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return nil, err
	}
	h, e := syscall.CreateFile(wide(path), syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0, nil, syscall.OPEN_ALWAYS, syscall.FILE_ATTRIBUTE_NORMAL, 0)
	if e != nil {
		return nil, e
	}
	return func() { syscall.CloseHandle(h) }, nil
}
func Preflight(install string, required int64) error {
	r, err := filepath.Abs(install)
	if err != nil {
		return err
	}
	if strings.HasPrefix(r, `\\`) {
		return errors.New("自动更新不支持网络目录")
	}
	for p := r; ; p = filepath.Dir(p) {
		attrs, e := syscall.GetFileAttributes(wide(p))
		if e != nil {
			return e
		}
		if attrs&0x400 != 0 {
			return errors.New("安装路径包含重解析点")
		}
		if p == filepath.Dir(p) {
			break
		}
	}
	typeCode, _, _ := kernel.NewProc("GetDriveTypeW").Call(uintptr(unsafe.Pointer(wide(filepath.VolumeName(r) + `\`))))
	if typeCode != 3 {
		return errors.New("自动更新仅支持本地固定磁盘目录")
	}
	var available, total, free uint64
	ok, _, e := kernel.NewProc("GetDiskFreeSpaceExW").Call(uintptr(unsafe.Pointer(wide(r))), uintptr(unsafe.Pointer(&available)), uintptr(unsafe.Pointer(&total)), uintptr(unsafe.Pointer(&free)))
	if ok == 0 {
		return e
	}
	if available < uint64(required) {
		return errors.New("磁盘可用空间不足，请清理后重试")
	}
	f, e := os.CreateTemp(r, ".update-write-probe-")
	if e != nil {
		return errors.New("安装目录无写入权限，请手动下载或移动到用户可写目录")
	}
	p := f.Name()
	f.Close()
	defer os.Remove(p)
	defer os.Remove(p + ".renamed")
	if e = os.Rename(p, p+".renamed"); e != nil {
		return errors.New("安装目录无法重命名文件，请检查权限")
	}
	return nil
}
