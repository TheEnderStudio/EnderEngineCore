// Package ui 提供 Fyne 图形界面。
package ui

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"buildhelper/internal/config"
	"buildhelper/internal/runner"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

type depUI struct {
	name   string
	status string
	prog   float64
}

// 日志显示参数：内存保留 2000 行，界面只渲染尾部 500 行并节流更新，
// 避免日志越长 RichText 重排开销越大导致界面卡顿。
const (
	logKeepTotal   = 2000
	logDisplayMax  = 500
	logUpdateEvery = 80 * time.Millisecond
)

// App 是 BuildHelper 的图形界面。
type App struct {
	fyneApp fyne.App
	win     fyne.Window
	cfg     *config.Config
	cfgPath string
	run     *runner.Runner

	mu     sync.Mutex
	depsUI []depUI

	depList   *widget.List
	logText   *widget.RichText
	logScroll *container.Scroll
	logBuf    []string
	lastLog   time.Time
	progress  *widget.ProgressBar
	status    *widget.Label
	rootLbl   *widget.Label

	btnDownload *widget.Button
	btnBuild    *widget.Button
	btnAll      *widget.Button
	btnForce    *widget.Button
	btnStop     *widget.Button
	btnDepDown  *widget.Button
	btnDepBuild *widget.Button
	btnDepReset *widget.Button
	btnDepClean *widget.Button
	btnDepPath  *widget.Button

	cancel     context.CancelFunc
	busy       bool
	selectedID int
}

// New 创建界面。
func New(cfg *config.Config, cfgPath, root string) *App {
	a := &App{
		fyneApp:    app.NewWithID("com.enderengine.buildhelper"),
		cfg:        cfg,
		cfgPath:    cfgPath,
		run:        runner.New(cfg, root),
		selectedID: -1, // 尚未选中任何依赖
	}
	a.buildUI()
	a.refreshList()
	return a
}

// Run 显示窗口并进入事件循环。
func (a *App) Run() {
	go a.pumpEvents()
	go func() {
		time.Sleep(400 * time.Millisecond)
		a.toolCheck()
	}()
	a.win.ShowAndRun()
}

func (a *App) buildUI() {
	a.win = a.fyneApp.NewWindow("EnderEngine BuildHelper")
	a.win.Resize(fyne.NewSize(1060, 700))
	a.win.SetCloseIntercept(func() {
		if a.cancel != nil {
			a.cancel()
		}
		a.win.Close()
	})

	a.depsUI = make([]depUI, 0, len(a.cfg.Dependencies))
	for i := range a.cfg.Dependencies {
		d := &a.cfg.Dependencies[i]
		a.depsUI = append(a.depsUI, depUI{name: d.Name, status: a.depInitialStatus(d)})
	}

	a.depList = widget.NewList(
		func() int { return len(a.depsUI) },
		func() fyne.CanvasObject { return widget.NewLabel("") },
		func(id widget.ListItemID, obj fyne.CanvasObject) {
			lb := obj.(*widget.Label)
			a.mu.Lock()
			du := a.depsUI[id]
			a.mu.Unlock()
			lb.SetText(fmt.Sprintf("%-22s  %s", du.name, du.status))
		},
	)
	a.depList.OnSelected = func(id widget.ListItemID) {
		a.mu.Lock()
		a.selectedID = id
		a.mu.Unlock()
		a.updateDepButtons()
	}
	a.depList.OnUnselected = func(id widget.ListItemID) {
		a.mu.Lock()
		a.selectedID = -1
		a.mu.Unlock()
		a.updateDepButtons()
	}

	a.logText = widget.NewRichTextWithText("")
	a.logText.Wrapping = fyne.TextWrapWord
	a.logScroll = container.NewScroll(a.logText)

	a.progress = widget.NewProgressBar()
	a.status = widget.NewLabel("就绪")
	a.rootLbl = widget.NewLabel("引擎根目录: " + a.run.Root)

	a.btnDownload = widget.NewButton("下载全部", func() { a.action(true, false, false) })
	a.btnBuild = widget.NewButton("构建全部", func() { a.action(false, true, false) })
	a.btnAll = widget.NewButton("下载并构建全部", func() { a.action(true, true, false) })
	a.btnForce = widget.NewButton("强制重建全部", func() { a.action(true, true, true) })
	a.btnStop = widget.NewButton("停止", func() {
		if a.cancel != nil {
			a.cancel()
			a.appendLog("", "正在停止…")
		}
	})
	a.btnDepDown = widget.NewButton("下载选中", func() { a.actionDep(true, false) })
	a.btnDepBuild = widget.NewButton("构建选中", func() { a.actionDep(false, true) })
	a.btnDepReset = widget.NewButton("重置选中", func() { a.resetDep() })
	a.btnDepClean = widget.NewButton("清理选中", func() { a.cleanDep() })
	a.btnDepPath = widget.NewButton("选择路径…", func() { a.chooseDepPath() })

	top := container.NewHBox(
		widget.NewButton("工具链检测", func() { a.toolCheck() }),
		widget.NewButton("重新加载配置", func() { a.reloadConfig() }),
		widget.NewButton("选择根目录…", func() { a.chooseRoot() }),
		widget.NewButton("刷新日志", func() { a.flushLog() }),
		a.rootLbl,
	)
	depPanel := container.NewBorder(
		nil,
		container.NewHBox(a.btnDepDown, a.btnDepBuild, a.btnDepReset, a.btnDepClean, a.btnDepPath),
		nil, nil,
		a.depList,
	)
	center := container.NewHSplit(depPanel, a.logScroll)
	center.SetOffset(0.38)

	bottom := container.NewBorder(
		a.status,
		container.NewHBox(a.btnDownload, a.btnBuild, a.btnAll, a.btnForce, a.btnStop),
		nil, nil,
		a.progress,
	)

	a.win.SetContent(container.NewBorder(top, bottom, nil, nil, center))
	a.setBusy(false)
}

func (a *App) refreshList() {
	a.mu.Lock()
	a.depsUI = make([]depUI, 0, len(a.cfg.Dependencies))
	for i := range a.cfg.Dependencies {
		d := &a.cfg.Dependencies[i]
		a.depsUI = append(a.depsUI, depUI{name: d.Name, status: a.depInitialStatus(d)})
	}
	a.mu.Unlock()
	if a.depList != nil {
		a.depList.Refresh()
	}
	a.selectFirst()
	a.updateDepButtons()
}

// selectFirst 默认选中第一项，使界面高亮与内部 selectedID 保持一致，
// 避免「界面未选中、内部却默认选中第一项」导致按钮作用在隐藏项上。
func (a *App) selectFirst() {
	if a.depList == nil {
		return
	}
	a.mu.Lock()
	n := len(a.depsUI)
	a.mu.Unlock()
	if n == 0 {
		a.mu.Lock()
		a.selectedID = -1
		a.mu.Unlock()
		return
	}
	a.depList.Select(0)
}

// depInitialStatus 计算依赖的初始状态文本（userpath 依赖提示路径是否已设置）。
func (a *App) depInitialStatus(d *config.Dependency) string {
	if s := a.run.Status(d.Name); s != "未处理" {
		return s
	}
	if d.Kind == "userpath" {
		if a.run.UserPath(d) != "" {
			return "手动构建 ✓"
		}
		return "待选择路径"
	}
	return "未处理"
}

// selectedDepCfg 返回当前选中依赖的配置项。
func (a *App) selectedDepCfg() *config.Dependency {
	name, ok := a.selectedDep()
	if !ok {
		return nil
	}
	for i := range a.cfg.Dependencies {
		if a.cfg.Dependencies[i].Name == name {
			return &a.cfg.Dependencies[i]
		}
	}
	return nil
}

// updateDepButtons 根据当前选中项刷新按钮可用状态。
func (a *App) updateDepButtons() {
	if a.busy {
		a.btnDepPath.Disable()
		return
	}
	d := a.selectedDepCfg()
	if d != nil && d.Kind == "userpath" {
		a.btnDepPath.Enable()
	} else {
		a.btnDepPath.Disable()
	}
}

// pumpEvents 在后台 goroutine 读取运行事件，批量派发到主线程更新 UI。
// 重要: fyne v2.5 的 StartAnimation 回调运行在独立 goroutine 而非主线程，
// 直接在回调里改 widget 会产生数据竞争（导致高 CPU、界面卡死）。
// fyne v2.6 提供 fyne.Do 将函数可靠地派发到主线程执行。
func (a *App) pumpEvents() {
	for {
		batch := drainEvents(a.run.Events, 256)
		if batch == nil {
			return
		}
		evs := batch
		fyne.Do(func() {
			for _, e := range evs {
				a.handleEvent(e)
			}
		})
	}
}

// drainEvents 从事件通道批量取出事件（最多 max 条），通道空闲时立即返回。
// 注意: 必须以带标签的 break 退出内层循环——普通 break 只退出 select，
// 会变成无限空转（livelock）：单核 100% 占用、后续事件全被吞掉、日志冻结。
func drainEvents(ch <-chan runner.Event, max int) []runner.Event {
	first, ok := <-ch
	if !ok {
		return nil
	}
	batch := []runner.Event{first}
drain:
	for len(batch) < max {
		select {
		case e := <-ch:
			batch = append(batch, e)
		default:
			break drain
		}
	}
	return batch
}

func (a *App) handleEvent(ev runner.Event) {
	switch ev.Type {
	case runner.EvLog:
		if ev.Message != "" {
			a.appendLog(ev.Dep, ev.Message)
		}
	case runner.EvPhase:
		if ev.Message != "" {
			a.setDepStatus(ev.Dep, ev.Message)
		}
		if ev.Overall {
			// 总体进度（整批任务或单依赖运行）驱动进度条
			a.progress.SetValue(ev.Progress / 100)
		} else if ev.Progress > 0 {
			a.setDepProgress(ev.Dep, ev.Progress)
		}
	case runner.EvDepDone:
		a.setDepStatus(ev.Dep, "完成")
		a.flushLog() // 依赖完成时立即渲染，避免尾部日志滞留
	case runner.EvDepFail:
		a.setDepStatus(ev.Dep, "失败")
		if ev.Message != "" {
			a.appendLog(ev.Dep, ev.Message)
		}
		a.flushLog()
	case runner.EvRunDone:
		a.setBusy(false)
		a.progress.SetValue(1)
		a.status.SetText("就绪")
		a.appendLog("", "----------------------------------------")
		a.flushLog()
	}
}

func (a *App) setDepStatus(name, s string) {
	a.mu.Lock()
	for i := range a.depsUI {
		if a.depsUI[i].name == name {
			a.depsUI[i].status = s
			break
		}
	}
	a.mu.Unlock()
	a.depList.Refresh()
}

func (a *App) setDepProgress(name string, p float64) {
	a.mu.Lock()
	for i := range a.depsUI {
		if a.depsUI[i].name == name {
			a.depsUI[i].prog = p
			break
		}
	}
	a.mu.Unlock()
}

func (a *App) appendLog(dep, msg string) {
	if a.logText == nil {
		return
	}
	prefix := ""
	if dep != "" {
		prefix = "[" + dep + "] "
	}
	a.logBuf = append(a.logBuf, prefix+msg)
	if len(a.logBuf) > logKeepTotal {
		a.logBuf = append([]string(nil), a.logBuf[len(a.logBuf)-logKeepTotal:]...)
	}
	// 节流 + 只渲染尾部少量行，避免日志越长 RichText 重排越重导致界面卡顿
	if time.Since(a.lastLog) >= logUpdateEvery {
		a.flushLog()
	}
}

// flushLog 立即把缓冲的日志渲染到界面（不受节流限制）。
// 用于任务结束/依赖边界以及手动「刷新日志」按钮，避免最后几行因节流窗口滞留。
func (a *App) flushLog() {
	if a.logText == nil {
		return
	}
	a.lastLog = time.Now()
	display := a.logBuf
	if len(display) > logDisplayMax {
		display = display[len(display)-logDisplayMax:]
	}
	// 白字日志（默认深色主题下 foreground 即白色，替代灰色不可读的禁用态 Entry）
	a.logText.Segments = []widget.RichTextSegment{
		&widget.TextSegment{
			Style: widget.RichTextStyle{ColorName: theme.ColorNameForeground},
			Text:  strings.Join(display, "\n"),
		},
	}
	a.logText.Refresh()
	a.logScroll.ScrollToBottom()
}

// ---------------------------------------------------------------------------
// 动作
// ---------------------------------------------------------------------------

func (a *App) action(dls, builds, force bool) {
	if a.busy {
		return
	}
	if force {
		for _, d := range a.cfg.Dependencies {
			a.run.Reset(d.Name)
		}
	}
	a.setBusy(true)
	ctx, cancel := context.WithCancel(context.Background())
	a.cancel = cancel
	a.appendLog("", fmt.Sprintf("开始任务: 下载=%v 构建=%v 强制=%v", dls, builds, force))
	go a.run.RunAll(ctx, dls, builds)
}

func (a *App) actionDep(dls, builds bool) {
	if a.busy {
		return
	}
	name, ok := a.selectedDep()
	if !ok {
		a.appendLog("", "请先在左侧列表选择一个依赖")
		return
	}
	if d := a.selectedDepCfg(); d != nil && d.Kind == "userpath" {
		if !builds {
			a.appendLog("", name+" 为手动构建依赖，无需下载")
			return
		}
		if a.run.UserPath(d) == "" {
			a.appendLog("", "请先点击「选择路径…」设置 "+name+" 的构建产物目录")
			return
		}
	}
	a.setBusy(true)
	ctx, cancel := context.WithCancel(context.Background())
	a.cancel = cancel
	a.appendLog("", fmt.Sprintf("处理依赖 %s: 下载=%v 构建=%v", name, dls, builds))
	go a.run.RunDep(ctx, name, dls, builds)
}

// chooseDepPath 为选中的 userpath 依赖设置构建产物目录。
func (a *App) chooseDepPath() {
	if a.busy {
		return
	}
	d := a.selectedDepCfg()
	if d == nil || d.Kind != "userpath" {
		a.appendLog("", "请先在左侧列表选择一个手动构建依赖（如 PhysX）")
		return
	}
	a.promptPath("选择 "+d.Name+" 构建产物目录", a.run.UserPath(d), func(p string) {
		a.run.SetUserPath(d.Name, p)
		a.setDepStatus(d.Name, "手动构建 ✓")
		a.appendLog("", "已设置 "+d.Name+" 构建产物目录: "+p)
	})
}

// promptPath 弹出自定义路径输入对话框。
// 不使用 fyne 自带的文件选择对话框：其在本机枚举收藏位置/驱动器时
// 会报 "uri is not listable"、"file in use" 并可能阻塞主线程导致界面卡死。
func (a *App) promptPath(title, current string, onOK func(path string)) {
	entry := widget.NewEntry()
	entry.SetText(current)
	entry.SetPlaceHolder("例如 D:\\PhysX 或 G:\\0\\EnderEngine\\..")

	hint := widget.NewLabel("请输入目录的完整路径（可从资源管理器地址栏复制粘贴），相对路径将相对引擎根目录解析：")
	hint.Wrapping = fyne.TextWrapWord
	errLbl := widget.NewLabel("")
	errLbl.Wrapping = fyne.TextWrapWord
	errLbl.Importance = widget.DangerImportance

	dlg := dialog.NewCustomWithoutButtons(title, container.NewVBox(hint, entry, errLbl), a.win)
	okBtn := widget.NewButton("确定", func() {
		p := strings.TrimSpace(entry.Text)
		if p == "" {
			errLbl.SetText("路径不能为空")
			return
		}
		resolved := p
		if !filepath.IsAbs(p) {
			resolved = filepath.Join(a.run.Root, p)
		}
		if fi, err := os.Stat(resolved); err != nil || !fi.IsDir() {
			errLbl.SetText("路径不存在或不是目录：" + resolved)
			return
		}
		dlg.Hide()
		onOK(p)
	})
	cancelBtn := widget.NewButton("取消", func() { dlg.Hide() })
	entry.OnSubmitted = func(string) { okBtn.OnTapped() }
	dlg.SetButtons([]fyne.CanvasObject{okBtn, cancelBtn})
	dlg.Show()
}

func (a *App) resetDep() {
	if a.busy {
		return
	}
	name, ok := a.selectedDep()
	if !ok {
		a.appendLog("", "请先在左侧列表选择一个依赖")
		return
	}
	a.run.Reset(name)
	a.setDepStatus(name, "已重置")
}

// cleanDep 彻底清理选中依赖的工作产物（构建树/暂存/源码/下载包）。
func (a *App) cleanDep() {
	if a.busy {
		return
	}
	name, ok := a.selectedDep()
	if !ok {
		a.appendLog("", "请先在左侧列表选择一个依赖")
		return
	}
	a.run.Clean(name)
	a.setDepStatus(name, "已清理")
}

func (a *App) selectedDep() (string, bool) {
	a.mu.Lock()
	id := a.selectedID
	a.mu.Unlock()
	if id < 0 || id >= len(a.depsUI) {
		return "", false
	}
	return a.depsUI[id].name, true
}

func (a *App) toolCheck() {
	a.run.Logf("==== 工具链检测 ====")
	a.run.CheckTools()
	a.run.Logf("提示: Fyne 构建需要 MinGW-w64(gcc)；依赖下载/构建需要 git、curl、cmake 与 VS2022")
}

func (a *App) reloadConfig() {
	if a.busy {
		a.appendLog("", "任务运行中，暂不能重新加载配置")
		return
	}
	cfg, err := config.Load(a.cfgPath)
	if err != nil {
		dialog.ShowError(err, a.win)
		return
	}
	a.cfg = cfg
	a.run.Cfg = cfg
	a.run.RefreshTools()
	a.refreshList()
	a.appendLog("", "已重新加载配置: "+a.cfgPath+"（CMake 生成器: "+a.run.GeneratorName()+"）")
}

func (a *App) chooseRoot() {
	if a.busy {
		a.appendLog("", "任务运行中，暂不能切换根目录")
		return
	}
	a.promptPath("选择引擎根目录", a.run.Root, func(root string) {
		a.run.SetRoot(root)
		a.rootLbl.SetText("引擎根目录: " + root)
		a.appendLog("", "引擎根目录: "+root)
	})
}

func (a *App) setBusy(b bool) {
	a.busy = b
	a.btnDownload.Disable()
	a.btnBuild.Disable()
	a.btnAll.Disable()
	a.btnForce.Disable()
	a.btnDepDown.Disable()
	a.btnDepBuild.Disable()
	a.btnDepReset.Disable()
	a.btnDepClean.Disable()
	a.btnDepPath.Disable()
	a.btnStop.Disable()
	if b {
		a.status.SetText("任务运行中…")
		a.btnStop.Enable()
	} else {
		a.status.SetText("就绪")
		for _, btn := range []*widget.Button{a.btnDownload, a.btnBuild, a.btnAll, a.btnForce, a.btnDepDown, a.btnDepBuild, a.btnDepReset, a.btnDepClean} {
			btn.Enable()
		}
		a.updateDepButtons()
	}
}
