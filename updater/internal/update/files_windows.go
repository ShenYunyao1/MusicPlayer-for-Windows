package update

import "syscall"

var moveFileEx = syscall.NewLazyDLL("kernel32.dll").NewProc("MoveFileExW")

func replaceFile(from, to string) error {
	a, err := syscall.UTF16PtrFromString(from)
	if err != nil {
		return err
	}
	b, err := syscall.UTF16PtrFromString(to)
	if err != nil {
		return err
	}
	ok, _, e := moveFileEx.Call(uintptr(unsafePointer(a)), uintptr(unsafePointer(b)), 0x1|0x8)
	if ok == 0 {
		return e
	}
	return nil
}
func isReparse(p string) bool {
	a, e := syscall.UTF16PtrFromString(p)
	if e != nil {
		return true
	}
	attr, e := syscall.GetFileAttributes(a)
	return e == nil && attr&0x400 != 0
}
