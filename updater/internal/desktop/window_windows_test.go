package desktop

import (
	"context"
	"math"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
)

func TestSpacePreflightFailsBeforeChanges(t *testing.T) {
	err := Preflight(t.TempDir(), math.MaxInt64)
	if err == nil || !strings.Contains(err.Error(), "空间不足") {
		t.Fatal(err)
	}
}

func TestWindowCanInitializeAndExitHidden(t *testing.T) {
	if os.Getenv("MUSICPLAYER_WINDOW_TEST") == "1" {
		ran := false
		Run(true, func(w *Window) error {
			ran = true
			w.Status("fixture status")
			w.Progress(1, 2, time.Second)
			go func() { time.Sleep(100 * time.Millisecond); post.Call(w.hwnd, 0x111, 0, w.buttons[0]) }()
			if w.Ask("原生窗口交互测试", []string{"关闭"}) != 0 {
				t.Error("button reply failed")
			}
			return nil
		})
		if !ran {
			t.Fatal("native window creation failed")
		}
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, os.Args[0], "-test.run=^TestWindowCanInitializeAndExitHidden$")
	cmd.Env = append(os.Environ(), "MUSICPLAYER_WINDOW_TEST=1")
	if b, e := cmd.CombinedOutput(); e != nil {
		t.Fatalf("native window failed to initialize/exit: %v %s", e, b)
	}
}
