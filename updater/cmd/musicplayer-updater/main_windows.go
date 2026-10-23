package main

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"musicplayer/updater/internal/desktop"
	"musicplayer/updater/internal/trust"
	"musicplayer/updater/internal/update"
)

var currentVersion = "0.0.0"
var currentCommit = ""
var startupTimeout = 90 * time.Second
var verificationKey ed25519.PublicKey = trust.PublicKey()

type updateUI interface {
	SetCancel(context.CancelFunc)
	Status(string)
	Progress(int64, int64, time.Duration)
	Ask(string, []string) int
}

func main() {
	install := flag.String("install", "", "installation directory")
	stateDir := flag.String("state", "", "per-user check state directory")
	work := flag.String("work", "", "private downloaded package directory")
	parent := flag.Uint("parent", 0, "parent process")
	parentCreated := flag.Uint64("parent-created", 0, "parent creation FILETIME")
	window := flag.Uint64("window", 0, "player window")
	background := flag.Bool("background", false, "silent unless an update is available")
	apply := flag.Bool("apply", false, "apply verified staged package")
	recoverMode := flag.Bool("recover", false, "restore an interrupted update")
	inspect := flag.Bool("inspect", false, "verify and print staged manifest")
	verify := flag.Bool("verify-package", false, "verify staged package without installation")
	flag.Parse()
	if *install == "" {
		exe, _ := os.Executable()
		*install = filepath.Dir(exe)
	}
	if *stateDir == "" {
		base, _ := os.UserCacheDir()
		*stateDir = filepath.Join(base, "MusicPlayer2", "updates")
	}
	if !*apply && !*inspect && !*verify {
		if tx, e := update.LoadTransaction(*install); e == nil && tx.State != "complete" && tx.State != "rolled_back" && tx.State != "prepared" {
			*recoverMode = true
		}
	}
	if *recoverMode {
		exe, _ := os.Executable()
		a, _ := filepath.Abs(filepath.Dir(exe))
		b, _ := filepath.Abs(*install)
		if strings.EqualFold(a, b) {
			os.MkdirAll(*stateDir, 0700)
			dir, e := os.MkdirTemp(*stateDir, "recovery-")
			if e != nil {
				desktop.Run(true, func(*desktop.Window) error { return e })
				return
			}
			body, e := os.ReadFile(exe)
			if e == nil {
				e = os.WriteFile(filepath.Join(dir, "MusicPlayerUpdater.exe"), body, 0700)
			}
			if e == nil {
				e = exec.Command(filepath.Join(dir, "MusicPlayerUpdater.exe"), "--recover", "--install", *install, "--state", *stateDir, "--parent", fmt.Sprint(*parent)).Start()
			}
			if e != nil {
				desktop.Run(true, func(*desktop.Window) error { return e })
			}
			return
		}
	}
	if *inspect || *verify {
		m, _, _, _, err := load(*work, *verify)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		json.NewEncoder(os.Stdout).Encode(m)
		return
	}
	desktop.Run(!*background, func(ui *desktop.Window) error {
		lock, err := desktop.Lock(filepath.Join(*install, ".musicplayer-updater.lock"))
		if *apply {
			for i := 0; err != nil && i < 50; i++ {
				time.Sleep(100 * time.Millisecond)
				lock, err = desktop.Lock(filepath.Join(*install, ".musicplayer-updater.lock"))
			}
		}
		if err != nil {
			if *background {
				return nil
			}
			return errors.New("另一个更新窗口正在运行或目录无写权限，请检查后重试")
		}
		defer lock()
		os.MkdirAll(*stateDir, 0700)
		ui.LogPath = filepath.Join(*stateDir, "latest-update.log")
		defer func() { ui.WriteLog("更新器退出") }()
		if *recoverMode {
			tx, _ := update.LoadTransaction(*install)
			// Preparation has not changed program files; it can be discarded
			// without waiting for a running player's exit.
			if *parent != 0 && (tx == nil || tx.State != "preparing" && tx.State != "prepared") {
				p, e := desktop.OpenParent(uint32(*parent), *install)
				if e == nil {
					defer p.Close()
					if e = p.Wait(90 * time.Second); e != nil {
						return e
					}
				}
			}
			return recoverInstall(ui, *install)
		}
		if *apply {
			return applyInstall(ui, *install, *work, uint32(*parent), *parentCreated, uintptr(*window))
		}
		return check(ui, *install, *stateDir, uint32(*parent), *parentCreated, uintptr(*window), *background)
	})
}
func load(work string, withPackage bool) (update.Manifest, []byte, []byte, map[string][]byte, error) {
	raw, e := os.ReadFile(filepath.Join(work, "client-release-manifest.json"))
	if e != nil {
		return update.Manifest{}, nil, nil, nil, e
	}
	sig, e := os.ReadFile(filepath.Join(work, "client-release-manifest.json.sig"))
	if e != nil {
		return update.Manifest{}, nil, nil, nil, e
	}
	m, e := update.ParseManifest(raw, sig, verificationKey)
	if e != nil {
		return m, nil, nil, nil, e
	}
	if !withPackage {
		return m, raw, sig, nil, nil
	}
	info, e := os.Stat(filepath.Join(work, "MusicPlayer2.zip"))
	if e != nil {
		return m, nil, nil, nil, e
	}
	if info.Size() != m.Assets[0].Size {
		return m, nil, nil, nil, errors.New("包大小不符")
	}
	b, e := os.ReadFile(filepath.Join(work, "MusicPlayer2.zip"))
	if e != nil {
		return m, nil, nil, nil, e
	}
	files, e := update.ReadPackage(b, m)
	return m, raw, sig, files, e
}
func check(ui updateUI, install, stateDir string, parent uint32, parentCreated uint64, window uintptr, background bool) error {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	ui.SetCancel(cancel)
	s := update.LoadCheckState(stateDir)
	if elapsed := time.Now().Unix() - s.Checked; background && elapsed >= 0 && elapsed < 86400 {
		return nil
	}
	ui.Status("正在连接 GitHub 并验证发行清单…")
	// Bound startup attempts as well as successful checks. Manual checks bypass this.
	s.Checked = time.Now().Unix()
	if err := update.SaveCheckState(stateDir, s); err != nil {
		return err
	}
	h := update.NewHTTP()
	m, raw, sig, source, err := h.Discover(ctx, verificationKey, &s, "https://api.github.com/repos/"+update.Repository+"/releases/latest")
	if err != nil {
		if background || ctx.Err() != nil {
			return nil
		}
		return err
	}
	if err = update.SaveCheckState(stateDir, s); err != nil {
		return err
	}
	n, err := update.CompareVersion(m.Version, currentVersion)
	if err != nil {
		return err
	}
	if n <= 0 {
		if !background {
			ui.Ask("当前已是最新正式版本 "+currentVersion, []string{"关闭"})
		}
		return nil
	}
	if background && s.Skipped == m.Version {
		return nil
	}
	choice := ui.Ask(fmt.Sprintf("当前版本：%s\r\n新版本：%s\r\n下载大小：%.1f MB\r\n\r\n%s", currentVersion, m.Version, float64(m.Assets[0].Size)/(1<<20), strings.ReplaceAll(m.Notes, "\n", "\r\n")), []string{"下载更新", "稍后", "跳过此版本"})
	if choice != 0 {
		if choice == 2 {
			s.Skipped = m.Version
			return update.SaveCheckState(stateDir, s)
		}
		return nil
	}
	ui.Status("正在下载更新…（可取消）")
	start := time.Now()
	body, err := h.Download(ctx, source, m, func(done, total int64) { ui.Progress(done, total, time.Since(start)) })
	if err != nil {
		if ctx.Err() != nil {
			return nil
		}
		return err
	}
	ui.Status("正在校验并检查安装目录…")
	files, err := update.ReadPackage(body, m)
	if err != nil {
		return err
	}
	if err = desktop.Preflight(install, requiredSpace(m)); err != nil {
		return err
	}
	if err = update.Cleanup(install); err != nil {
		return err
	}
	tx, err := update.Prepare(install, m, files)
	if err != nil {
		return err
	}
	if err = update.Cleanup(install); err != nil {
		return err
	}
	_ = tx
	if parent == 0 || window == 0 {
		return errors.New("请从播放器的“检查更新”入口安装；独立更新器可用于恢复")
	}
	if ui.Ask("下载和校验完成。安装将停止播放、保存个人数据并重启播放器。", []string{"安装并重启", "稍后"}) != 0 {
		return nil
	}
	// No cancellation after this point: the replacement process owns recovery.
	ui.SetCancel(nil)
	cleanupDownloads(stateDir)
	work, err := os.MkdirTemp(stateDir, "transaction-")
	if err != nil {
		return err
	}
	for name, data := range map[string][]byte{"client-release-manifest.json": raw, "client-release-manifest.json.sig": sig, "MusicPlayer2.zip": body} {
		if err = os.WriteFile(filepath.Join(work, name), data, 0600); err != nil {
			return err
		}
	}
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	helper, err := os.ReadFile(exe)
	if err != nil {
		return err
	}
	staged := filepath.Join(work, "MusicPlayerUpdater.exe")
	if err = os.WriteFile(staged, helper, 0700); err != nil {
		return err
	}
	cmd := exec.Command(staged, "--apply", "--install", install, "--work", work, "--state", stateDir, "--parent", fmt.Sprint(parent), "--parent-created", fmt.Sprint(parentCreated), "--window", fmt.Sprint(window))
	return cmd.Start()
}
func applyInstall(ui updateUI, install, work string, parent uint32, parentCreated uint64, window uintptr) error {
	ui.SetCancel(nil)
	ui.Status("正在验证安装事务…")
	m, _, _, files, err := load(work, true)
	if err != nil {
		return err
	}
	n, err := update.CompareVersion(m.Version, currentVersion)
	if err != nil || n <= 0 {
		return errors.New("拒绝同版本安装或降级")
	}
	if err = desktop.Preflight(install, requiredSpace(m)); err != nil {
		return err
	}
	process, err := desktop.OpenParent(parent, install)
	if err != nil {
		return err
	}
	defer process.Close()
	if err = process.VerifyCreation(parentCreated); err != nil {
		return err
	}
	if err = desktop.RequestExit(process, window); err != nil {
		return err
	}
	ui.Status("正在等待播放器保存数据并退出…")
	if err = process.Wait(90 * time.Second); err != nil {
		return err
	}
	// A successful build may carry a changed resource baseline; validate again after exit.
	if err = update.Cleanup(install); err != nil {
		return err
	}
	tx, err := update.Prepare(install, m, files)
	if err != nil {
		return err
	}
	ui.Status("正在替换程序文件，请勿关闭更新器…")
	if err = tx.Apply(); err == nil {
		ui.Status("正在重启并等待新版启动确认…")
		err = launchAndConfirm(install, m)
	}
	if err != nil {
		ui.Status("更新未完成，正在恢复上一版…")
		if rollbackErr := tx.Rollback(); rollbackErr != nil {
			return fmt.Errorf("更新失败；恢复也未完成，请运行 MusicPlayerUpdater.exe --recover。%v", rollbackErr)
		}
		_ = exec.Command(filepath.Join(install, "MusicPlayer2.exe")).Start()
		return fmt.Errorf("更新失败，已恢复上一版程序：%v", err)
	}
	if err = tx.Complete(); err != nil {
		return err
	}
	ui.Ask("更新成功，已启动 "+m.Version+"。个人配置、Key、歌单和缓存已保留。", []string{"关闭"})
	return nil
}
func requiredSpace(m update.Manifest) int64 {
	total := m.Assets[0].Size
	for _, f := range m.Files {
		total += f.Size * 3
	}
	return total + (32 << 20)
}

func cleanupDownloads(stateDir string) {
	entries, _ := os.ReadDir(stateDir)
	for _, entry := range entries {
		if !entry.IsDir() || !strings.HasPrefix(entry.Name(), "transaction-") {
			continue
		}
		dir := filepath.Join(stateDir, entry.Name())
		resolved, e := filepath.EvalSymlinks(dir)
		abs, _ := filepath.Abs(dir)
		if e != nil || !strings.EqualFold(abs, resolved) {
			continue
		}
		info, e := entry.Info()
		if e != nil || time.Since(info.ModTime()) < 24*time.Hour {
			continue
		}
		// Remove only known updater files, never recursively delete an arbitrary folder.
		for _, name := range []string{"client-release-manifest.json", "client-release-manifest.json.sig", "MusicPlayer2.zip", "MusicPlayerUpdater.exe"} {
			_ = os.Remove(filepath.Join(dir, name))
		}
		_ = os.Remove(dir)
	}
}
func launchAndConfirm(install string, m update.Manifest) error {
	var nonce [32]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return err
	}
	token := hex.EncodeToString(nonce[:])
	ready := filepath.Join(install, update.TransactionDir, "ready.txt")
	_ = os.Remove(ready)
	cmd := exec.Command(filepath.Join(install, "MusicPlayer2.exe"))
	cmd.Dir = install
	cmd.Env = append(os.Environ(), "MUSICPLAYER_UPDATE_NONCE="+token)
	if err := cmd.Start(); err != nil {
		return err
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	deadline := time.NewTimer(startupTimeout)
	defer deadline.Stop()
	tick := time.NewTicker(250 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-tick.C:
			b, _ := os.ReadFile(ready)
			if string(b) == token+"\n"+m.Version+"\n"+m.Commit+"\n" {
				return nil
			}
		case <-done:
			return errors.New("新版在完成初始化前退出（请检查单实例冲突或运行库）")
		case <-deadline.C:
			// Only terminate the exact newly launched process; never a different player instance.
			_ = cmd.Process.Kill()
			<-done
			return errors.New("新版启动确认超时")
		}
	}
}
func recoverInstall(ui updateUI, install string) error {
	tx, err := update.LoadTransaction(install)
	if err != nil {
		return err
	}
	if tx.State == "complete" || tx.State == "rolled_back" {
		ui.Ask("上次事务已结束，无需恢复。", []string{"关闭"})
		return nil
	}
	if tx.State == "preparing" || tx.State == "prepared" {
		if ui.Ask("上次更新在准备阶段中断，程序文件未被替换。是否清理暂存？", []string{"清理暂存", "取消"}) == 0 {
			return update.Cleanup(install)
		}
		return nil
	}
	if ui.Ask("请先退出此目录的播放器。恢复将还原上一版程序，保留个人数据。", []string{"恢复上一版", "取消"}) != 0 {
		return nil
	}
	if err = tx.Rollback(); err != nil {
		return err
	}
	ui.Ask("已恢复上一版程序，可以重新启动播放器。", []string{"关闭"})
	return nil
}
