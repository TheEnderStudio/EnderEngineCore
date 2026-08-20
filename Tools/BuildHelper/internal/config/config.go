// Package config 负责加载/保存 BuildHelper 的依赖配置文件 (deps.yaml)。
package config

import (
	"fmt"
	"os"
	"path/filepath"

	"gopkg.in/yaml.v3"
)

// Engine 引擎级配置。
type Engine struct {
	Root      string   `yaml:"root"`      // 引擎根目录；空 = 自动推断
	Generator string   `yaml:"generator"` // CMake 生成器；空 = 按最新 VS 版本自动检测
	Arch      string   `yaml:"arch"`      // CMake -A 架构
	Configs   []string `yaml:"configs"`   // 构建配置，如 Debug / Release
}

// Tools 外部工具名（可带完整路径）。
type Tools struct {
	Git   string `yaml:"git"`
	Curl  string `yaml:"curl"`
	Cmake string `yaml:"cmake"`
}

// LayoutRule 产物放置规则。
type LayoutRule struct {
	Configs []string `yaml:"configs"` // 适用配置；["*"] 表示只执行一次
	From    string   `yaml:"from"`    // 相对 source 根的源路径（文件或目录）
	To      string   `yaml:"to"`      // 相对引擎根目录的目标路径
	Pattern string   `yaml:"pattern"` // 文件名匹配（filepath.Match），空 = 全部
	Source  string   `yaml:"source"`  // stage | repo | build | auto；空 = 按依赖类型推断
}

// FetchFile 下载单个文件到源码目录（用于子模块失效时补充文件，如 fastgltf 的 simdjson）。
type FetchFile struct {
	URL  string `yaml:"url"`  // 文件下载地址
	Dest string `yaml:"dest"` // 相对源码根的目标路径（含文件名）
}

// Dependency 单个依赖。
type Dependency struct {
	Name         string       `yaml:"name"`
	Kind         string       `yaml:"kind"` // git | archive | userpath
	URL          string       `yaml:"url"`
	Ref          string       `yaml:"ref"` // git tag / 分支 / commit
	ArchiveFile  string       `yaml:"archiveFile"`
	Path         string       `yaml:"path"` // userpath：用户构建产物目录（绝对路径，或相对引擎根）
	Submodules   bool         `yaml:"submodules"`
	FetchFiles   []FetchFile  `yaml:"fetchFiles"` // 下载到源码目录的附加文件
	Build        string       `yaml:"build"`      // cmake | none
	SourceSubdir string       `yaml:"sourceSubdir"`
	CMakeOptions []string     `yaml:"cmakeOptions"`
	InstallMode  string       `yaml:"installMode"` // cmake | buildtree
	Target       string       `yaml:"target"`
	CleanTarget  bool         `yaml:"cleanTarget"` // 同步前清空目标目录
	Layout       []LayoutRule `yaml:"layout"`
}

// Config 全部配置。
type Config struct {
	Engine       Engine       `yaml:"engine"`
	Tools        Tools        `yaml:"tools"`
	Dependencies []Dependency `yaml:"dependencies"`
}

// Find 按名字查找依赖配置项。
func (c *Config) Find(name string) *Dependency {
	for i := range c.Dependencies {
		if c.Dependencies[i].Name == name {
			return &c.Dependencies[i]
		}
	}
	return nil
}

// Normalize 填充默认值并做基本校验。
func (c *Config) Normalize() error {
	// Generator 留空表示自动检测（VS 17 → "Visual Studio 17 2022"，VS 18 → "Visual Studio 18 2026"）
	if c.Engine.Arch == "" {
		c.Engine.Arch = "x64"
	}
	if len(c.Engine.Configs) == 0 {
		c.Engine.Configs = []string{"Debug", "Release"}
	}
	if c.Tools.Git == "" {
		c.Tools.Git = "git"
	}
	if c.Tools.Curl == "" {
		c.Tools.Curl = "curl"
	}
	if c.Tools.Cmake == "" {
		c.Tools.Cmake = "cmake"
	}
	seen := map[string]bool{}
	for i := range c.Dependencies {
		d := &c.Dependencies[i]
		if d.Name == "" {
			return fmt.Errorf("存在缺少 name 的依赖项")
		}
		if seen[d.Name] {
			return fmt.Errorf("依赖名重复: %s", d.Name)
		}
		seen[d.Name] = true
		if d.Kind == "" {
			d.Kind = "git"
		}
		switch d.Kind {
		case "git", "archive", "userpath":
		default:
			return fmt.Errorf("依赖 %s: 未知 kind %q（git | archive | userpath）", d.Name, d.Kind)
		}
		if d.Kind == "git" || d.Kind == "archive" {
			if d.URL == "" {
				return fmt.Errorf("依赖 %s: 缺少 url", d.Name)
			}
		}
		if d.Kind == "userpath" {
			d.Build = "none" // userpath 不参与自动下载/构建
		}
		if d.Build == "" {
			d.Build = "none"
		}
		if d.Build != "cmake" && d.Build != "none" {
			return fmt.Errorf("依赖 %s: 未知 build %q", d.Name, d.Build)
		}
		if d.Target == "" {
			return fmt.Errorf("依赖 %s: 缺少 target", d.Name)
		}
		if d.InstallMode == "" {
			d.InstallMode = "cmake"
		}
		if d.InstallMode != "cmake" && d.InstallMode != "buildtree" {
			return fmt.Errorf("依赖 %s: 未知 installMode %q", d.Name, d.InstallMode)
		}
		if d.Ref == "" && d.Kind == "git" {
			d.Ref = "main"
		}
		for j := range d.Layout {
			lr := &d.Layout[j]
			if lr.Pattern == "" {
				lr.Pattern = "*"
			}
			if len(lr.Configs) == 0 {
				lr.Configs = []string{"*"}
			}
		}
		for j := range d.FetchFiles {
			ff := &d.FetchFiles[j]
			if ff.URL == "" {
				return fmt.Errorf("依赖 %s: fetchFiles[%d] 缺少 url", d.Name, j)
			}
			if ff.Dest == "" {
				return fmt.Errorf("依赖 %s: fetchFiles[%d] 缺少 dest", d.Name, j)
			}
		}
	}
	return nil
}

// Load 读取配置文件；文件不存在时写入内置默认配置。
func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			if werr := os.WriteFile(path, []byte(defaultYAML), 0o644); werr != nil {
				return nil, fmt.Errorf("无法创建默认配置 %s: %w", path, werr)
			}
			data = []byte(defaultYAML)
		} else {
			return nil, err
		}
	}
	var cfg Config
	if err := yaml.Unmarshal(data, &cfg); err != nil {
		return nil, fmt.Errorf("解析 %s 失败: %w", path, err)
	}
	if err := cfg.Normalize(); err != nil {
		return nil, fmt.Errorf("配置校验失败: %w", err)
	}
	return &cfg, nil
}

// Save 将配置写回文件（主要用于把默认配置落盘）。
func Save(path string, cfg *Config) error {
	data, err := yaml.Marshal(cfg)
	if err != nil {
		return err
	}
	if dir := filepath.Dir(path); dir != "." && dir != "" {
		_ = os.MkdirAll(dir, 0o755)
	}
	return os.WriteFile(path, data, 0o644)
}

// Default 返回内置默认配置。
func Default() *Config {
	var cfg Config
	if err := yaml.Unmarshal([]byte(defaultYAML), &cfg); err != nil {
		panic("内置默认配置损坏: " + err.Error())
	}
	_ = cfg.Normalize()
	return &cfg
}
