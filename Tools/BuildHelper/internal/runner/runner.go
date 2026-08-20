// Package runner 实现依赖的下载、构建与产物布局同步。
package runner

import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"

	"buildhelper/internal/config"
	"buildhelper/internal/toolcheck"
)

// EventType 事件类型。
type EventType int

const (
	EvLog     EventType = iota // 日志行
	EvPhase                    // 阶段/进度变化
	EvDepDone                  // 单个依赖成功
	EvDepFail                  // 单个依赖失败
	EvRunDone                  // 全部任务结束
)

// Event 运行事件。
type Event struct {
	Dep      string
	Type     EventType
	Message  string
	Progress float64 // 0..100（当前依赖）
	Overall  bool    // 该进度事件是否表示整批任务的总体进度（驱动 UI 进度条）
}

// Runner 执行依赖处理。
type Runner struct {
	Cfg  *config.Config
	Root string // 引擎根目录
	Work string // 工作目录（<Root>/Tools/BuildHelper/.work）

	Events chan Event

	mu        sync.Mutex
	statuses  map[string]string
	progs     map[string]float64
	running   bool
	userPaths map[string]string // userpath 依赖的用户构建产物目录（持久化）
	single    bool              // 单依赖运行模式（总体进度 = 该依赖自身进度）
	base      int               // 批处理中已完成的依赖数（用于总体进度）
	total     int               // 批处理依赖总数

	genName string    // 实际使用的 CMake 生成器（自动检测或 deps.yaml 指定）
	needEnv bool      // cmake 不在 PATH（来自 VS 安装），子进程需注入 VS 开发环境
	envOnce sync.Once // VS 开发环境只加载一次
	vsEnv   []string  // VsDevCmd.bat 导出的环境变量
}

// New 创建 Runner。root 为空时自动推断。
func New(cfg *config.Config, root string) *Runner {
	if root == "" {
		root = DefaultRoot()
	}
	r := &Runner{
		Cfg:       cfg,
		Root:      root,
		Work:      filepath.Join(root, "Tools", "BuildHelper", ".work"),
		Events:    make(chan Event, 512),
		statuses:  map[string]string{},
		progs:     map[string]float64{},
		userPaths: map[string]string{},
	}
	r.loadUserPaths()
	r.RefreshTools()
	return r
}

// RefreshTools 解析 cmake 路径与 CMake 生成器（重新加载配置后调用）。
func (r *Runner) RefreshTools() {
	if r.Cfg.Tools.Cmake == "" || r.Cfg.Tools.Cmake == "cmake" {
		p, needEnv := toolcheck.ResolveCMake()
		r.Cfg.Tools.Cmake = p
		r.needEnv = needEnv
	}
	r.genName = r.effectiveGenerator()
}

// effectiveGenerator 返回实际使用的 CMake 生成器：
// deps.yaml 显式指定则优先，否则按最新安装的 VS 版本自动匹配。
func (r *Runner) effectiveGenerator() string {
	if r.Cfg.Engine.Generator != "" {
		return r.Cfg.Engine.Generator
	}
	if _, major, err := toolcheck.FindVS(); err == nil {
		if g := toolcheck.GeneratorFor(major); g != "" {
			return g
		}
	}
	return "Visual Studio 17 2022"
}

// GeneratorName 返回当前使用的 CMake 生成器（UI 展示用）。
func (r *Runner) GeneratorName() string {
	if r.genName == "" {
		r.RefreshTools()
	}
	return r.genName
}

// devEnv 惰性加载 VS 开发环境（等价于 Developer PowerShell 注入的环境）。
func (r *Runner) devEnv() []string {
	r.envOnce.Do(func() {
		vs, _, err := toolcheck.FindVS()
		if err != nil {
			return
		}
		env, err := toolcheck.VsDevEnv(vs)
		if err == nil && len(env) > 0 {
			r.vsEnv = env
		}
	})
	return r.vsEnv
}

// SetRoot 切换引擎根目录。
func (r *Runner) SetRoot(root string) {
	r.Root = root
	r.Work = filepath.Join(root, "Tools", "BuildHelper", ".work")
	r.loadUserPaths()
}

// userPath 返回 userpath 依赖的构建产物目录；优先 deps.yaml 的 path，其次 GUI 设置。
func (r *Runner) userPath(d *config.Dependency) string {
	p := r.userPaths[d.Name]
	if d.Path != "" {
		p = d.Path
	}
	if p == "" {
		return ""
	}
	if !filepath.IsAbs(p) {
		p = filepath.Join(r.Root, p)
	}
	return filepath.Clean(p)
}

// UserPath 查询 userpath 依赖的构建产物目录（UI 使用）。
func (r *Runner) UserPath(d *config.Dependency) string {
	return r.userPath(d)
}

// SetUserPath 设置 userpath 依赖的构建产物目录并持久化。
func (r *Runner) SetUserPath(name, path string) {
	r.mu.Lock()
	r.userPaths[name] = path
	r.mu.Unlock()
	r.saveUserPaths()
}

func (r *Runner) loadUserPaths() {
	data, err := os.ReadFile(filepath.Join(r.Work, "userpaths.json"))
	if err != nil {
		return
	}
	var m map[string]string
	if json.Unmarshal(data, &m) != nil {
		return
	}
	r.mu.Lock()
	for k, v := range m {
		r.userPaths[k] = v
	}
	r.mu.Unlock()
}

func (r *Runner) saveUserPaths() {
	if err := os.MkdirAll(r.Work, 0o755); err != nil {
		return
	}
	r.mu.Lock()
	data, err := json.MarshalIndent(r.userPaths, "", "  ")
	r.mu.Unlock()
	if err != nil {
		return
	}
	_ = os.WriteFile(filepath.Join(r.Work, "userpaths.json"), data, 0o644)
}

// DefaultRoot 根据可执行文件/工作目录推断引擎根目录。
func DefaultRoot() string {
	probe := func(dir string) (string, bool) {
		if filepath.Base(dir) == "BuildHelper" {
			if p := filepath.Dir(dir); filepath.Base(p) == "Tools" {
				return filepath.Dir(p), true
			}
		}
		return "", false
	}
	if exe, err := os.Executable(); err == nil {
		if root, ok := probe(filepath.Dir(exe)); ok {
			return root
		}
	}
	if wd, err := os.Getwd(); err == nil {
		if root, ok := probe(wd); ok {
			return root
		}
		return wd
	}
	return "."
}

// Status 查询依赖状态文本。
func (r *Runner) Status(dep string) string {
	r.mu.Lock()
	defer r.mu.Unlock()
	if s, ok := r.statuses[dep]; ok {
		return s
	}
	return "未处理"
}

// Progress 查询依赖进度。
func (r *Runner) Progress(dep string) float64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.progs[dep]
}

// IsRunning 是否有任务在运行。
func (r *Runner) IsRunning() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.running
}

func (r *Runner) emit(dep string, t EventType, msg string, p float64, overall bool) {
	r.Events <- Event{Dep: dep, Type: t, Message: msg, Progress: p, Overall: overall}
}

func (r *Runner) setStatus(dep, s string) {
	r.mu.Lock()
	r.statuses[dep] = s
	r.mu.Unlock()
	r.emit(dep, EvPhase, s, r.Progress(dep), false)
}

// setProgress 更新单个依赖的进度；单依赖模式下同时转发为总体进度，
// 批处理模式下按 (已完成数 + 当前依赖进度占比) / 总数 计算总体进度。
func (r *Runner) setProgress(dep string, p float64) {
	r.mu.Lock()
	r.progs[dep] = p
	single := r.single
	base, total := r.base, r.total
	r.mu.Unlock()
	r.emit(dep, EvPhase, "", p, false)
	var overall float64
	if single {
		overall = p
	} else if total > 0 {
		overall = (float64(base) + p/100) / float64(total) * 100
	} else {
		return
	}
	r.emit("", EvPhase, "", overall, true)
}

// setOverall 发送整批任务的总体进度事件（驱动 UI 进度条）。
func (r *Runner) setOverall(p float64) {
	r.emit("", EvPhase, "", p, true)
}

func (r *Runner) setSingleMode(v bool) {
	r.mu.Lock()
	r.single = v
	r.mu.Unlock()
}

func (r *Runner) logf(dep, format string, a ...any) {
	r.emit(dep, EvLog, fmt.Sprintf(format, a...), 0, false)
}

// Logf 向日志通道写入一行（不关联具体依赖，供 UI 使用）。
func (r *Runner) Logf(format string, a ...any) {
	r.logf("", format, a...)
}

// CheckTools 检测工具链并把结果写入日志。
func (r *Runner) CheckTools() {
	for _, res := range toolcheck.Check() {
		if res.Found {
			r.logf("", "✔ %s: %s", res.Tool, res.Version)
		} else {
			r.logf("", "✘ %s: 未找到（%s）", res.Tool, res.Detail)
		}
	}
	r.logf("", "CMake 生成器: %s（deps.yaml 的 generator 留空时自动检测 VS 版本）", r.GeneratorName())
	if r.needEnv {
		r.logf("", "cmake 不在 PATH：将使用 VS 安装目录中的 cmake，并为子进程注入 VsDevCmd 开发环境")
	}
}

// RunAll 依次处理全部依赖。dls/builds 控制是否执行下载/构建。
// 进度条按「已完成依赖数 / 总依赖数」驱动（总体进度事件）。
func (r *Runner) RunAll(ctx context.Context, dls, builds bool) {
	if !r.begin() {
		return
	}
	defer r.end()

	r.mu.Lock()
	r.single = false
	r.base = 0
	r.total = len(r.Cfg.Dependencies)
	r.mu.Unlock()
	r.setOverall(0)

	for i := range r.Cfg.Dependencies {
		d := &r.Cfg.Dependencies[i]
		if ctx.Err() != nil {
			r.logf("", "任务已取消（处理到 %s 之前）", d.Name)
			return
		}
		r.processOne(ctx, d, dls, builds)
		r.mu.Lock()
		r.base++
		b, t := r.base, r.total
		r.mu.Unlock()
		if t > 0 {
			r.setOverall(float64(b) / float64(t) * 100)
		}
	}
}

// RunDep 处理单个依赖。进度条按该依赖自身阶段进度驱动。
func (r *Runner) RunDep(ctx context.Context, name string, dls, builds bool) {
	if !r.begin() {
		return
	}
	defer r.end()

	r.setSingleMode(true)
	r.setOverall(0)

	d := r.findDep(name)
	if d == nil {
		r.emit("", EvDepFail, "未找到依赖 "+name, 0, false)
		return
	}
	r.processOne(ctx, d, dls, builds)
	r.setOverall(100)
}

// processOne 处理单个依赖并发出状态/完成/失败事件。
func (r *Runner) processOne(ctx context.Context, d *config.Dependency, dls, builds bool) {
	if d.Kind == "userpath" {
		if r.userPath(d) == "" {
			r.setStatus(d.Name, "待选择路径")
			r.logf(d.Name, "手动构建依赖：请先在 GUI 中为其选择构建产物目录（或填写 deps.yaml 的 path），本次跳过")
			return
		}
		r.setStatus(d.Name, "开始")
		if err := r.Build(ctx, d); err != nil {
			r.setStatus(d.Name, "失败")
			r.emit(d.Name, EvDepFail, "【"+d.Name+"】同步失败: "+err.Error(), 0, false)
		} else {
			r.setStatus(d.Name, "完成")
			r.emit(d.Name, EvDepDone, "【"+d.Name+"】同步完成", 100, false)
		}
		return
	}
	r.setStatus(d.Name, "开始")
	var err error
	if dls {
		err = r.Download(ctx, d)
	}
	if err == nil && builds {
		err = r.Build(ctx, d)
	}
	if err != nil {
		r.setStatus(d.Name, "失败")
		r.emit(d.Name, EvDepFail, "【"+d.Name+"】失败: "+err.Error(), 0, false)
	} else {
		r.setStatus(d.Name, "完成")
		r.emit(d.Name, EvDepDone, "【"+d.Name+"】完成", 100, false)
	}
}

func (r *Runner) begin() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.running {
		return false
	}
	r.running = true
	return true
}

func (r *Runner) end() {
	r.mu.Lock()
	r.running = false
	r.single = false
	r.mu.Unlock()
	r.emit("", EvRunDone, "任务结束", 0, false)
}

func (r *Runner) findDep(name string) *config.Dependency {
	for i := range r.Cfg.Dependencies {
		if r.Cfg.Dependencies[i].Name == name {
			return &r.Cfg.Dependencies[i]
		}
	}
	return nil
}

// Reset 清除某依赖的构建缓存与中间产物（保留源码，便于重新构建）。
func (r *Runner) Reset(name string) {
	d := r.findDep(name)
	if d == nil {
		return
	}
	_ = os.RemoveAll(r.stageRoot(d))
	_ = os.Remove(filepath.Join(r.Work, "build", d.Name, ".built.json"))
	r.setStatus(name, "已重置")
}

// Clean 彻底清理某依赖的全部工作产物：构建树、暂存、源码目录与下载包，下次全量重建。
func (r *Runner) Clean(name string) {
	d := r.findDep(name)
	if d == nil {
		return
	}
	removed := []string{}
	paths := []string{
		filepath.Join(r.Work, "build", d.Name),
		filepath.Join(r.Work, "stage", d.Name),
		r.sourceDir(d),
	}
	for _, p := range paths {
		if exists(p) {
			if err := os.RemoveAll(p); err == nil {
				removed = append(removed, p)
			}
		}
	}
	if d.Kind == "archive" && d.ArchiveFile != "" {
		p := filepath.Join(r.Work, "downloads", d.ArchiveFile)
		if exists(p) {
			if err := os.Remove(p); err == nil {
				removed = append(removed, p)
			}
		}
	}
	r.setStatus(name, "已清理")
	if len(removed) == 0 {
		r.logf(name, "没有可清理的工作产物")
	} else {
		r.logf(name, "已清理: %s", strings.Join(removed, ", "))
	}
}

// ---------------------------------------------------------------------------
// 下载
// ---------------------------------------------------------------------------

func (r *Runner) sourceDir(d *config.Dependency) string {
	return filepath.Join(r.Work, "source", d.Name)
}

func (r *Runner) stageRoot(d *config.Dependency) string {
	return filepath.Join(r.Work, "stage", d.Name)
}

// Download 下载单个依赖。
func (r *Runner) Download(ctx context.Context, d *config.Dependency) error {
	r.setProgress(d.Name, 5)
	switch d.Kind {
	case "git":
		return r.gitFetch(ctx, d)
	case "archive":
		return r.archiveFetch(ctx, d)
	case "userpath":
		r.logf(d.Name, "手动构建依赖，无需下载")
		return nil
	default:
		return fmt.Errorf("未知 kind: %s", d.Kind)
	}
}

func (r *Runner) gitFetch(ctx context.Context, d *config.Dependency) error {
	src := r.sourceDir(d)
	r.setStatus(d.Name, "下载中 (git)")
	if _, err := os.Stat(filepath.Join(src, ".git")); err == nil {
		r.logf(d.Name, "仓库已存在，更新 %s @ %s", d.Name, d.Ref)
		if err := r.runCmd(ctx, d.Name, src, r.Cfg.Tools.Git,
			"-c", "http.connectTimeout=15", "-c", "http.lowSpeedLimit=1024", "-c", "http.lowSpeedTime=30",
			"fetch", "--progress", "--all", "--tags", "--prune"); err != nil {
			return fmt.Errorf("git fetch 失败: %w", err)
		}
	} else {
		// 浅克隆：--depth 1 --single-branch，只拉取目标 ref，加快首次下载。
		// http.connectTimeout / lowSpeed*：网络受限时快速失败，避免无限静默卡住。
		r.logf(d.Name, "git clone %s（浅克隆, ref=%s）", d.URL, d.Ref)
		if err := os.MkdirAll(filepath.Dir(src), 0o755); err != nil {
			return err
		}
		args := []string{
			"-c", "http.connectTimeout=15", "-c", "http.lowSpeedLimit=1024", "-c", "http.lowSpeedTime=30",
			"clone", "--progress", "--depth", "1", "--single-branch",
		}
		if d.Ref != "" {
			args = append(args, "--branch", d.Ref)
		}
		args = append(args, d.URL, src)
		if err := r.runCmd(ctx, d.Name, r.Work, r.Cfg.Tools.Git, args...); err != nil {
			return fmt.Errorf("git clone 失败: %w", err)
		}
	}
	if d.Ref != "" {
		if err := r.runCmd(ctx, d.Name, src, r.Cfg.Tools.Git, "checkout", "--force", d.Ref); err != nil {
			// 浅克隆只保留了单分支；ref 变更时定向获取目标 ref 后重试
			r.logf(d.Name, "checkout %s 失败，尝试定向获取该 ref", d.Ref)
			if err2 := r.runCmd(ctx, d.Name, src, r.Cfg.Tools.Git,
				"-c", "http.connectTimeout=15", "fetch", "origin", d.Ref, "--tags"); err2 != nil {
				return fmt.Errorf("git checkout %s 失败: %v（定向 fetch 也失败: %v）", d.Ref, err, err2)
			}
			if err3 := r.runCmd(ctx, d.Name, src, r.Cfg.Tools.Git, "checkout", "--force", d.Ref); err3 != nil {
				return fmt.Errorf("git checkout %s 失败: %w", d.Ref, err3)
			}
		}
	}
	if d.Submodules {
		r.logf(d.Name, "更新子模块（可能需要较长时间）")
		if err := r.runCmd(ctx, d.Name, src, r.Cfg.Tools.Git, "submodule", "update", "--init", "--recursive"); err != nil {
			return fmt.Errorf("git submodule 失败: %w", err)
		}
	}
	if err := r.fetchFiles(ctx, d, src); err != nil {
		return err
	}
	r.setProgress(d.Name, 25)
	return nil
}

func (r *Runner) archiveFetch(ctx context.Context, d *config.Dependency) error {
	r.setStatus(d.Name, "下载中 (archive)")
	dlDir := filepath.Join(r.Work, "downloads")
	if err := os.MkdirAll(dlDir, 0o755); err != nil {
		return err
	}
	file := d.ArchiveFile
	if file == "" {
		file = filepath.Base(d.URL)
		if file == "" || file == "." || file == "/" {
			return fmt.Errorf("无法从 URL 推断文件名，请在 deps.yaml 的 archiveFile 中指定")
		}
	}
	dest := filepath.Join(dlDir, file)
	if fi, err := os.Stat(dest); err == nil && fi.Size() > 0 {
		r.logf(d.Name, "压缩包已存在，跳过下载: %s", file)
	} else {
		r.logf(d.Name, "curl 下载 %s", d.URL)
		if err := r.runCmd(ctx, d.Name, dlDir, r.Cfg.Tools.Curl,
			"-L", "--fail", "--retry", "3", "--connect-timeout", "15",
			"--speed-limit", "1024", "--speed-time", "30",
			"--ssl-no-revoke", "--progress-bar", "-o", file, d.URL); err != nil {
			r.logf(d.Name, "下载失败。若网络受限，可手动下载该文件到 %s 后重试（已存在将跳过下载）", dest)
			return fmt.Errorf("curl 下载失败: %w", err)
		}
	}
	raw := filepath.Join(r.stageRoot(d), "raw")
	_ = os.RemoveAll(raw)
	if err := os.MkdirAll(raw, 0o755); err != nil {
		return err
	}
	r.logf(d.Name, "解压 %s", file)
	if err := r.runCmd(ctx, d.Name, raw, "tar", "-xf", dest, "-C", raw); err != nil {
		r.logf(d.Name, "tar 解压失败，改用 PowerShell Expand-Archive")
		if err2 := r.runCmd(ctx, d.Name, raw, "powershell", "-NoProfile", "-Command",
			"Expand-Archive -LiteralPath '"+dest+"' -DestinationPath '"+raw+"' -Force"); err2 != nil {
			return fmt.Errorf("解压失败: %v / %v", err, err2)
		}
	}
	if err := r.fetchFiles(ctx, d, r.extractRoot(d)); err != nil {
		return err
	}
	r.setProgress(d.Name, 25)
	return nil
}

// fetchFiles 把 deps.yaml 中 fetchFiles 列出的文件下载到源码目录
// （用于子模块不可用时补充文件，如 fastgltf 的 simdjson 单头文件）。
func (r *Runner) fetchFiles(ctx context.Context, d *config.Dependency, baseDir string) error {
	for _, ff := range d.FetchFiles {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		dest := filepath.Join(baseDir, ff.Dest)
		if fi, err := os.Stat(dest); err == nil && fi.Size() > 0 {
			r.logf(d.Name, "文件已存在，跳过下载: %s", ff.Dest)
			continue
		}
		if err := os.MkdirAll(filepath.Dir(dest), 0o755); err != nil {
			return err
		}
		r.logf(d.Name, "下载 %s → %s", ff.URL, ff.Dest)
		if err := r.runCmd(ctx, d.Name, filepath.Dir(dest), r.Cfg.Tools.Curl,
			"-L", "--fail", "--retry", "3", "--connect-timeout", "15",
			"--ssl-no-revoke", "-sS", "-o", filepath.Base(dest), ff.URL); err != nil {
			return fmt.Errorf("下载 %s 失败: %w", ff.Dest, err)
		}
	}
	return nil
}

// extractRoot 定位 archive 解压后的实际内容根目录。
func (r *Runner) extractRoot(d *config.Dependency) string {
	raw := filepath.Join(r.stageRoot(d), "raw")
	for _, rule := range d.Layout {
		if rule.From != "" && rule.From != "." {
			if _, err := os.Stat(filepath.Join(raw, rule.From)); err == nil {
				return raw
			}
		}
	}
	entries, err := os.ReadDir(raw)
	if err == nil && len(entries) == 1 && entries[0].IsDir() {
		return filepath.Join(raw, entries[0].Name())
	}
	return raw
}

// ---------------------------------------------------------------------------
// 构建
// ---------------------------------------------------------------------------

// Build 构建单个依赖（cmake），或同步头文件/归档类依赖。
func (r *Runner) Build(ctx context.Context, d *config.Dependency) error {
	if d.Build != "cmake" {
		r.setProgress(d.Name, 30)
		r.setStatus(d.Name, "同步中")
		if err := r.syncAll(ctx, d); err != nil {
			return err
		}
		r.setProgress(d.Name, 100)
		return nil
	}

	buildDir := filepath.Join(r.Work, "build", d.Name)
	hash := r.configHash(d)
	if r.stampMatch(buildDir, ".built.json", hash) && r.targetPopulated(d) {
		r.logf(d.Name, "已是最新构建（%s @ %s），跳过", d.Name, d.Ref)
		r.setProgress(d.Name, 100)
		return nil
	}

	srcDir := r.sourceDir(d)
	if d.Kind == "archive" {
		// archive 依赖的源码即解压后的目录
		srcDir = r.extractRoot(d)
	}
	if _, err := os.Stat(srcDir); err != nil {
		return fmt.Errorf("源码目录不存在 %s（请先执行下载）", srcDir)
	}
	cmakeSrc := srcDir
	if d.SourceSubdir != "" {
		cmakeSrc = filepath.Join(srcDir, d.SourceSubdir)
	}
	if _, err := os.Stat(filepath.Join(cmakeSrc, "CMakeLists.txt")); err != nil {
		return fmt.Errorf("未找到 %s 下的 CMakeLists.txt", cmakeSrc)
	}

	if !r.stampMatch(buildDir, ".configured.json", hash) {
		r.setStatus(d.Name, "CMake 配置中")
		if err := os.MkdirAll(buildDir, 0o755); err != nil {
			return err
		}
		// CMAKE_POLICY_VERSION_MINIMUM=3.5: 兼容 CMake 4.x（旧项目 cmake_minimum_required < 3.5 会报错）
		args := []string{"-S", cmakeSrc, "-B", buildDir, "-G", r.genName, "-A", r.Cfg.Engine.Arch,
			"-DCMAKE_POLICY_VERSION_MINIMUM=3.5"}
		if d.InstallMode == "cmake" {
			_ = os.MkdirAll(r.stageRoot(d), 0o755)
			args = append(args, "-DCMAKE_INSTALL_PREFIX="+r.stageRoot(d))
		}
		args = append(args, d.CMakeOptions...)
		if err := r.runCmd(ctx, d.Name, buildDir, r.Cfg.Tools.Cmake, args...); err != nil {
			return fmt.Errorf("cmake 配置失败: %w", err)
		}
		r.writeStamp(buildDir, ".configured.json", hash)
	}

	for i, cfgName := range r.Cfg.Engine.Configs {
		r.setProgress(d.Name, 35+float64(i)*30)
		r.setStatus(d.Name, "构建 "+cfgName)
		if err := r.runCmd(ctx, d.Name, buildDir, r.Cfg.Tools.Cmake,
			"--build", buildDir, "--config", cfgName, "--parallel"); err != nil {
			return fmt.Errorf("cmake 构建 %s 失败: %w", cfgName, err)
		}
		if d.InstallMode == "cmake" {
			r.setStatus(d.Name, "安装 "+cfgName)
			stageCfg := filepath.Join(r.stageRoot(d), cfgName)
			_ = os.RemoveAll(stageCfg)
			_ = os.MkdirAll(stageCfg, 0o755)
			err := r.runCmd(ctx, d.Name, buildDir, r.Cfg.Tools.Cmake,
				"--install", buildDir, "--config", cfgName, "--prefix", stageCfg)
			if err != nil {
				// 兼容旧版 cmake（无 --prefix 参数）：安装到公共前缀
				r.logf(d.Name, "cmake --install --prefix 不可用，回退到公共安装前缀（需要 cmake ≥ 3.21）")
				if err2 := r.runCmd(ctx, d.Name, buildDir, r.Cfg.Tools.Cmake,
					"--install", buildDir, "--config", cfgName); err2 != nil {
					return fmt.Errorf("cmake 安装 %s 失败: %v / %v", cfgName, err, err2)
				}
			}
		}
	}
	r.setProgress(d.Name, 90)
	r.setStatus(d.Name, "同步产物")
	if err := r.syncAll(ctx, d); err != nil {
		return err
	}
	r.writeStamp(buildDir, ".built.json", hash)
	r.setProgress(d.Name, 100)
	return nil
}

// ---------------------------------------------------------------------------
// 布局同步（将产物放置到正确位置 = 配置项目结构）
// ---------------------------------------------------------------------------

func (r *Runner) syncAll(ctx context.Context, d *config.Dependency) error {
	target := filepath.Join(r.Root, d.Target)
	if d.CleanTarget {
		if err := os.RemoveAll(target); err != nil {
			return err
		}
		r.logf(d.Name, "已清空目标目录: %s", target)
	}
	if err := os.MkdirAll(target, 0o755); err != nil {
		return err
	}
	applied := map[string]bool{}
	for _, rule := range d.Layout {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		cfgList := rule.Configs
		for _, c := range cfgList {
			base, ok := r.ruleBase(d, rule, c)
			if !ok {
				r.logf(d.Name, "跳过（找不到 source 目录）: %s", rule.From)
				continue
			}
			src := filepath.Join(base, rule.From)
			if _, err := os.Stat(src); err != nil {
				r.logf(d.Name, "跳过缺失路径: %s", src)
				continue
			}
			dst := filepath.Join(target, rule.To)
			if c == "*" {
				key := rule.From + "→" + rule.To
				if applied[key] {
					continue
				}
				applied[key] = true
			}
			r.logf(d.Name, "同步 %s → %s", src, dst)
			if err := copyTree(src, dst, rule.Pattern); err != nil {
				return fmt.Errorf("同步 %s 失败: %w", d.Name, err)
			}
		}
	}
	return nil
}

// ruleBase 返回 layout 规则适用的源根目录。
func (r *Runner) ruleBase(d *config.Dependency, rule config.LayoutRule, cfgName string) (string, bool) {
	if d.Kind == "userpath" {
		p := r.userPath(d)
		if p == "" {
			return "", false
		}
		return p, true
	}
	src := rule.Source
	if src == "" {
		if d.Kind == "archive" || (d.Build == "cmake" && d.InstallMode == "cmake") {
			src = "stage"
		} else {
			src = "repo"
		}
	}
	switch src {
	case "stage":
		if d.Kind == "archive" && d.Build != "cmake" {
			// 纯 archive 依赖（如 glfw / SteamAudio）：stage 即解压目录
			return r.extractRoot(d), true
		}
		// archive + cmake 构建（如 DiligentEngine）：stage 是 cmake 安装前缀（含 Debug/Release 子目录）
		base := r.stageRoot(d)
		if cfgName != "" && cfgName != "*" {
			p := filepath.Join(base, cfgName)
			if exists(p) {
				return p, true
			}
			if exists(base) {
				// 兼容旧版 cmake 回退到公共前缀的情况
				return base, true
			}
			return p, false
		}
		for _, c := range r.Cfg.Engine.Configs {
			p := filepath.Join(base, c)
			if exists(p) {
				return p, true
			}
		}
		return filepath.Join(base, r.Cfg.Engine.Configs[0]), false
	case "repo":
		if d.Kind == "archive" {
			// archive 依赖的“仓库”即解压后的源码树
			return r.extractRoot(d), true
		}
		return filepath.Join(r.sourceDir(d), d.SourceSubdir), true
	case "build":
		return filepath.Join(r.Work, "build", d.Name), true
	default: // auto
		cands := []string{
			filepath.Join(r.Work, "build", d.Name),
			filepath.Join(r.sourceDir(d), d.SourceSubdir),
		}
		for _, c := range cands {
			if exists(c) {
				return c, true
			}
		}
		return cands[0], false
	}
}

// ---------------------------------------------------------------------------
// 构建缓存戳
// ---------------------------------------------------------------------------

func (r *Runner) configHash(d *config.Dependency) string {
	h := sha256.New()
	fmt.Fprintf(h, "ref=%s|url=%s|file=%s|gen=%s|arch=%s|configs=%v|opts=%v|sub=%s",
		d.Ref, d.URL, d.ArchiveFile, r.genName, r.Cfg.Engine.Arch, r.Cfg.Engine.Configs, d.CMakeOptions, d.SourceSubdir)
	return hex.EncodeToString(h.Sum(nil))
}

func (r *Runner) stampMatch(buildDir, file, hash string) bool {
	data, err := os.ReadFile(filepath.Join(buildDir, file))
	if err != nil {
		return false
	}
	var m map[string]string
	if json.Unmarshal(data, &m) != nil {
		return false
	}
	return m["hash"] == hash
}

func (r *Runner) writeStamp(buildDir, file, hash string) {
	_ = os.MkdirAll(buildDir, 0o755)
	data, _ := json.Marshal(map[string]string{"hash": hash})
	_ = os.WriteFile(filepath.Join(buildDir, file), data, 0o644)
}

func (r *Runner) targetPopulated(d *config.Dependency) bool {
	entries, err := os.ReadDir(filepath.Join(r.Root, d.Target))
	return err == nil && len(entries) > 0
}

// ---------------------------------------------------------------------------
// 命令执行
// ---------------------------------------------------------------------------

func (r *Runner) runCmd(ctx context.Context, dep, dir, name string, args ...string) error {
	cmd := exec.CommandContext(ctx, name, args...)
	cmd.Dir = dir
	// cmake 不在 PATH（来自 VS 安装）时，注入 VsDevCmd 的开发环境
	if r.needEnv {
		if env := r.devEnv(); len(env) > 0 {
			cmd.Env = env
		}
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return err
	}
	if err := cmd.Start(); err != nil {
		return err
	}

	// Windows: 取消时用 taskkill /T 连子进程树一起终止。git clone 等会派生子进程
	// （如 git-remote-https）继承管道句柄，只杀父进程会导致 cmd.Wait() 永久阻塞。
	treeKilled := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			if cmd.Process != nil {
				_ = exec.Command("taskkill", "/T", "/F", "/PID", strconv.Itoa(cmd.Process.Pid)).Run()
			}
		case <-treeKilled:
		}
	}()

	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		r.scanLines(dep, stdout)
	}()
	go func() {
		defer wg.Done()
		r.scanLines(dep, stderr)
	}()
	err = cmd.Wait()
	close(treeKilled)
	wg.Wait()
	if ctx.Err() != nil {
		return ctx.Err()
	}
	return err
}

// scanLines 逐行读取子进程输出；同时按 \r 切分，使 git --progress 的进度更新
// 成为独立日志行（否则会挤在一行里只显示最后一帧）。
func (r *Runner) scanLines(dep string, rd io.Reader) {
	sc := bufio.NewScanner(rd)
	sc.Buffer(make([]byte, 64*1024), 1024*1024)
	sc.Split(func(data []byte, atEOF bool) (advance int, token []byte, err error) {
		if atEOF && len(data) == 0 {
			return 0, nil, nil
		}
		if i := bytes.IndexAny(data, "\r\n"); i >= 0 {
			return i + 1, data[:i], nil
		}
		if atEOF {
			return len(data), data, nil
		}
		return 0, nil, nil
	})
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line != "" {
			r.emit(dep, EvLog, line, 0, false)
		}
	}
}

// ---------------------------------------------------------------------------
// 文件工具
// ---------------------------------------------------------------------------

func exists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

func copyTree(src, dst, pattern string) error {
	fi, err := os.Stat(src)
	if err != nil {
		return err
	}
	if !fi.IsDir() {
		// 单个文件：直接复制到 dst（dst 可以是目标文件名，
		// 例如 fastgltf 的 Debug 库改名 fastgltfd.lib）。
		// 注意: 不能走 WalkDir 分支——那里会跳过根节点导致单文件复制静默失败。
		if pattern != "" && pattern != "*" {
			ok, err := filepath.Match(pattern, filepath.Base(src))
			if err != nil {
				return err
			}
			if !ok {
				return nil
			}
		}
		return copyFile(src, dst)
	}
	return filepath.WalkDir(src, func(p string, de fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if p == src {
			return nil
		}
		rel, err := filepath.Rel(src, p)
		if err != nil {
			return err
		}
		if de.IsDir() {
			if de.Name() == ".git" {
				return filepath.SkipDir
			}
			return os.MkdirAll(filepath.Join(dst, rel), 0o755)
		}
		if pattern != "" && pattern != "*" {
			ok, err := filepath.Match(pattern, filepath.Base(p))
			if err != nil {
				return err
			}
			if !ok {
				return nil
			}
		}
		return copyFile(p, filepath.Join(dst, rel))
	})
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}
